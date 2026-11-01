// arcblock.so — SAM-ON veto for /usr/sbin/eim (32-bit ARM webOS TV).
//
// No Frida (no soft-float devkit; the TV loader rejects hard-float), so the
// hook is a Thumb-2 inline patch (hookthumb.c) with a plain C body, resolved
// via dlsym(RTLD_DEFAULT, mangled) — eim's .dynsym is in the global scope.
//
// The Beam steals audio by re-sending System Audio Mode ON (72:01) after
// yielding; that re-assert flows through handler_setSystemAudioMode, so it
// is the veto point: SAM-ON drops, SAM-OFF and everything else pass.
// Single target (Thumb, in eim .dynsym): handler_setSystemAudioMode 0x96f41.
// Not hooked: the 6-byte GetArc*Status/SetSystemAudioMode fns (an 8-byte
// patch would overwrite the neighbour) and GetSupportARCSoundOut, cut after
// two eim restarts correlated with its hook (and zero veto value).
// C++ by-value args arrive as an invisible reference: hooks take
// (r0=this, r1=arg).
//
// ezinject module ABI: the payload needs `crt_init` (linked CRT objects) ->
// lib_loginit -> lib_preinit -> lib_main; lib_main alone dies with
// INJ_ERR_DLOPEN. Self-hold via dlopen(RTLD_NODELETE) against dlclose.

#include "hookthumb.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#define LOG_PATH "/tmp/arcblock.log"
// Rendezvous with the homebrew app (ARC Sound Override Blocker): the app dir is world-writable
// and visible both in the service jail (as .../applications/org.tigger2014.arcblock
// under its own mounts) and to root on the host. The jailed service can NOT
// touch /home/root (0700) or /var/lib/webosbrew, so config/status live here.
#define APP_DIR "/media/developer/apps/usr/palm/applications/org.tigger2014.arcblock"
#define CFG_PATH APP_DIR "/config.json"
#define STATUS_PATH APP_DIR "/status.json"
#define SELF_PATH "/media/developer/apps/usr/palm/services/org.tigger2014.arcblock.service/bin/arcblock.so"

static int g_logfd = -1;
// Master switch: the app's single toggle (enabled/disabled). Default OFF —
// no config, no hook: the app must explicitly opt in with "enabled":true.
// Sound mode itself is the status signal (read live from prefs per call);
// this flag only gates veto.
static volatile int g_enabled = 0;
static void read_config(void);  // defined below; hook refreshes per call

// --- logging (write-only, keeps errno) ---------------------------------------
static void log_line(const char* fmt, ...) {
  if (g_logfd < 0) return;
  int saved = errno;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    ssize_t w = write(g_logfd, buf, (size_t)n);  // best-effort single write
    (void)w;
  }
  errno = saved;
}

// --- safe in-process readers --------------------------------------------------
// std::vector<unsigned char>: { start, finish, end } (3 words, 32-bit)
static size_t vec_bytes(const void* v, unsigned char* out, size_t cap) {
  if (v == NULL || out == NULL || cap == 0) return 0;
  const uint32_t* w = (const uint32_t*)v;
  const unsigned char* start = *(const unsigned char* const*)w;
  const unsigned char* finish = (const unsigned char*)w[1];
  if (finish < start || start == NULL) return 0;
  size_t n = (size_t)(finish - start);
  size_t m = n > cap ? cap : n;
  for (size_t i = 0; i < m; i++) out[i] = start[i];
  return n;
}

// --- crash forensics -----------------------------------------------------------
// eim has no journal, so log our own tombstone (sig/fault/PC/LR/SP) with only
// async-signal-safe ops, then re-raise (SA_RESETHAND).
static int g_crashfd = -1;

static void crash_hex(char** p, const char* end, unsigned long v) {
  char tmp[16];
  int i = 0;
  const char* hx = "0123456789abcdef";
  if (*p + 2 < end) { *(*p)++ = '0'; *(*p)++ = 'x'; }
  if (v == 0) { if (*p < end) *(*p)++ = '0'; return; }
  while (v != 0 && i < 16) { tmp[i++] = hx[v & 0xFUL]; v >>= 4; }
  while (i > 0 && *p < end) *(*p)++ = tmp[--i];
}

static void crash_handler(int sig, siginfo_t* si, void* ctx) {
  if (g_crashfd >= 0 && ctx != NULL) {
    char buf[192];
    char* p = buf;
    const char* end = buf + sizeof(buf);
    const char* h;
    ucontext_t* uc = (ucontext_t*)ctx;
    unsigned long pc = 0, lr = 0, sp = 0, fa = 0;
#if defined(__arm__)
    pc = (unsigned long)uc->uc_mcontext.arm_pc;
    lr = (unsigned long)uc->uc_mcontext.arm_lr;
    sp = (unsigned long)uc->uc_mcontext.arm_sp;
#endif
    if (si != NULL) fa = (unsigned long)si->si_addr;
    h = "[arcblock] CRASH sig=";
    while (*h && p < end) *p++ = *h++;
    if (sig >= 10 && p < end) *p++ = (char)('0' + sig / 10);
    if (p < end) *p++ = (char)('0' + sig % 10);
    h = " addr=";
    while (*h && p < end) *p++ = *h++;
    crash_hex(&p, end, fa);
    h = " pc=";
    while (*h && p < end) *p++ = *h++;
    crash_hex(&p, end, pc);
    h = " lr=";
    while (*h && p < end) *p++ = *h++;
    crash_hex(&p, end, lr);
    h = " sp=";
    while (*h && p < end) *p++ = *h++;
    crash_hex(&p, end, sp);
    h = " pid=";
    while (*h && p < end) *p++ = *h++;
    {
      int pid = (int)getpid();
      char t[12];
      int i = 0;
      if (pid == 0) { if (p < end) *p++ = '0'; }
      else {
        while (pid > 0 && i < 12) { t[i++] = (char)('0' + pid % 10); pid /= 10; }
        while (i > 0 && p < end) *p++ = t[--i];
      }
    }
    if (p < end) *p++ = '\n';
    write(g_crashfd, buf, (size_t)(p - buf));
  }
  raise(sig);
}

static void install_crash_handler(void) {
  g_crashfd = open("/tmp/arcblock-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

// --- output awareness ------------------------------------------------------------
// Veto applies only when the TV itself is on speakers: current soundOutput is
// read live from the settings prefs file (/var/luna/preferences/sound, first
// "soundOutput" key = current value). If the user picks the Beam
// (external_arc*), SAM-ON passes untouched — free choice both ways.
#define SOUND_PREFS_PATH "/var/luna/preferences/sound"

// 1 if the output value routes through ARC (Beam), else 0. Non-static for tests.
extern "C" int arcblock_out_is_arc(const char* v) {
  if (v == NULL) return 0;
  for (const char* p = v; *p; p++) {
    if ((p[0] == 'a' || p[0] == 'A') &&
        (p[1] == 'r' || p[1] == 'R') &&
        (p[2] == 'c' || p[2] == 'C')) return 1;
  }
  return 0;
}

// 1 = ARC output (allow ON), 0 = TV/other (veto applies), -1 = unreadable
// (fail-safe: treated as non-ARC). Copies the value into out.
static int read_sound_output(char* out, size_t cap) {
  if (cap > 0) out[0] = '\0';
  int fd = open(SOUND_PREFS_PATH, O_RDONLY);
  if (fd < 0) return -1;
  char buf[2048];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = '\0';
  const char* key = "\"soundOutput\":\"";
  char* p = strstr(buf, key);
  if (p == NULL) return -1;
  p += strlen(key);
  size_t len = 0;
  while (p[len] && p[len] != '"' && (size_t)(p - buf) + len < (size_t)n) len++;
  if (p[len] != '"' || len + 1 >= cap) return -1;  // unterminated/absurd
  for (size_t i = 0; i < len; i++) out[i] = p[i];
  out[len] = '\0';
  return arcblock_out_is_arc(out);
}

// --- app-visible status --------------------------------------------------------
// Only the hook knows its own pid, so it publishes status.json (on attach
// and every DROP) for the jailed app service to display. Best-effort.
static void write_status(const char* lastDropHex, const char* out) {
  int fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"hookedPid\":%d,\"soundOutput\":\"%s\",\"lastDrop\":\"%s\"}\n",
    (int)getpid(), out ? out : "?", lastDropHex ? lastDropHex : "");
  if (n > 0) {
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
  }
  close(fd);
}

// --- targets -------------------------------------------------------------------
typedef int (*fn_vec_t)(void*, const void*);  // this + const std::vector*

static void* g_tramp[1] = { NULL };

// SAM-ON detector: on the wire the vector is one status byte (01=ON,
// 00=OFF). The {0x72,0x01} scan covers full frames if they ever arrive.
// Non-static so the qemu test links the real predicate.
extern "C" int arcblock_is_sam_on(const unsigned char* b, unsigned n) {
  if (b == NULL || n == 0) return 0;
  if (n == 1) return b[0] == 0x01;
  for (unsigned i = 0; i + 1 < n; i++)
    if (b[i] == 0x72 && b[i + 1] == 0x01) return 1;
  return 0;
}

static int hook_handler_setSystemAudioMode(void* self, const void* v) {
  read_config();  // threadless refresh (see worker_thread)
  char out[32];
  int arc = read_sound_output(out, sizeof(out));
  unsigned char bytes[16];
  size_t total = vec_bytes(v, bytes, sizeof(bytes));
  // vec_bytes reports full length but fills only the head; SAM needs 1-3 bytes.
  unsigned avail = total < sizeof(bytes) ? (unsigned)total : (unsigned)sizeof(bytes);
  char hex[64];
  hex[0] = '\0';
  size_t h = 0;
  for (size_t i = 0; i < avail && h + 3 < sizeof(hex); i++)
    h += (size_t)snprintf(hex + h, sizeof(hex) - h, "%02x ", bytes[i]);
  int sam_on = arcblock_is_sam_on(bytes, avail);
  // Veto unsolicited ON only on TV speakers; a manual Beam pick (ARC)
  // passes. Unreadable prefs (arc<0) fail towards veto; disabled passes all.
  if (g_enabled && sam_on && arc != 1) {
    log_line("[arcblock] DROP handler_setSystemAudioMode SAM-ON this=%p ceclen=%u first=[%s] out=\"%s\"\n",
             self, (unsigned)total, hex, out);
    write_status(hex, out);
    return 1;  // mimic normal return (movs r0,#1); 0 could trip a caller check.
  }
  log_line("[arcblock] enter handler_setSystemAudioMode this=%p ceclen=%u first=[%s] out=\"%s\" block=%d\n",
           self, (unsigned)total, hex, out, g_enabled);
  // Return type unverified on-device; logged raw, passed through untouched.
  int r = ((fn_vec_t)g_tramp[0])(self, v);
  log_line("[arcblock] leave handler_setSystemAudioMode ret=%d block=%d\n", r, g_enabled);
  return r;
}

typedef struct {
  const char* short_name;
  const char* mangled;
  unsigned expect_st;   // odd (Thumb) st_value from readelf, for on-device self-check
  void* hook_fn;
} Target;

static Target g_targets[] = {
  { "handler_setSystemAudioMode",
    "_ZN8CAdapter26handler_setSystemAudioModeESt6vectorIhSaIhEE",
    0x96f41, (void*)hook_handler_setSystemAudioMode },
};
#define N_TARGETS (sizeof(g_targets) / sizeof(g_targets[0]))

// --- config (re-read on every hook call; no watcher thread in eim) ----------------
static void read_config(void) {
  int fd = open(CFG_PATH, O_RDONLY);
  if (fd < 0) return;
  char buf[256];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';
  // The ONLY supported key. Explicit "enabled":true opts in; anything
  // else — including a missing file — stays OFF. No self-enabling.
  int on = (strstr(buf, "\"enabled\":true") != NULL) ? 1 : 0;
  if (on != g_enabled) {
    g_enabled = on;
    log_line("[arcblock] config enabled=%d (SAM-ON veto %s)\n", on,
             on ? "ENFORCED" : "off");
  }
}

// --- eim base (for the offset self-check) -----------------------------------------
static unsigned long eim_base_from_maps(void) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512];
  unsigned long base = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "/eim") && strstr(line, "r-xp")) {
      base = strtoul(line, NULL, 16);
      break;
    }
  }
  fclose(f);
  return base;
}

// --- worker: resolve via dlsym + hook all targets ----------------------------------
static void* worker_thread(void* arg) {
  (void)arg;
  g_logfd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
  log_line("[arcblock] loaded pid=%d phase=2-sam-veto gum-free\n", (int)getpid());
  install_crash_handler();
  read_config();  // initial enabled value

  // Never unloadable: survive ezinject dlclose even if persist flag is missed.
  dlopen(SELF_PATH, RTLD_NOW | RTLD_NODELETE);

  dlerror();  // clear
  unsigned long base = eim_base_from_maps();
  unsigned attached = 0;
  for (unsigned i = 0; i < N_TARGETS; i++) {
    void* sym = dlsym(RTLD_DEFAULT, g_targets[i].mangled);
    const char* err = dlerror();
    if (!sym || err) {
      log_line("[arcblock] %s: dlsym FAILED (%s), skipping\n",
               g_targets[i].short_name, err ? err : "null");
      continue;
    }
    unsigned long even = (unsigned long)sym & ~1UL;
    const char* match = "?";
    if (base) {
      unsigned off = (unsigned)(even - base);
      match = (off == (g_targets[i].expect_st & ~1u)) ? "MATCH" : "MISMATCH";
      log_line("[arcblock] %s: dlsym=%p off=0x%x expect=0x%x %s\n",
               g_targets[i].short_name, sym, off,
               g_targets[i].expect_st & ~1u, match);
    } else {
      log_line("[arcblock] %s: dlsym=%p (no maps base for check)\n",
               g_targets[i].short_name, sym);
    }
    // Trampolines hold Thumb code: call them through an odd pointer so the
    // ARM-mode hooks above enter via BLX interworking (see hookthumb.h).
    void* tr = NULL;
    int rc = thumb_hook((void*)even, g_targets[i].hook_fn, &tr);
    if (rc == 0) {
      attached++;
      g_tramp[i] = (void*)((uintptr_t)tr | 1u);
      log_line("[arcblock] attached %s @ %p tramp=%p\n",
               g_targets[i].short_name, (void*)even, g_tramp[i]);
    } else {
      log_line("[arcblock] hook FAILED %s rc=%d failoff=%d\n",
               g_targets[i].short_name, rc, thumb_fail_offset());
    }
  }
  log_line("[arcblock] attached %u/%u targets; no persistent threads, config re-read per call\n",
           attached, (unsigned)N_TARGETS);
  { char so[32]; read_sound_output(so, sizeof(so)); write_status(NULL, so); }
  return NULL;  // worker exits: no extra threads left in eim
}

// --- ezinject entry points (unmangled; called by the linked-in CRT) ----------
extern "C" {
struct injcode_user { bool persist; };
struct log_config;
int lib_preinit(struct injcode_user* user) { user->persist = true; return 0; }
int lib_loginit(struct log_config* cfg) { (void)cfg; return -1; }
// NOTE: no lib_unload_prepare stub — the CRT objects already define it.
int lib_main(int argc, char* argv[]) {
  (void)argc; (void)argv;
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, worker_thread, NULL) != 0) return 1;
  pthread_attr_destroy(&at);
  return 0;
}
}
