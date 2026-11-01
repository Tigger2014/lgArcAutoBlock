/* hookthumb.c — Thumb-2 inline-hook engine (see hookthumb.h).
 *
 * Patch (requires target 4-byte aligned, verified on the eim target):
 *   at P:    DF F8 00 F0 = LDR.W PC, [PC, #0]   (base = Align(P+4,4) = P+4)
 *   at P+4:  <hook_fn & ~1>                      (bit0 clear: enter ARM mode)
 * Touches no registers, so r0-r3 arrive at the hook intact. Hooks are
 * ordinary ARM-mode C functions (our build flags); the trampoline holds
 * Thumb code and MUST be called through an odd pointer (BLX interworking),
 * e.g. ((fn)((uintptr_t)tramp | 1))(args). The jump-back slot keeps bit0
 * set (targets are Thumb).
 *
 * Trampoline layout: [rewritten prologue][LDR.W PC,[PC,#0]][target+copied|1].
 * PC-relative sources are rewritten to absolute form so the trampoline can
 * live anywhere (no ±4KB/±16MB range assumptions: the TV maps payloads GBs
 * away from eim, as the ezinject logs showed).
 */
#include "hookthumb.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>

#define PATCH_LEN 8  /* 4-byte LDR.W + 4-byte slot */

static int g_fail_off = 0;
int thumb_fail_offset(void) { return g_fail_off; }

/* ---- byte helpers (little-endian) ---- */
static uint16_t RH(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t RW(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void W16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void W32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 32-bit first halfword iff top 5 bits >= 0b11101. */
static int is32(uint16_t hw) { return (hw & 0xF800u) >= 0xE800u; }

/* ---- emitters into trampoline (return bytes written) ---- */

/* LDR.W PC,[PC,#0] + absolute slot (the universal absolute jump).
 * The slot must sit exactly at Align(pos+4,4), so pad with NOPs until the
 * jump sits 4-aligned (trampoline base is page-aligned, so o&3 == pos&3).
 * Takes and updates *o (buffer offset). */
static void emit_abs_jump(uint8_t* tr, int* o, uint32_t dest_thumb) {
    while (*o & 3) { W16(tr + *o, 0xBF00u); *o += 2; } /* NOP pad */
    W16(tr + *o, 0xF8DFu); W16(tr + *o + 2, 0xF000u);
    W32(tr + *o + 4, dest_thumb | 1u);
    *o += 8;
}

/* MOVW/MOVT pair materializing abs32 into rd (rd <= r12).
 * NOTE: 32-bit Thumb-2 words are stored HIGH halfword first, so emit each
 * word as two W16s, high half first — a plain W32 silently swaps them and
 * the CPU decodes garbage. */
static int emit_movw_movt(uint8_t* d, uint32_t rd, uint32_t abs) {
    uint32_t lo = abs & 0xFFFFu, hi = (abs >> 16) & 0xFFFFu;
    uint32_t movw = 0xF2400000u | ((lo & 0xF000u) << 4) |
                    (((lo >> 11) & 1u) << 26) | (((lo >> 8) & 7u) << 12) |
                    ((rd & 0xFu) << 8) | (lo & 0xFFu);
    uint32_t movt = 0xF2C00000u | ((hi & 0xF000u) << 4) |
                    (((hi >> 11) & 1u) << 26) | (((hi >> 8) & 7u) << 12) |
                    ((rd & 0xFu) << 8) | (hi & 0xFFu);
    W16(d, (uint16_t)(movw >> 16)); W16(d + 2, (uint16_t)movw);
    W16(d + 4, (uint16_t)(movt >> 16)); W16(d + 6, (uint16_t)movt);
    return 8;
}

/* Rewrite of "LDR rt, [PC, #off]" (16- or 32-bit): rt = *(abs). */
static int emit_lit_load(uint8_t* d, uint32_t rt, uint32_t abs) {
    int n = emit_movw_movt(d, rt, abs);
    /* LDR rt,[rt,#0] (T1): 01101 00000 Rn Rt */
    W16(d + n, (uint16_t)(0x6800u | (rt << 3) | rt));
    return n + 2;
}

/* Rewrite of ADR rd, pc_rel (16- or 32-bit): rd = abs. */
static int emit_adr(uint8_t* d, uint32_t rd, uint32_t abs) {
    return emit_movw_movt(d, rd, abs);
}

/* Rewrite of "BL dest": R12=dest|1; BLX R12 (R12 is ABI scratch across
 * calls; bit0 set keeps the target in Thumb mode). */
static int emit_bl(uint8_t* d, uint32_t dest) {
    int n = emit_movw_movt(d, 12, dest | 1u);
    W16(d + n, 0x47E0u); /* BLX r12: 0100 0111 1100 0000 */
    return n + 2;
}

/* Rewrite of conditional branch "B<cond> dest": B<inv> skip; abs-jump.
 * Skip offset is computed from the final layout (pad + jump). */
static void emit_bcond(uint8_t* tr, int* o, uint32_t cond, uint32_t dest) {
    int o0 = *o;
    *o += 2; /* reserve the BNE halfword; patched below */
    emit_abs_jump(tr, o, dest);
    { /* B<inv> from o0 to *o: (skip - (o0+4)) / 2 halfwords. */
        uint32_t off = (uint32_t)(*o - (o0 + 4)) / 2u;
        W16(tr + o0, (uint16_t)(0xD000u | ((cond ^ 1u) << 8) | (off & 0xFFu)));
    }
}

/* ---- decoder: returns insn length, fills *w (full 32-bit word for 32-bit
 * insns), or -1 for refuse. Handles only what the engine rewrites; the
 * caller treats K_COPY* verbatim. ---- */
typedef enum {
    K_COPY2, K_COPY4,
    K_LIT16, K_ADR16, K_BCOND16, K_B16, K_CBZ,
    K_BL, K_LIT32, K_PLD, K_REFUSE
} kind_t;

static int need32(uint16_t hw0, uint16_t hw1, kind_t* k, uint32_t* dest,
                  uint32_t pc, uint32_t* aux /*rt/rd/rn+flags*/) {
    /* BL pair only: hw0 = 11110S imm10, hw1 = 11 J1 1 J2 imm11.
     * Everything else in the 11110 group (BLX, B.W, MRS/MSR, ...) refuses:
     * a wrong call-vs-jump rewrite would corrupt LR. */
    if ((hw0 & 0xF800u) == 0xF000u) {
        if ((hw1 & 0xD000u) == 0xD000u) {
            int32_t S = (hw0 >> 10) & 1;
            int32_t J1 = (hw1 >> 13) & 1, J2 = (hw1 >> 11) & 1;
            int32_t I1 = ~(J1 ^ S) & 1, I2 = ~(J2 ^ S) & 1;
            int32_t off = (S ? (int32_t)0xFF000000 : 0) |
                          (I1 << 23) | (I2 << 22) |
                          ((hw0 & 0x3FF) << 12) | ((hw1 & 0x7FF) << 1);
            *k = K_BL; *dest = (uint32_t)((int32_t)(pc + 4) + off); return 4;
        }
        *k = K_REFUSE; return 4;
    }    /* T3 LDR.W (immediate): hw0 0xF8Dx = ADD, 0xF85x = SUBTRACT (both
     * disassemble as ldr.w with signed [pc, #imm]). Only Rn==15 (PC-
     * relative literal) is rewritten; Rn-relative forms copy verbatim. */
    if ((hw0 & 0xFFF0u) == 0xF8D0u || (hw0 & 0xFFF0u) == 0xF850u) {
        uint32_t rn = hw0 & 0xFu, rt = (hw1 >> 12) & 0xFu;
        uint32_t imm = hw1 & 0xFFFu;
        uint32_t base = (pc + 4) & ~3u;
        if (rn != 15u) { *k = K_COPY4; return 4; }
        if (rt == 15u) { *k = K_REFUSE; return 4; } /* LDR PC,...: computed */
        *k = K_LIT32; *aux = rt;
        *dest = ((hw0 & 0xFFF0u) == 0xF8D0u) ? base + imm : base - imm;
        return 4;
    }
    /* MOVW/MOVT (f245/f2c1): pure immediates, no PC input, copy verbatim. */
    if ((hw0 & 0xFF00u) == 0xF240u || (hw0 & 0xFF00u) == 0xF2C0u) {
        *k = K_COPY4; return 4;
    }
    /* PLD/PLI: prefetch hints, safe to drop (NOP). No mask overlap. */
    if ((hw0 & 0xFF70u) == 0xF810u || (hw0 & 0xFF70u) == 0xF830u ||
        (hw0 & 0xFF70u) == 0xF910u) {
        *k = K_PLD; return 4;
    }
    /* TBB/TBH (computed jumps): cannot relocate. */
    if ((hw0 & 0xFFF0u) == 0xE8D0u) { *k = K_REFUSE; return 4; }
    /* Default: refuse. The target window has no 32-bit insns, so anything
     * unrecognized skips loudly instead of risking a wrong rewrite. Extend
     * the whitelist only with gas-verified samples. */
    *k = K_REFUSE; return 4;
}

static int decode(const uint8_t* p, uint32_t pc, kind_t* k, uint32_t* dest,
                  uint32_t* aux) {
    uint16_t hw = RH(p);
    *dest = 0; *aux = 0;
    if (!is32(hw)) {
        if ((hw & 0xF800u) == 0x4800u) {           /* LDR (literal) T1 */
            uint32_t rt = (hw >> 8) & 7u;
            *k = K_LIT16; *aux = rt;
            *dest = ((pc + 4) & ~3u) + ((hw & 0xFFu) * 4u);
            return 2;
        }
        if ((hw & 0xF800u) == 0xA000u) {           /* ADR T1 */
            uint32_t rd = (hw >> 8) & 7u;
            *k = K_ADR16; *aux = rd;
            *dest = ((pc + 4) & ~3u) + ((hw & 0xFFu) * 4u);
            return 2;
        }
        if ((hw & 0xF000u) == 0xD000u) {           /* B<cc> T1 / SVC / UDF */
            uint32_t cond = (hw >> 8) & 0xFu;
            if (cond >= 0xEu) { *k = K_REFUSE; return 2; } /* UDF/SVC */
            int8_t i8 = (int8_t)(hw & 0xFFu);
            *k = K_BCOND16; *aux = cond;
            *dest = (uint32_t)((int32_t)(pc + 4) + ((int32_t)i8 * 2));
            return 2;
        }
        if ((hw & 0xF800u) == 0xE000u) {           /* B T2 */
            int32_t off = (hw & 0x7FFu);
            if (off & 0x400) off |= ~0x7FF;
            *k = K_B16;
            *dest = (uint32_t)((int32_t)(pc + 4) + off * 2);
            return 2;
        }
        if ((hw & 0xFF00u) == 0xB100u || (hw & 0xFF00u) == 0xB900u) {
            /* CBZ/CBNZ: refuse (a wrong field split corrupts control flow). */
            *k = K_REFUSE; return 2;
        }
        if ((hw & 0xFF00u) == 0xBF00u) {
            if (hw == 0xBF00u) { *k = K_COPY2; return 2; } /* NOP */
            *k = K_REFUSE; return 2;                        /* IT block */
        }
        *k = K_COPY2; return 2;
    }
    return need32(hw, RH(p + 2), k, dest, pc, aux);
}

/* ---- main entry ---- */

int thumb_hook(void* target_even, void* hook_fn, void** tramp_out) {
    uint8_t* T = (uint8_t*)target_even;
    uint32_t base = (uint32_t)(uintptr_t)T;
    *tramp_out = NULL;
    g_fail_off = 0;

    if ((base & 1u) || (base & 3u)) return -1; /* must be even + 4-aligned */

    /* 1. Decode source window: grow until >= PATCH_LEN on an insn boundary. */
    kind_t ks[8]; uint32_t dests[8], auxs[8]; int lens[8], n = 0, srclen = 0;
    while (srclen < PATCH_LEN && n < 8) {
        uint32_t pc = base + (uint32_t)srclen;
        int L = decode(T + srclen, pc, &ks[n], &dests[n], &auxs[n]);
        if (L < 0 || ks[n] == K_REFUSE) { g_fail_off = srclen; return -2; }
        lens[n] = L; srclen += L; n++;
    }


    /* 2. Emit trampoline. */
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    uint8_t* tr = (uint8_t*)mmap(NULL, (size_t)pagesz,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) return -4;
    /* Emit in order; source offsets recomputed. */
    int o = 0; {
        int soff = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t* s = T + soff;
            switch (ks[i]) {
            case K_COPY2: memcpy(tr + o, s, 2); o += 2; break;
            case K_COPY4: memcpy(tr + o, s, 4); o += 4; break;
            case K_LIT16: case K_LIT32:
                o += emit_lit_load(tr + o, auxs[i], dests[i]); break;
            case K_ADR16:
                o += emit_adr(tr + o, auxs[i], dests[i]); break;
            case K_BL: o += emit_bl(tr + o, dests[i]); break;
            case K_BCOND16:
                emit_bcond(tr, &o, auxs[i], dests[i]); break;
            case K_B16: emit_abs_jump(tr, &o, dests[i]); break;
            case K_CBZ: /* unreachable: decode refuses CBZ (loud skip) */
                munmap(tr, (size_t)pagesz); g_fail_off = soff; return -2;
            case K_PLD: W16(tr + o, 0xBF00u); o += 2; break; /* NOP */
            default: munmap(tr, (size_t)pagesz); g_fail_off = soff; return -2;
            }
            soff += lens[i];
        }
    }
    emit_abs_jump(tr, &o, base + (uint32_t)srclen);

    if (mprotect(tr, (size_t)pagesz, PROT_READ | PROT_EXEC) != 0) {
        munmap(tr, (size_t)pagesz); return -4;
    }
    __builtin___clear_cache((char*)tr, (char*)(tr + o));

    /* 3. Patch the target: slot first, then the jump insn. */
    {
        uint32_t start = base & ~((uint32_t)pagesz - 1u);
        uint32_t end = (base + PATCH_LEN + (uint32_t)pagesz - 1u) &
                       ~((uint32_t)pagesz - 1u);
        if (mprotect((void*)(uintptr_t)start, (size_t)(end - start),
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            { munmap(tr, (size_t)pagesz); return -3; }
        uint8_t patch[PATCH_LEN];
        W16(patch, 0xF8DFu); W16(patch + 2, 0xF000u);
        /* Even slot: the Thumb patch branches to an ARM-mode hook. */
        W32(patch + 4, ((uint32_t)(uintptr_t)hook_fn) & ~1u);
        memcpy(T + 4, patch + 4, 4);   /* slot before insn */
        memcpy(T, patch, 4);
        __builtin___clear_cache((char*)T, (char*)(T + PATCH_LEN));
        /* restore original protection (assume r-x text) */
        mprotect((void*)(uintptr_t)start, (size_t)(end - start),
                 PROT_READ | PROT_EXEC);
    }

    *tramp_out = tr;
    return 0;
}
