#!/usr/bin/env python3
"""Summarise tools/prof_all.sh output.

For every thread: how often it was busy (top frame not a blocking syscall),
and where. Threads are grouped by role, recognised from their stacks.
  python3 tools/prof_report.py logs/prof_all.txt [top=25]
"""
import collections
import re
import sys

WAIT = re.compile(r'__syscall_cancel_arch|futex|nanosleep|clock_nanosleep|epoll_wait|poll|'
                  r'__GI___poll|sched_yield|pthread_cond_wait|ioctl|read$|__libc_read|sem_wait')
FRAME = re.compile(r'^#(\d+)\s+0x[0-9a-f]+\s+(.*)$')


def role(frames):
    s = ' '.join(frames)
    for key, name in (('guest_main', 'guest-main'), ('frame_clock', 'frame-clock'),
                      ('spurs_worker', 'spurs-worker'), ('spurs', 'spurs'),
                      ('spu_thread', 'spu-thread'), ('rsx', 'rsx'), ('fifo', 'fifo'),
                      ('vk', 'vulkan'), ('audio', 'audio'), ('ppu_thread', 'ppu-thread'),
                      ('thread_trampoline', 'guest-thread')):
        if key in s:
            return name
    return 'other'


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    samples = 0
    threads = []          # (tid, frames) for current sample
    busy = collections.Counter()      # role -> busy samples
    seen = collections.Counter()      # role -> samples
    leaf = collections.Counter()      # busy leaf functions (all threads)
    incl = collections.Counter()      # busy inclusive guest/runtime functions
    leaf_by_role = collections.defaultdict(collections.Counter)
    cur = None

    def flush():
        for tid, frames in threads:
            if not frames:
                continue
            r = role(frames)
            seen[r] += 1
            top0 = frames[0]
            if WAIT.search(top0) or (len(frames) > 1 and WAIT.search(frames[1]) and 'syscall' in top0):
                continue
            busy[r] += 1
            leaf[top0] += 1
            leaf_by_role[r][top0] += 1
            for f in set(frames[:40]):
                incl[f] += 1
        threads.clear()

    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('%%SAMPLE'):
            if cur is not None:
                threads.append(cur)
            cur = None
            flush()
            samples += 1
            continue
        if line.startswith('TID '):
            if cur is not None:
                threads.append(cur)
            cur = (line.split()[1].rstrip(':'), [])
            continue
        m = FRAME.match(line)
        if m and cur is not None:
            name = m.group(2).split('(')[0].strip()
            cur[1].append(name)
    if cur is not None:
        threads.append(cur)
    flush()

    print(f'{samples} samples')
    print('busy thread-samples per role (avg busy threads):')
    for r, n in busy.most_common():
        print(f'  {r:14s} {n / max(samples, 1):5.2f} busy of {seen[r] / max(samples, 1):5.2f} threads')
    print('\ntop busy leaf functions (avg threads):')
    for f, n in leaf.most_common(top):
        print(f'  {n / max(samples, 1):5.2f}  {f}')
    print('\ntop busy inclusive (avg threads):')
    for f, n in incl.most_common(top * 2):
        print(f'  {n / max(samples, 1):5.2f}  {f}')
    for r, c in leaf_by_role.items():
        print(f'\n[{r}] top leaves:')
        for f, n in c.most_common(10):
            print(f'  {n / max(samples, 1):5.2f}  {f}')


if __name__ == '__main__':
    main()
