#!/usr/bin/env python3
"""Tokisaki Port -- who calls a leaf function, from a kzprof raw dump taken with KZPROF_CALLER=1 (the word at the
user stack pointer, which is the return address while a sample sits in a leaf that has pushed nothing: libc's
memcpy/memset, small runtime helpers).
    tools/kzprof_callers.py logs/NAME <fps> <tid>[,<tid>...]|lanes <symbol substring> [top=20]"""
import bisect, collections, gzip, re, subprocess, sys

base, fps, which, want = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
top = int(sys.argv[5]) if len(sys.argv) > 5 else 20
txt = open(base + '.txt').read()
secs = float(re.search(r'samples over ([\d.]+) s', txt).group(1))
busy = {m.group(1): float(m.group(2)) for m in re.finditer(r'^\s+(\d+) \S+\s+([\d.]+)%', txt, re.M)}
tids = sorted([t for t, b in busy.items() if 50 < b < 80], key=int)[-5:] if which == 'lanes' else which.split(',')
maps, exe_base = [], 0
for l in open(base + '.raw'):
    if l.startswith('# map '):
        _, _, lo, hi, off, path = l.rstrip('\n').split(' ', 5)
        maps.append((int(lo, 16), int(hi, 16), int(off, 16), path))
    elif l.startswith('# base '):
        exe_base = int(l.split()[2], 16)
    elif not l.startswith('#'):
        break
addrs, names = [], []
for l in gzip.open(base + '.nm.gz', 'rt'):
    p = l.split()
    if len(p) == 3:
        addrs.append(int(p[0], 16)); names.append(p[2])
o = sorted(range(len(addrs)), key=lambda i: addrs[i])
addrs = [addrs[i] for i in o]; names = [names[i] for i in o]
dsyms = {}

def name_of(off):
    i = bisect.bisect_right(addrs, off) - 1
    if i >= 0 and off < addrs[-1] + 0x100000:
        return names[i]
    a = off + exe_base
    for lo, hi, foff, path in maps:
        if lo <= a < hi and path:
            if path not in dsyms:
                out = subprocess.run(['nm', '-D', '--defined-only', path], capture_output=True, text=True).stdout
                dsyms[path] = sorted((int(x.split()[0], 16), x.split()[2]) for x in out.splitlines()
                                     if len(x.split()) == 3)
            ss = dsyms[path]
            j = bisect.bisect_right([q[0] for q in ss], a - lo + foff) - 1
            return path.split('/')[-1] + ':' + (ss[j][1] if j >= 0 else '?')
    return '?'

c = collections.Counter(); n = 0
for l in open(base + '.raw'):
    if l.startswith('#'): continue
    p = l.split()
    if p[0] not in tids or len(p) < 4: continue
    if want not in name_of(int(p[1], 16)): continue
    n += 1
    c[name_of(int(p[3], 16))] += 1
print(f"{n} samples in *{want}* ({n / secs / fps:.2f} ms/frame); callers:")
for k, v in c.most_common(top):
    print(f"  {v * 100 / max(n, 1):5.1f}%  {v / secs / fps:5.2f} ms  {k[:100]}")
