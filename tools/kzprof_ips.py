#!/usr/bin/env python3
"""Instruction-level view of one function from a kzprof raw dump.
  python3 tools/kzprof_ips.py logs/kzprof.raw SYMBOL [top=40] [binary=build/killzone2]
Prints the hottest instructions of SYMBOL with their share of its samples,
each with the few instructions before it (where the cost usually comes from)."""
import collections, subprocess, sys

raw, sym = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
binary = sys.argv[4] if len(sys.argv) > 4 else 'build/killzone2'
nm = subprocess.run(['nm', '--defined-only', binary], capture_output=True, text=True).stdout.split('\n')
addrs = sorted((int(l.split()[0], 16), l.split()[2]) for l in nm if len(l.split()) == 3 and l.split()[1] in 'tTwW')
lo = next(a for a, n in addrs if n == sym)
hi = min(a for a, n in addrs if a > lo)
cnt = collections.Counter()
for line in open(raw):
    if line.startswith('#'):
        continue
    off = int(line.split()[1], 16)
    if lo <= off < hi:
        cnt[off] += 1
tot = sum(cnt.values())
print(f'{sym}: {tot} samples, {hi - lo} bytes')
dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn', f'--start-address={lo}', f'--stop-address={hi}', binary],
                     capture_output=True, text=True).stdout.split('\n')
ins = []
for l in dis:
    p = l.strip().split(':', 1)
    if len(p) == 2:
        try:
            ins.append((int(p[0], 16), p[1].strip()))
        except ValueError:
            pass
idx = {a: i for i, (a, _) in enumerate(ins)}
for off, n in cnt.most_common(top):
    i = idx.get(off)
    print(f'--- {100.0 * n / tot:5.1f}%  {off:x}')
    if i is None:
        continue
    for a, t in ins[max(0, i - 3):i + 1]:
        print(f'      {a:x}  {t}')
