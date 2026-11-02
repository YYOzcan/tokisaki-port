/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_toggle.h at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_toggle.h -- the same-run A/B of native gates: KZ2_NATIVE_TOGGLE=<slots> flips those slots'
 * gates between the lifted code and the native kernel every KZ2_NATIVE_TOGGLE_PERIOD seconds (default 4) of one run,
 * so a small job's two versions meet the same scene, load and heat. Off unless KZ2_NATIVE_TOGGLE is set.
 *
 * Phase k = floor((now - t0) / period): even = lifted, odd = native (the run starts lifted). The first gate call of a
 * new phase prints `[native-toggle] t=... phase=N native|lifted ...` on stderr, the stream patch 1032's `[wacct]`
 * windows go to: tools/spu/toggle_ab.py drops the wacct window that holds a switch and sums the rest per phase.
 *
 * Slots: a comma list of slot numbers and ranges ("75,76", "75-77"). A toggled slot takes the toggle's gate whatever
 * its flags say (a default-on slot is lifted in the even phases too). Split out of kz2_native_jobs.c so that
 * tools/spu/test_native_toggle.sh tests the parse and the phase rule on the host. */
#ifndef KZ2_NATIVE_TOGGLE_H
#define KZ2_NATIVE_TOGGLE_H

#include <stdlib.h>

/* Marks the slots of `list` in mask[0..nslots); returns how many were marked, -1 on a malformed list (nothing kept
 * from it then: the caller treats that as "toggle off"). */
static inline int kz2_native_toggle_parse(const char* list, unsigned char* mask, int nslots)
{
    int n = 0;
    for (int i = 0; i < nslots; i++) mask[i] = 0;
    const char* p = list;
    while (p && *p) {
        char* e;
        const long a = strtol(p, &e, 10);
        if (e == p) goto bad;
        long b = a;
        p = e;
        if (*p == '-') {
            b = strtol(p + 1, &e, 10);
            if (e == p + 1) goto bad;
            p = e;
        }
        if (a < 0 || b < a || b >= nslots) goto bad;
        for (long s = a; s <= b; s++)
            if (!mask[s]) { mask[s] = 1; n++; }
        if (*p == ',') p++;
        else if (*p) goto bad;
    }
    return n;
bad:
    for (int i = 0; i < nslots; i++) mask[i] = 0;
    return -1;
}

/* Phase index of time t_ms since t0 (period_ms > 0); odd = native. */
static inline long kz2_native_toggle_phase(unsigned long long t_ms, unsigned long long period_ms)
{
    return (long)(t_ms / period_ms);
}

#endif
