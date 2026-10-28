#!/usr/bin/env python3
"""Tokisaki Port -- name the shared-library samples of a kzprof raw dump
(KZPROF_RAW, with its "# map" lines): per thread, the hottest functions inside
libc, the GPU driver and the other libraries, from their dynamic symbols.
    tools/kzprof_libs.py logs/kzprof.raw [tid] [top=25]"""
import bisect, collections, subprocess, sys
raw = sys.argv[1]; want = int(sys.argv[2]) if len(sys.argv) > 2 else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
maps, base, samples = [], 0, []
for line in open(raw):
    if line.startswith('# map '):
        _, _, lo, hi, off, path = line.split(' ', 5)
        maps.append((int(lo, 16), int(hi, 16), int(off, 16), path.strip()))
    elif line.startswith('# base '):
        base = int(line.split()[2], 16)
    elif not line.startswith('#'):
        t, ip = line.split()[:2]
        samples.append((int(t), int(ip, 16) + base))
syms = {}
def table(path):
    if path not in syms:
        out = subprocess.run(['nm', '-D', '--defined-only', path], capture_output=True, text=True).stdout
        out += subprocess.run(['nm', '--defined-only', path], capture_output=True, text=True).stdout
        v = sorted({(int(l.split()[0], 16), l.split()[2]) for l in out.splitlines()
                    if len(l.split()) >= 3 and l.split()[1] in 'tTwWiI'})
        syms[path] = ([a for a, _ in v], [n for _, n in v])
    return syms[path]
cnt = collections.Counter(); tot = collections.Counter()
for t, ip in samples:
    if want is not None and t != want: continue
    tot[t] += 1
    for lo, hi, off, path in maps:
        if lo <= ip < hi and not path.endswith('/killzone2'):
            a, n = table(path); x = ip - lo + off
            i = bisect.bisect_right(a, x) - 1
            cnt[(path.split('/')[-1], n[i] if i >= 0 else '?')] += 1
            break
n = sum(tot.values())
for (lib, fn), c in cnt.most_common(top):
    print(f"{100 * c / n:6.2f}%  {lib:28} {fn}")
