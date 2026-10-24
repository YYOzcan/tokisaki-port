#!/usr/bin/env python3
"""Per-thread activity timeline from a kzprof raw dump (with timestamps).
  python3 tools/kzprof_timeline.py logs/NAME [start_ms=5000] [span_ms=400] [bucket_ms=2]
One row per busy thread, one character per bucket: what the thread ran most in
it, '.' when it was not on a CPU (sleeping or waiting). The legend maps job
modules to letters; S = PPU spin on SPU progress (func_003BD660),
G = other guest PPU code, R = SPU runtime, V = vertex fetch, D = draw engine,
K = Vulkan/driver/libc, P = SPURS policy module."""
import bisect, collections, gzip, re, sys

base = sys.argv[1]
start = float(sys.argv[2]) if len(sys.argv) > 2 else 5000
span = float(sys.argv[3]) if len(sys.argv) > 3 else 400
bucket = float(sys.argv[4]) if len(sys.argv) > 4 else 2

addrs, names = [], []
for l in gzip.open(base + '.nm.gz', 'rt'):
    p = l.split()
    if len(p) == 3:
        addrs.append(int(p[0], 16)); names.append(p[2])
order = sorted(range(len(addrs)), key=lambda i: addrs[i])
addrs = [addrs[i] for i in order]; names = [names[i] for i in order]

letters = {}
def cat(off):
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0 or off > addrs[-1] + 0x100000:
        return 'K'
    n = names[i]
    m = re.match(r'jobmod_([0-9a-f]+)', n)
    if m:
        k = m.group(1)
        if k not in letters:
            letters[k] = 'abcdefghijklmnopqrstuvwxyz0123456789'[len(letters) % 36]
        return letters[k]
    if n.startswith('pm_'): return 'P'
    if n == '_Z13func_003BD660P11ppu_context': return 'S'
    if n.startswith('_Z13func_'): return 'G'
    if n.startswith(('spu_', 'mfc_', 'yz_')): return 'R'
    if n.startswith('rsx_vertex'): return 'V'
    if n.startswith(('rsx_', 'eng_', 'sink_', 'cellGcm')): return 'D'
    if n.startswith('vk_'): return 'K'
    return 'o'

rows = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
t0 = None
samples = []
for line in open(base + '.raw'):
    if line.startswith('#'):
        continue
    p = line.split()
    if len(p) < 3:
        sys.exit('no timestamps in this dump (kzprof before the timeline change)')
    tid, off, t = int(p[0]), int(p[1], 16), int(p[2])
    samples.append((t, tid, off))
samples.sort()
t0 = samples[0][0]
for t, tid, off in samples:
    ms = (t - t0) / 1e6
    if start <= ms < start + span:
        rows[tid][int((ms - start) // bucket)][cat(off)] += 1
nb = int(span // bucket)
busy = sorted(rows, key=lambda r: -sum(sum(c.values()) for c in rows[r].values()))
for tid in busy[:12]:
    line = ''.join(rows[tid][b].most_common(1)[0][0] if rows[tid][b] else '.' for b in range(nb))
    print(f'{tid:>8} {line}')
print('legend:', ', '.join(f'{v}={k}' for k, v in letters.items()))
