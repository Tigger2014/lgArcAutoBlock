/* test_hook.c — qemu-arm functional test for hookthumb.c.
 * Hooks each mock, calls through the patch, and asserts on VALUES (literal
 * loads, BL results, branch directions). A wrong encoding anywhere fails
 * loudly here instead of on the TV.
 */
#include "hookthumb.h"

#include <stdint.h>
#include <stdio.h>

/* ARM function pointers to Thumb code are odd (bit0 = Thumb mode); the
 * engine wants the even patch address, exactly as arcblock.cpp masks the
 * dlsym results. Hooks are ARM-mode C: the engine branches to them with
 * bit0 clear. Trampolines hold Thumb code and must be called through an
 * odd pointer (BLX interworking). */
#define TGT(f) ((void*)((uintptr_t)(f) & ~(uintptr_t)1))
#define ODD(p) ((void*)((uintptr_t)(p) | (uintptr_t)1))

extern int mock_add(int a, int b);
extern int mock_branch(int x);
extern int mock_bl(int x);
extern int mock_litw(void);
extern int mock_adr(void);
extern int mock_lit16(int a);
extern int mock_refuse(int x);

int arcblock_is_sam_on(const unsigned char* b, unsigned n);
int arcblock_out_is_arc(const char* v);

static int (*tr_add)(int, int);
static int hook_add(int a, int b) { return tr_add(a, b) + 1000; }

static int (*tr_branch)(int);
static int hook_branch(int x) { return tr_branch(x); }

static int (*tr_bl)(int);
static int hook_bl(int x) { return tr_bl(x); }

static int (*tr_litw)(void);
static int hook_litw(void) { return tr_litw(); }

static int (*tr_adr)(void);
static int hook_adr(void) { return tr_adr(); }

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main(void) {
    void* tr = NULL;
    int rc;

    rc = thumb_hook(TGT(mock_add), (void*)hook_add, &tr);
    CHECK(rc == 0); tr_add = ODD(tr);
    { int (*direct)(int,int) = (int(*)(int,int))ODD(tr);
      CHECK(direct(1, 2) == 103); }          /* trampoline alone */
    CHECK(mock_add(1, 2) == 1103);           /* 1+2+100 orig, +1000 hook */

    rc = thumb_hook(TGT(mock_branch), (void*)hook_branch, &tr);
    CHECK(rc == 0); tr_branch = ODD(tr);
    CHECK(mock_branch(0) == 0);
    CHECK(mock_branch(5) == 1);

    rc = thumb_hook(TGT(mock_bl), (void*)hook_bl, &tr);
    CHECK(rc == 0); tr_bl = ODD(tr);
    CHECK(mock_bl(41) == 43);

    rc = thumb_hook(TGT(mock_litw), (void*)hook_litw, &tr);
    CHECK(rc == 0); tr_litw = ODD(tr);
    CHECK(mock_litw() == 0x1234);

    rc = thumb_hook(TGT(mock_adr), (void*)hook_adr, &tr);
    CHECK(rc == 0); tr_adr = ODD(tr);
    CHECK(mock_adr() == 0x5678);

    rc = thumb_hook(TGT(mock_lit16), (void*)hook_add, &tr);
    CHECK(rc == 0); tr_add = ODD(tr);
    CHECK(mock_lit16(10) == 10 + 0xA5 + 1000);

    /* SAM-ON veto predicate (real code from arcblock.cpp).
     * On-device truth: single status byte, 01=ON / 00=OFF. */
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x01}, 1) == 1);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x00}, 1) == 0);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x72}, 1) == 0);
    CHECK(arcblock_is_sam_on(NULL, 0) == 0);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x5f, 0x72, 0x01}, 3) == 1);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x50, 0x72, 0x01}, 3) == 1);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x5f, 0x72, 0x00}, 3) == 0);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x50, 0x72, 0x00}, 3) == 0);
    CHECK(arcblock_is_sam_on((const unsigned char[]){0x72, 0x01}, 2) == 1);

    /* Output awareness: ARC outputs allow ON, TV outputs veto. */
    CHECK(arcblock_out_is_arc("external_arc") == 1);
    CHECK(arcblock_out_is_arc("external_arc_sound_mode_sync") == 1);
    CHECK(arcblock_out_is_arc("tv_speaker_external_arc") == 1);
    CHECK(arcblock_out_is_arc("tv_speaker") == 0);
    CHECK(arcblock_out_is_arc("alsa") == 0);
    CHECK(arcblock_out_is_arc(NULL) == 0);

    /* CBZ: loud refusal, function executes untouched. */
    rc = thumb_hook(TGT(mock_refuse), (void*)hook_branch, &tr);
    CHECK(rc == -2);
    CHECK(mock_refuse(0) == 0);
    CHECK(mock_refuse(5) == 7);

    printf("ALL HOOK TESTS PASSED\n");
    return 0;
}
