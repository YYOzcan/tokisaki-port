/*
 * Tokisaki Port -- lifted-function entry histogram (diagnostic builds only).
 *
 * Configure with -DKZ2_FTRACE=ON: the lifted PPU chunks are then compiled with
 * -finstrument-functions and every guest function entry lands here. With
 * KZ2_FTRACE=<start>:<len> (seconds) the guest main thread's entries during
 * that window are counted and written to logs/ftrace.txt as
 * "<count> <host offset>", which tools/ftrace_resolve.sh maps back to
 * func_XXXXXXXX names. In a normal build nothing calls these hooks.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <unordered_map>

#define NOINSTR __attribute__((no_instrument_function))

thread_local bool g_kz2_ftrace_main = false;   /* set on the guest main thread */

namespace {
struct Window { double start = -1, len = 0; };
NOINSTR double now_s()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
NOINSTR Window& window()
{
    static Window w = [] () NOINSTR {
        Window x;
        if (const char* e = getenv("KZ2_FTRACE")) {
            double s = 0, l = 1;
            sscanf(e, "%lf:%lf", &s, &l);
            x.start = now_s() + s; x.len = l;
        }
        return x;
    }();
    return w;
}
std::unordered_map<uintptr_t, uint64_t>* g_hits;
std::atomic<int> g_state{0};   /* 0 idle, 1 recording, 2 written */
}

extern "C" NOINSTR void kz2_ftrace_mark_main(void) { g_kz2_ftrace_main = true; }

extern "C" NOINSTR void __cyg_profile_func_enter(void* fn, void*)
{
    if (!g_kz2_ftrace_main) return;
    Window& w = window();
    if (w.start < 0) return;
    double t = now_s();
    if (t < w.start) return;
    if (t < w.start + w.len) {
        if (!g_hits) { g_hits = new std::unordered_map<uintptr_t, uint64_t>(); g_state = 1; }
        (*g_hits)[(uintptr_t)fn]++;
        return;
    }
    if (g_state.exchange(2) != 1) return;
    Dl_info info{};
    dladdr(fn, &info);
    uintptr_t base = (uintptr_t)info.dli_fbase;
    FILE* f = fopen("logs/ftrace.txt", "w");
    if (!f) return;
    for (auto& [addr, n] : *g_hits) fprintf(f, "%llu %llx\n", (unsigned long long)n, (unsigned long long)(addr - base));
    fclose(f);
    fprintf(stderr, "[ftrace] wrote %zu functions to logs/ftrace.txt\n", g_hits->size());
}

extern "C" NOINSTR void __cyg_profile_func_exit(void*, void*) {}
