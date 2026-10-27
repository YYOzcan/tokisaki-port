#!/usr/bin/env python3
"""CPU time per SPU job module / runtime area from kzprof raw dumps, per
presented frame. Compare runs:
  python3 tools/kzprof_mods.py logs/kzprof1 logs/kzprof2 ...
(each name without extension: NAME.raw and NAME.log are read)"""
import bisect, collections, re, subprocess, sys

def syms(binary, saved=None):
    import gzip, os
    if saved and os.path.exists(saved):
        out = gzip.open(saved, 'rt').read()
    else:
        out = subprocess.run(['nm', '--defined-only', binary], capture_output=True, text=True).stdout
    a = sorted((int(l.split()[0], 16), l.split()[2]) for l in out.split('\n')
               if len(l.split()) == 3 and l.split()[1] in 'tTwW')
    return [x for x, _ in a], [n for _, n in a]

def group(name):
    m = re.match(r'(jobmod_[0-9a-f]+|pm_[0-9A-F]+|spu_\d+_at_[0-9A-F]+)', name)
    if m: return m.group(1)
    if name.startswith('_Z13func_'): return 'ppu-guest'
    if name.startswith(('spu_', 'mfc_', 'yz_')): return 'spu-runtime'
    if name.startswith(('rsx_', 'eng_', 'sink_', 'vk_', 'cellGcm')): return 'rsx'
    return 'other'

cols = []
for base in sys.argv[1:]:
    addrs, names = syms('build/killzone2', base + '.nm.gz')
    fps = [float(x) for x in re.findall(r'\[fps\] ([\d.]+) presented', open(base + '.log', errors='replace').read())[-4:]]
    fps = sum(fps) / len(fps)
    c = collections.Counter(); n = 0
    for line in open(base + '.raw'):
        if line.startswith('#'): continue
        off = int(line.split()[1], 16); n += 1
        i = bisect.bisect_right(addrs, off) - 1
        c[group(names[i]) if i >= 0 and off < addrs[-1] + 0x100000 else 'libs'] += 1
    secs = 20.0
    try:   # the sampling time, from the report's first line ("N samples over S s")
        secs = float(re.search(r'samples over ([\d.]+) s', open(base + '.txt').read()).group(1))
    except (OSError, AttributeError):
        pass
    cols.append((base, fps, {k: v / (secs * fps) for k, v in c.items()}))   # ms per frame (1000 Hz)
keys = sorted(set(k for _, _, d in cols for k in d), key=lambda k: -max(d.get(k, 0) for _, _, d in cols))
print('%-28s' % 'ms CPU per frame' + ''.join('%14s' % b.split('/')[-1] for b, _, _ in cols))
print('%-28s' % 'fps' + ''.join('%14.1f' % f for _, f, _ in cols))
for k in keys[:30]:
    print('%-28s' % k + ''.join('%14.1f' % d.get(k, 0) for _, _, d in cols))
