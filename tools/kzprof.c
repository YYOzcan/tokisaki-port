/* Tokisaki Port -- a small sampling profiler on perf_event_open (no perf
 * package needed; kernel.perf_event_paranoid <= 2 lets a user sample its own
 * processes). Samples the user-space IP of every thread of PID at FREQ Hz of
 * task clock for SECONDS, then prints per-thread totals and the hottest
 * symbols (resolved from the binary's symbol table with nm).
 *
 *   cc -O2 -o build/kzprof tools/kzprof.c
 *   build/kzprof PID [seconds=10] [freq=1000] [top=60] > logs/kzprof.txt
 *
 * Threads created after the start are not followed. */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXT 256
#define PAGES 64                     /* ring data pages per thread */

typedef struct { uint64_t ip; uint64_t t; uint32_t tid; } sample_t;
static sample_t* s_samp; static size_t s_n, s_cap;

typedef struct { int fd; int tid; void* base; uint64_t busy; char name[32]; } th_t;
static th_t s_th[MAXT]; static int s_nth;

static void add(uint64_t ip, uint32_t tid, uint64_t t)
{
    if (s_n == s_cap) { s_cap = s_cap ? s_cap * 2 : 1 << 16; s_samp = realloc(s_samp, s_cap * sizeof *s_samp); }
    s_samp[s_n].ip = ip; s_samp[s_n].t = t; s_samp[s_n++].tid = tid;
}

static void drain(th_t* t)
{
    struct perf_event_mmap_page* mp = t->base;
    uint8_t* data = (uint8_t*)t->base + 4096;
    uint64_t size = (uint64_t)PAGES * 4096, head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = mp->data_tail;
    while (tail < head) {
        struct perf_event_header h; uint8_t buf[256];
        for (unsigned i = 0; i < sizeof h; i++) ((uint8_t*)&h)[i] = data[(tail + i) % size];
        if (h.size > sizeof buf || h.size < sizeof h) break;
        for (unsigned i = 0; i < h.size; i++) buf[i] = data[(tail + i) % size];
        if (h.type == PERF_RECORD_SAMPLE) {
            uint64_t ip, tm; uint32_t pid, tid;
            memcpy(&ip, buf + 8, 8); memcpy(&pid, buf + 16, 4); memcpy(&tid, buf + 20, 4);
            memcpy(&tm, buf + 24, 8);
            add(ip, tid, tm); t->busy++;
        }
        tail += h.size;
    }
    __atomic_store_n(&mp->data_tail, tail, __ATOMIC_RELEASE);
}

typedef struct { uint64_t a; char* n; } sym_t;
static sym_t* s_sym; static size_t s_nsym;
static int symcmp(const void* x, const void* y)
{ uint64_t a = ((const sym_t*)x)->a, b = ((const sym_t*)y)->a; return a < b ? -1 : a > b; }
static const char* lookup(uint64_t a)
{
    size_t lo = 0, hi = s_nsym;
    if (!s_nsym || a < s_sym[0].a) return 0;
    while (hi - lo > 1) { size_t m = (lo + hi) / 2; if (s_sym[m].a <= a) lo = m; else hi = m; }
    return s_sym[lo].n;
}

typedef struct { const char* k; uint64_t n; } cnt_t;
static int cntcmp(const void* x, const void* y)
{ uint64_t a = ((const cnt_t*)x)->n, b = ((const cnt_t*)y)->n; return a < b ? 1 : a > b ? -1 : 0; }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: kzprof PID [seconds] [freq] [top]\n"); return 2; }
    int pid = atoi(argv[1]);
    double secs = argc > 2 ? atof(argv[2]) : 10; int freq = argc > 3 ? atoi(argv[3]) : 1000;
    int top = argc > 4 ? atoi(argv[4]) : 60;
    char path[256]; snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR* d = opendir(path); if (!d) { perror(path); return 1; }
    struct dirent* e;
    while ((e = readdir(d)) && s_nth < MAXT) {
        if (e->d_name[0] == '.') continue;
        int tid = atoi(e->d_name);
        struct perf_event_attr at; memset(&at, 0, sizeof at);
        at.size = sizeof at; at.type = PERF_TYPE_SOFTWARE; at.config = PERF_COUNT_SW_TASK_CLOCK;
        at.freq = 1; at.sample_freq = freq; at.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
        at.exclude_kernel = 1; at.exclude_hv = 1; at.disabled = 1; at.wakeup_events = 64;
        int fd = (int)syscall(SYS_perf_event_open, &at, tid, -1, -1, 0);
        if (fd < 0) { perror("perf_event_open"); continue; }
        void* b = mmap(0, (PAGES + 1) * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (b == MAP_FAILED) { perror("mmap"); close(fd); continue; }
        th_t* t = &s_th[s_nth++]; t->fd = fd; t->tid = tid; t->base = b;
        char cp[256]; snprintf(cp, sizeof cp, "/proc/%d/task/%d/comm", pid, tid);
        FILE* f = fopen(cp, "r"); if (f) { if (fgets(t->name, sizeof t->name, f)) t->name[strcspn(t->name, "\n")] = 0; fclose(f); }
    }
    closedir(d);
    for (int i = 0; i < s_nth; i++) ioctl(s_th[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec ts = { 0, 20 * 1000000 }; nanosleep(&ts, 0);
        for (int i = 0; i < s_nth; i++) drain(&s_th[i]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9 >= secs) break;
    }
    for (int i = 0; i < s_nth; i++) { ioctl(s_th[i].fd, PERF_EVENT_IOC_DISABLE, 0); drain(&s_th[i]); }

    /* load base from /proc/pid/maps for the main executable */
    char exe[512] = {0}; uint64_t base = 0;
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t el = readlink(path, exe, sizeof exe - 1); if (el > 0) exe[el] = 0;
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE* m = fopen(path, "r"); char line[1024];
    typedef struct { uint64_t lo, hi, off; char n[64]; char path[256]; } map_t; static map_t maps[4096]; int nm = 0;
    while (m && fgets(line, sizeof line, m)) {
        uint64_t lo, hi, off; char perm[8], file[512] = ""; int fo = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %n", &lo, &hi, perm, &off, &fo) < 4) continue;
        if (fo > 0) { snprintf(file, sizeof file, "%s", line + fo); file[strcspn(file, "\n")] = 0; }
        if (!base && strcmp(file, exe) == 0 && off == 0) base = lo;
        if (perm[2] == 'x' && nm < 4096) { maps[nm].lo = lo; maps[nm].hi = hi; maps[nm].off = off;
            snprintf(maps[nm].path, sizeof maps[nm].path, "%s", file);
            const char* sl = strrchr(file, '/'); snprintf(maps[nm].n, 64, "%s", sl ? sl + 1 : file); nm++; }
    }
    if (m) fclose(m);
    /* KZPROF_RAW=file: every sample as "tid ip-offset" (offset from the
     * executable's load base) for offline analysis (tools/kzprof_ips.py) */
    { const char* rawp = getenv("KZPROF_RAW"); FILE* rf = rawp ? fopen(rawp, "w") : 0;
      if (rf) { fprintf(rf, "# exe %s\n", exe);
                /* the executable mappings, so library samples can be named
                 * offline: "# map <lo> <hi> <file offset> <path>" (absolute ips) */
                for (int j = 0; j < nm; j++)
                    fprintf(rf, "# map %lx %lx %lx %s\n", (unsigned long)maps[j].lo, (unsigned long)maps[j].hi,
                            (unsigned long)maps[j].off, maps[j].path);
                fprintf(rf, "# base %lx\n", (unsigned long)base);
                for (size_t i = 0; i < s_n; i++) fprintf(rf, "%u %lx %lu\n", s_samp[i].tid, (unsigned long)(s_samp[i].ip - base), (unsigned long)s_samp[i].t);
                fclose(rf); } }
    char cmd[700]; snprintf(cmd, sizeof cmd, "nm --defined-only '%s' 2>/dev/null", exe);
    FILE* p = popen(cmd, "r");
    while (p && fgets(line, sizeof line, p)) {
        uint64_t a; char ty, nmb[512];
        if (sscanf(line, "%lx %c %511s", &a, &ty, nmb) != 3) continue;
        if (ty != 't' && ty != 'T' && ty != 'W' && ty != 'w') continue;
        if (s_nsym % 65536 == 0) s_sym = realloc(s_sym, (s_nsym + 65536) * sizeof *s_sym);
        s_sym[s_nsym].a = a + base; s_sym[s_nsym++].n = strdup(nmb);
    }
    if (p) pclose(p);
    qsort(s_sym, s_nsym, sizeof *s_sym, symcmp);

    double el_s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("%zu samples over %.1f s at %d Hz\n\nper thread (busy fraction of one core):\n", s_n, el_s, freq);
    for (int i = 0; i < s_nth; i++)
        if (s_th[i].busy > (uint64_t)(freq * el_s * 0.01))
            printf("  %6d %-16s %5.1f%%\n", s_th[i].tid, s_th[i].name, 100.0 * s_th[i].busy / (freq * el_s));
    /* symbol histogram, overall and per busy thread */
    for (int pass = -1; pass < s_nth; pass++) {
        if (pass >= 0 && s_th[pass].busy < (uint64_t)(freq * el_s * 0.05)) continue;
        cnt_t* c = calloc(s_n + 1, sizeof *c); size_t nc = 0; uint64_t tot = 0;
        for (size_t i = 0; i < s_n; i++) {
            if (pass >= 0 && (int)s_samp[i].tid != s_th[pass].tid) continue;
            const char* k = lookup(s_samp[i].ip);
            if (!k || s_samp[i].ip >= base + 0x40000000ull) {
                k = "?";
                for (int j = 0; j < nm; j++) if (s_samp[i].ip >= maps[j].lo && s_samp[i].ip < maps[j].hi) { k = maps[j].n; break; }
            }
            tot++;
            size_t j; for (j = 0; j < nc; j++) if (c[j].k == k) break;
            if (j == nc) { c[nc].k = k; c[nc++].n = 0; }
            c[j].n++;
        }
        qsort(c, nc, sizeof *c, cntcmp);
        if (pass < 0) printf("\nall threads, top symbols:\n");
        else printf("\n[%d %s] top symbols:\n", s_th[pass].tid, s_th[pass].name);
        for (size_t j = 0; j < nc && (int)j < (pass < 0 ? top : top / 3); j++)
            printf("  %5.1f%%  %s\n", 100.0 * c[j].n / (tot ? tot : 1), c[j].k);
        free(c);
    }
    return 0;
}
