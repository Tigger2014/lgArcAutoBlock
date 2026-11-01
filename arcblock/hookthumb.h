/* hookthumb.h — minimal Thumb-2 inline-hook engine for 32-bit ARM.
 *
 * Installs an 8-byte absolute jump (LDR.W PC,[PC,#0] + address, no registers
 * touched) over a target's prologue and builds a trampoline that replays the
 * displaced instructions (PC-relative ones rewritten to absolute form) before
 * jumping back. Hook functions are ordinary C with the target's prototype;
 * the patch is a tail jump, so argument registers arrive intact.
 *
 * Supported in the patched window: verbatim ALU/loads/stores, 16/32-bit LDR
 * literals (rewritten), ADR (materialized), B/BL/CBZ (absolute sequences),
 * PLD (dropped to NOP). Anything else (SVC/UDF/TBB/TBH/IT/computed jumps)
 * refuses loudly with a negative code — never a torn patch.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success (*tramp_out = executable trampoline).
 * Negative: -1 bad alignment, -2 unsupported insn @offset (see log),
 * -3 mprotect failed, -4 mmap failed. */
int thumb_hook(void* target_even, void* hook_fn, void** tramp_out);

/* Last refusal offset (valid after a -2 return), for logging. */
int thumb_fail_offset(void);

#ifdef __cplusplus
}
#endif
