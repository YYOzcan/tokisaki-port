/*
 * Tokisaki Port -- Killzone 2 (BCUS98116) boot runner.
 *
 * Boots the lifted EBOOT through ps3recomp's PPU scaffold: flat 4 GB guest VM,
 * ELF image, lifted function table, HLE NID handlers, lv2 syscalls, a frame
 * clock that drives vblank/flip and the RSX FIFO, then the entry OPD.
 *
 * Derived from two MIT-licensed files (license texts: THIRD_PARTY_NOTICES.md,
 * details: CREDITS.md):
 *   - ps3recomp templates/project/main.cpp
 *       Copyright (c) 2026 Ned Heller / sp00nznet
 *     boot order, VFS root derivation, guest-callback hook, Windows VM path.
 *   - Kurtisone/twistedmetal, branch linux/boot, src/boot_main.cpp
 *       (by Kurtisone; repository licensed MIT, Copyright (c) 2026 Ned Heller / sp00nznet)
 *     the POSIX frame clock, the 256 MB host stack for the guest main thread,
 *     the Vulkan/null backend pick and the MAP_NORESERVE guest space.
 * Added here for Killzone 2: PARAM.SFO title id, KZ2_PEEK / KZ2_DUMPAT
 * memory probes, the POSIX crash report and the ftrace main-thread mark.
 *
 * About the "4 GB guest VM": the PPU addresses memory with 32-bit effective
 * addresses, so every guest pointer is an offset into one host reservation
 * the size of that space. It is a model of the address space (main memory at
 * 0x00010000, RSX-mapped IO, the stacks at 0xD0000000, ...), not a claim that
 * the console has 4 GB of RAM -- it has 256 MB XDR + 256 MB GDDR3. RPCS3 and
 * ps3recomp both work this way. The kernel backs only the pages the title
 * touches.
 *
 * Environment:
 *   PS3_VFS_ROOT   directory that holds PS3_GAME (default: derived from the ELF path)
 *   PS3_HDD0_ROOT  host directory /dev_hdd0 maps into
 *   PS3_DEV_FLASH  firmware dev_flash tree (fonts, sys modules)
 *   KZ2_RSX=null   use the headless null backend in a Vulkan build
 *   KZ2_RSX_W/H    backbuffer size (default 1280x720)
 */
#include "ppu_recomp.h"
#include "win32_compat.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#else
#include <timeapi.h>
#endif

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
void     lv2_init_syscalls(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
void     ps3_load_prx_modules(void);
void     kz2_hle_register(void);              /* src/kz2_hle.cpp */
void     cellGame_init_from_paramsfo(const char* sfo_path);
void     kz2_ftrace_mark_main(void);              /* src/kz2_ftrace.cpp */

extern const char* ppu_vfs_root;
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;          /* GCM labels / control register / offset tables */

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

void cellGcm_request_tick(void);
int  cellGcm_take_flip_pending_synced(void);
extern volatile unsigned g_spurs_pm_cur_wid, g_spurs_pm_cur_lane;   /* spurs_policy.c */
extern volatile unsigned long long g_spurs_pm_cur_since;
extern volatile uint32_t g_spurs_last_task_arg[4];                   /* cellSpurs.c */
uint32_t cellGcm_fifo_walker_get(void);                              /* cellGcmSys.c */
unsigned long long ps3_ms_now(void);
void cellGcm_rsx_process_fifo(void);

int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
void rsx_null_backend_present(void);
int  rsx_null_backend_pump_messages(void);
#if defined(PS3RECOMP_RSX_VULKAN)
int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
void rsx_vulkan_backend_present(void);
int  rsx_vulkan_backend_pump_messages(void);
#endif
#if defined(_WIN32)
int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
void rsx_d3d12_backend_present(void);
int  rsx_d3d12_backend_pump_messages(void);
#endif
}

#define VM_SIZE     0x100010000ull     /* full 32-bit guest space + 64 KB guard */
#define STACK_TOP   0x0FF00000u        /* main guest thread stack, below 0x10000000 */
#define MAIN_HOST_STACK_BYTES (256u * 1024u * 1024u)
#define KZ2_TITLE   "Killzone 2 (Tokisaki Port)"

/* ---------------------------------------------------------------------------
 * Frame counter (read by the runtime's diagnostics)
 * -----------------------------------------------------------------------*/
static std::atomic<unsigned> g_frames_presented{0};

extern "C" unsigned ppu_boot_frames_presented(void)
{
    return g_frames_presented.load();
}

/* ---------------------------------------------------------------------------
 * RSX backend selection
 * -----------------------------------------------------------------------*/
struct RsxBackend {
    const char* name;
    int  (*init)(uint32_t, uint32_t, const char*);
    void (*present)(void);
    int  (*pump)(void);
};

static RsxBackend pick_backend(void)
{
#if defined(_WIN32)
    return { "D3D12", rsx_d3d12_backend_init, rsx_d3d12_backend_present,
             rsx_d3d12_backend_pump_messages };
#else
#  if defined(PS3RECOMP_RSX_VULKAN)
    const char* e = getenv("KZ2_RSX");
    if (!e || strcmp(e, "null") != 0)
        return { "Vulkan", rsx_vulkan_backend_init, rsx_vulkan_backend_present,
                 rsx_vulkan_backend_pump_messages };
#  endif
    return { "null", rsx_null_backend_init, rsx_null_backend_present,
             rsx_null_backend_pump_messages };
#endif
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void peek_tick(void);
static void dumpat_tick(void);
static void dumpfile_tick(void);
static void wws_tick(void);
extern "C" uint64_t ppu_guest_reg(unsigned tid, unsigned reg);
static void threads_tick(void);
static void hkwake_tick(void);

/* Drain the FIFO about every millisecond, deliver 60 Hz vblank ticks on the
 * guest thread (cellGcm_request_tick), and present on the guest's flip once
 * the FIFO has drained up to it. */
static void frame_clock_loop(void)
{
    uint32_t w = 1280, h = 720;
    if (const char* e = getenv("KZ2_RSX_W")) w = (uint32_t)strtoul(e, 0, 0);
    if (const char* e = getenv("KZ2_RSX_H")) h = (uint32_t)strtoul(e, 0, 0);

    const RsxBackend be = pick_backend();
    int rsx_ok = (be.init(w, h, KZ2_TITLE) == 0);
    fprintf(stderr, "[rsx] %s backend init %s (%ux%u)\n", be.name,
            rsx_ok ? "OK" : "FAILED", w, h);

    /* Present right after the drain that reached a flip. The drain stops at
     * each flip, but a slow pass lets several vblank ticks pile up and each
     * tick drains again: checking once per loop showed one flip in five. */
    auto drain = [&]() {
        if (!rsx_ok) return;
        cellGcm_rsx_process_fifo();
        if (cellGcm_take_flip_pending_synced()) {
            be.present();
            g_frames_presented.fetch_add(1);
        }
    };

    uint64_t next_tick = now_ms();
    for (;;) {
        Sleep(1);
        drain();
        peek_tick();
        dumpat_tick();
        dumpfile_tick();
        wws_tick();
        threads_tick();
        hkwake_tick();

        const uint64_t now = now_ms();
        int fired = 0;
        while ((int64_t)(now - next_tick) >= 0 && fired < 240) {
            cellGcm_request_tick();
            drain();
            next_tick += 16;
            fired++;
        }
        if (fired >= 240) next_tick = now;

        if (rsx_ok) {
            if (be.pump() != 0) {
                fprintf(stderr, "[rsx] window closed, exiting\n");
                _exit(0);
            }
            /* KZ2_FPS=1: presented frames per second, every 5 s. */
            static const bool fps_log = getenv("KZ2_FPS") != nullptr;
            static uint64_t fps_t0 = now;
            static unsigned fps_n0 = 0;
            if (fps_log && now - fps_t0 >= 5000) {
                const unsigned n = g_frames_presented.load();
                fprintf(stderr, "[fps] %.1f presented/s (%u total) | spurs pm wid %d lane %u for %llu ms\n",
                        (n - fps_n0) * 1000.0 / (double)(now - fps_t0), n,
                        (int)g_spurs_pm_cur_wid, g_spurs_pm_cur_lane,
                        ps3_ms_now() - g_spurs_pm_cur_since);
                fps_t0 = now; fps_n0 = n;
            }
        }
    }
}

/* KZ2_PEEK=<ea>[,<ea>...]: once a second, print each guest word and the word
 * at +8 of what it points to -- for watching a counter the guest polls. */
static void peek_tick(void)
{
    static const char* spec = getenv("KZ2_PEEK");
    static uint64_t last = 0;
    if (!spec) return;
    uint64_t now = now_ms();
    if (now - last < 1000) return;
    last = now;
    char buf[256];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        uint32_t ea = (uint32_t)strtoul(tok, nullptr, 0);
        uint32_t v = __builtin_bswap32(*(uint32_t*)(vm_base + ea));
        uint32_t d8 = __builtin_bswap32(*(uint32_t*)(vm_base + v + 8));
        fprintf(stderr, "[peek] [0x%08X]=0x%08X  [+8]=0x%08X\n", ea, v, d8);
    }
}

/* KZ2_DUMPAT=<sec>:<ea>:<len>[,...]: hex-dump guest memory once, <sec>
 * seconds after the frame clock starts. */
/* KZ2_THREADS=<sec>[,<sec>...]: print every guest thread's lr and registers. */
extern "C" void ppu_report_guest_lrs(void);
extern "C" uint64_t ppu_guest_reg(unsigned tid, unsigned reg);
static void threads_tick(void)
{
    static const char* spec = getenv("KZ2_THREADS");
    static uint64_t t0 = 0;
    static bool done[8];
    if (!spec) return;
    if (!t0) t0 = now_ms();
    char buf[128];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    int i = 0;
    for (char* tok = strtok(buf, ","); tok && i < 8; tok = strtok(nullptr, ","), i++) {
        const unsigned sec = (unsigned)strtoul(tok, nullptr, 0);
        if (done[i] || now_ms() - t0 < sec * 1000ull) continue;
        done[i] = true;
        setenv("PPU_REPORT_REGS", "1", 0);
        const uint32_t g = cellGcm_fifo_walker_get();
        fprintf(stderr, "[threads] t=%us fifo get=0x%08X word=0x%08X put=0x%08X\n", sec, g,
                __builtin_bswap32(*(uint32_t*)(vm_base + 0x40300000u + g)),
                __builtin_bswap32(*(uint32_t*)(vm_base + 0xE8002000u)));
        ppu_report_guest_lrs();
    }
}

/* KZ2_HKWAKE=<sec>:<tid> -- DIAGNOSTIC: release the semaphore guest thread
 * <tid> sleeps on in func_00803758 (r31 = semaphore object: +4 -> word, +8 =
 * event queue), the way the SPU side releases one (count +1, waiters -1, one
 * event). Tests whether the stall is a missed wake-up. */
extern "C" int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                          uint64_t data1, uint64_t data2, uint64_t data3);
static void hkwake_tick(void)
{
    static const char* spec = getenv("KZ2_HKWAKE");
    static uint64_t t0 = 0;
    static bool done = false;
    if (!spec || done) return;
    if (!t0) t0 = now_ms();
    unsigned sec = 0, tid = 0;
    if (sscanf(spec, "%u:%u", &sec, &tid) != 2) { done = true; return; }
    if (now_ms() - t0 < sec * 1000ull) return;
    done = true;
    const uint32_t obj  = (uint32_t)ppu_guest_reg(tid, 31);
    const uint32_t wptr = __builtin_bswap32(*(uint32_t*)(vm_base + obj + 4));
    const uint32_t q    = __builtin_bswap32(*(uint32_t*)(vm_base + obj + 8));
    uint32_t* w = (uint32_t*)(vm_base + wptr);
    const uint32_t before = __builtin_bswap32(*w);
    if ((before >> 16 & 0xFF) == 0) {
        fprintf(stderr, "[hkwake] obj=0x%08X word=0x%08X: no waiter\n", obj, before);
        return;
    }
    *w = __builtin_bswap32(before + 1 - 0x10000);
    const int rc = sys_event_queue_push_by_id(q, 0xFFFFFFFF53505501ull, 0, (uint64_t)(before >> 24) << 32, 0);
    fprintf(stderr, "[hkwake] obj=0x%08X word 0x%08X -> 0x%08X, event to queue %u rc=%d\n",
            obj, before, __builtin_bswap32(*w), q, rc);
}

static void dumpat_tick(void)
{
    static const char* spec = getenv("KZ2_DUMPAT");
    static uint64_t t0 = 0;
    static bool done[16];
    if (!spec) return;
    if (!t0) t0 = now_ms();
    char buf[512];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    int i = 0;
    for (char* tok = strtok(buf, ","); tok && i < 16; tok = strtok(nullptr, ","), i++) {
        unsigned sec = 0, ea = 0, len = 0, tk = 0, off = 0;
        /* <sec>:T<k>[>p1>p2...]+<off>:<len> dumps relative to word k of the
         * last cellSpursCreateTask argument (heap addresses move between
         * runs); each >p follows the pointer stored at the current address + p. */
        unsigned rtid = 0, rreg = 0;
        const bool by_reg = sscanf(tok, "%u:R%u.%u", &sec, &rtid, &rreg) == 3;
        if (by_reg || (sscanf(tok, "%u:T%u", &sec, &tk) == 2 && tk < 4)) {
            const char* q;
            if (by_reg) {
                /* R<tid>.<reg>: a guest thread's register as the base */
                ea = (uint32_t)ppu_guest_reg(rtid, rreg);
                q = strchr(strchr(tok, 'R'), '.') + 1;
                while (*q >= '0' && *q <= '9') q++;
            } else {
                q = strchr(tok, 'T') + 2;
                ea = g_spurs_last_task_arg[tk];
            }
            while (*q == '>') {
                const unsigned p = (unsigned)strtoul(q + 1, (char**)&q, 16);
                ea = __builtin_bswap32(*(uint32_t*)(vm_base + ea + p));
            }
            if (sscanf(q, "+%x:%x", &off, &len) != 2) continue;
            ea += off;
        } else if (sscanf(tok, "%u:%x:%x", &sec, &ea, &len) != 3) continue;
        if (done[i]) continue;
        if (now_ms() - t0 < sec * 1000ull) continue;
        done[i] = true;
        fprintf(stderr, "[dumpat] t=%us 0x%08X+0x%X:", sec, ea, len);
        for (unsigned o = 0; o < len; o += 4) {
            if ((o & 31) == 0) fprintf(stderr, "\n  %08X:", ea + o);
            fprintf(stderr, " %08X", __builtin_bswap32(*(uint32_t*)(vm_base + ea + o)));
        }
        fprintf(stderr, "\n");
    }
}

/* KZ2_DUMPFILE=<sec>:<ea>:<len>:<path>: write guest memory to a file once. */
static void dumpfile_tick(void)
{
    static const char* spec = getenv("KZ2_DUMPFILE");
    static uint64_t t0 = 0;
    static bool done = false;
    if (!spec || done) return;
    if (!t0) t0 = now_ms();
    unsigned sec = 0, ea = 0, len = 0;
    char path[512] = {0};
    if (sscanf(spec, "%u:%x:%x:%511s", &sec, &ea, &len, path) != 4) { done = true; return; }
    if (now_ms() - t0 < sec * 1000ull) return;
    done = true;
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(vm_base + ea, 1, len, f); fclose(f); }
    fprintf(stderr, "[dumpfile] 0x%08X+0x%X -> %s\n", ea, len, path);
}

/* KZ2_WWS_DUMP=<sec>:<list hdr ea>[,...]: walk a WWS job list once -- header
 * rows, then the command words (runs of the same command collapsed), and for
 * every `00000002 <ea>` entry the 16 bytes at <ea>, which looks like the
 * dependency it waits on. */
static void wws_tick(void)
{
    static const char* spec = getenv("KZ2_WWS_DUMP");
    static uint64_t t0 = 0;
    static bool done[8];
    if (!spec) return;
    if (!t0) t0 = now_ms();
    auto be32 = [](uint32_t ea) { return __builtin_bswap32(*(uint32_t*)(vm_base + ea)); };
    char buf[256];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    int i = 0;
    for (char* tok = strtok(buf, ","); tok && i < 8; tok = strtok(nullptr, ","), i++) {
        unsigned sec = 0, hdr = 0;
        if (done[i] || sscanf(tok, "%u:%x", &sec, &hdr) != 2) continue;
        if (now_ms() - t0 < sec * 1000ull) continue;
        done[i] = true;
        const uint32_t n = be32(hdr + 0x2C), list = be32(hdr + 0x3C);
        fprintf(stderr, "[wws] t=%us list 0x%08X: %u entries at 0x%08X\n", sec, hdr, n, list);
        for (uint32_t r = 0; r < 4; r++)
            fprintf(stderr, "[wws]   row %u: %08X %08X %08X %08X\n", r, be32(hdr + 0x40 + 16 * r),
                    be32(hdr + 0x44 + 16 * r), be32(hdr + 0x48 + 16 * r), be32(hdr + 0x4C + 16 * r));
        uint32_t run_cmd = 0xFFFFFFFFu, run_start = 0, run_len = 0;
        auto flush = [&]() {
            if (run_len) fprintf(stderr, "[wws]   %04X..%04X cmd %08X x%u\n", run_start,
                                 run_start + run_len - 1, run_cmd, run_len);
            run_len = 0;
        };
        for (uint32_t k = 0; k < n && k < 0x1000; k++) {
            const uint32_t c = be32(list + 8 * k), a = be32(list + 8 * k + 4);
            if (c == 2) {
                flush();
                fprintf(stderr, "[wws]   %04X guard @%08X: %08X %08X %08X %08X\n", k, a,
                        be32(a), be32(a + 4), be32(a + 8), be32(a + 12));
                continue;
            }
            if (c != run_cmd) { flush(); run_cmd = c; run_start = k; }
            run_len++;
        }
        flush();
    }
}

static DWORD WINAPI frame_clock(LPVOID)
{
    frame_clock_loop();
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root: <root>/PS3_GAME/USRDIR/EBOOT.ELF -> <root>, $PS3_VFS_ROOT overrides
 * -----------------------------------------------------------------------*/
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
#else
    /* Every 32-bit guest address is backed and reads zero; the kernel supplies
     * each page on first touch, nothing is committed up front. */
    void* m = mmap(nullptr, VM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (m == MAP_FAILED) ? nullptr : (uint8_t*)m;
#endif
    ppu_vm_size = 0;   /* whole space backed: no OOB guard */
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Crash report (POSIX)
 * -----------------------------------------------------------------------*/
#ifndef _WIN32
static void crash_handler(int sig, siginfo_t* si, void*)
{
    char buf[160];
    uintptr_t addr = (uintptr_t)si->si_addr;
    int n = snprintf(buf, sizeof buf, "\n[crash] signal %d at host %p", sig, si->si_addr);
    if (vm_base && addr >= (uintptr_t)vm_base && addr < (uintptr_t)vm_base + VM_SIZE)
        n += snprintf(buf + n, sizeof buf - n, " (guest 0x%08llX)",
                      (unsigned long long)(addr - (uintptr_t)vm_base));
    n += snprintf(buf + n, sizeof buf - n, "\n");
    write(2, buf, n);
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + sig);
}

static void install_crash_handler(void)
{
    static uint8_t alt_stack[64 * 1024];
    stack_t ss = {};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

struct GuestRun { uint32_t entry; int rc; };

static void* guest_main(void* p)
{
    GuestRun* run = (GuestRun*)p;
    kz2_ftrace_mark_main();
    run->rc = ppu_run(run->entry, STACK_TOP);
    return nullptr;
}
#endif

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                         uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char** argv)
{
#if defined(__linux__)
    /* KZ2_ALLOW_PTRACE=1: let any process of this user attach (eu-stack, gdb -p)
     * under yama ptrace_scope=1, so a stalled run can be inspected without
     * starting it under a debugger, which is too slow to reach the stall. */
    if (getenv("KZ2_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

    if (argc < 2) {
        printf("usage: %s <EBOOT.ELF>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);
#else
    if (!getenv("KZ2_NO_CRASH_HANDLER")) install_crash_handler();
#endif

    printf("=== Tokisaki Port: Killzone 2 (BCUS98116) ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not reserve the guest address space\n");
        return 1;
    }

    /* The runtime's GCM window (labels, put/get/ref, IO offset tables) defaults
     * to 0x20000000. Killzone 2 reserves its streaming heap with
     * sys_mmapper_allocate_address, which this runtime hands out upward from
     * 0x11000000, and the heap grows straight across 0x20000000: the title's
     * allocator wrote strings and refcounts over the control register and
     * `put` turned odd (0x13009). Nothing of the title's lives above the
     * stacks (0xD0000000..0xE0000000), so the window goes there. */
    ppu_hle_inject_base = 0xE8000000u;

    /* Killzone 2 parks the RSX mid-frame and patches the park once the rest
     * of the frame is built; give it time instead of skipping to `put`
     * (patches/ps3recomp/0005, cellGcmSys.c). */
    /* Killzone 2 always patches its mid-frame parks eventually, and the real RSX
     * waits on a park for as long as it takes. A finite grace dropped frame
     * remainders whenever a frame took longer than it (loading, first use of a
     * shader), and the renderer then waited forever for a label released
     * there. 10 minutes is "never" for practical purposes. */
    setenv("GCM_PARK_GRACE_MS", "600000", 0);
    /* The in-level colour grading reads a 64x64 LUT that the atmosphere system
     * fills at run time; that fill does not happen yet, and an all-zero LUT
     * turns every in-level frame black. Until it does, the engine substitutes
     * an identity curve (docs/PROGRESS.md section 28). */
    setenv("RSX_ZERO_LUT_IDENTITY", "1", 0);
    /* In the level the frame is gated by jm_post (workload 7), the SPU
     * post-processing job list. Its policy module keeps running while it has
     * jobs instead of returning to the kernel after each one (KZ3 fix list
     * R6), and it may use up to three SPUs instead of one -- the WWS job
     * manager claims jobs atomically per SPU row. 1.5 -> ~1.9 frames/s
     * (docs/PROGRESS.md section 31). */
    setenv("SPURS_PM_SELECT_CURRENT", "1", 0);
    setenv("SPURS_MAXCONT", "7:3", 0);

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Title id (BCUS98116) and name from the disc's own PARAM.SFO: cellGame,
     * cellSaveData and trophies build their /dev_hdd0/game/<id> paths on it. */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();
    kz2_hle_register();      /* after the generic table & syscalls: title overrides win */

    g_ps3_guest_caller = guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);

#ifdef _WIN32
    int rc = ppu_run(entry, STACK_TOP);
#else
    /* The process main thread only has ulimit -s; lifted call chains nest far
     * deeper, so the guest's main thread gets a 256 MB host stack. */
    GuestRun run = { entry, 0 };
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, MAIN_HOST_STACK_BYTES);
    int err = pthread_create(&th, &attr, guest_main, &run);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "ERROR: main guest thread could not be created (%d)\n", err);
        return 1;
    }
    pthread_join(th, nullptr);
    int rc = run.rc;
#endif

    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
