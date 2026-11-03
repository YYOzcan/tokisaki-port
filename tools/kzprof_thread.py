#!/usr/bin/env python3
"""Tokisaki Port -- the hottest symbols of one or more threads of a kzprof raw dump
(tools/prof_perf.sh writes NAME.raw and NAME.nm.gz), in ms of CPU per frame.
    tools/kzprof_thread.py logs/NAME <fps> <tid>[,<tid>...] [top=30]
    tools/kzprof_thread.py logs/NAME <fps> lanes [top]    the five SPURS lanes (50-80% busy)"""
import bisect, collections, gzip, re, sys

base, fps, which = sys.argv[1], float(sys.argv[2]), sys.argv[3]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
txt = open(base + '.txt').read()
secs = float(re.search(r'samples over ([\d.]+) s', txt).group(1))
busy = {m.group(1): float(m.group(2)) for m in re.finditer(r'^\s+(\d+) \S+\s+([\d.]+)%', txt, re.M)}
if which == 'lanes':
    tids = sorted([t for t, b in busy.items() if 50 < b < 80], key=lambda t: int(t))[-5:]
else:
    tids = which.split(',')
addrs, names = [], []
for l in gzip.open(base + '.nm.gz', 'rt'):
    p = l.split()
    if len(p) == 3:
        addrs.append(int(p[0], 16)); names.append(p[2])
o = sorted(range(len(addrs)), key=lambda i: addrs[i])
addrs = [addrs[i] for i in o]; names = [names[i] for i in o]
c = collections.Counter(); n = 0
for l in open(base + '.raw'):
    if l.startswith('#'): continue
    p = l.split()
    if p[0] not in tids: continue
    off = int(p[1], 16); n += 1
    i = bisect.bisect_right(addrs, off) - 1
    c[names[i] if i >= 0 and off < addrs[-1] + 0x100000 else 'libs'] += 1
per = lambda v: v / secs / fps * 1000 / 1000   # samples at 1 kHz -> ms per frame
print(f"threads {','.join(tids)}: {n} samples, {n / secs / fps:.2f} ms/frame")
for k, v in c.most_common(top):
    print(f"{v * 100 / n:6.2f}% {v / secs / fps:7.2f} ms  {k[:110]}")
