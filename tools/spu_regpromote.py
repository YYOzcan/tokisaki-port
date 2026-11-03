#!/usr/bin/env python3
"""Tokisaki Port -- SPU registers as C locals in lifted functions.

The SPU lifter writes every register as ctx->gpr[N], memory the compiler must
assume any local-store store may change (the build has -fno-strict-aliasing,
and the store goes through a byte pointer): after each LS store every register
the code goes on to use is loaded again, and each result is stored at once.
This pass rewrites a lifted function so the registers it uses live in locals
(u128 rN), which no pointer can reach, and ctx->gpr is the truth only where
something else may read or write it:

  - a line that calls anything with the context except the local-store
    accessors (a lifted function, a channel read or write, the drain, a stop),
    or returns, runs with its original text after the locals are stored back
    into ctx (a run of such lines once), and the locals are loaded again
    after it -- before any goto inside it, too;
  - every other line uses the locals;
  - the function loads them at entry and stores them before falling off its
    end.

So each line computes what it did, on the same values. Usage:
    tools/spu_regpromote.py <spu_recomp.c> [function-name-substring ...]
rewrites the file in place (all functions, or those whose names contain one
of the substrings); a function already rewritten is left alone."""
import re, sys

GPR = re.compile(r'ctx->gpr\[(\d+)\]')
CALL = re.compile(r'(\w*)\s*\(\s*ctx\s*[,)]')   # an empty name: a call through a table or pointer
PURE_CALLS = {'spu_ls_read128', 'spu_ls_write128', 'spu_ls_read32', 'spu_ls_write32'}
FUNC = re.compile(r'^(void\s+(\w+)\s*\(\s*spu_context\s*\*\s*ctx\s*\)\s*\{)\s*$')
LABEL = re.compile(r'^\s*loc_[0-9A-Fa-f]+\s*:')
GOTO = re.compile(r'\bgoto\s+(loc_[0-9A-Fa-f]+)\s*;')
MARK = '/* regpromote */'


def impure(line):
    if re.search(r'\breturn\b|\bSPU_RET', line):
        return True
    return any(m.group(1) not in PURE_CALLS for m in CALL.finditer(line))


def rewrite(name, body):
    regs = sorted({int(n) for l in body for n in GPR.findall(l)})
    if not regs:
        return None
    spill = ' '.join(f'ctx->gpr[{n}] = r{n};' for n in regs)
    reload = ' '.join(f'r{n} = ctx->gpr[{n}];' for n in regs)
    out = ['        ' + MARK + ' u128 ' + ', '.join(f'r{n} = ctx->gpr[{n}]' for n in regs) + ';']
    in_run = False
    for l in body:
        s = l.strip()
        if not s or s.startswith('/*') and s.endswith('*/') and 'ctx' not in s:
            out.append(l)
            continue
        if not LABEL.match(l) and impure(l):
            if not in_run:
                out.append('        { ' + spill + ' }')
                in_run = True
            out.append(GOTO.sub(lambda m: '{ ' + reload + ' goto ' + m.group(1) + '; }', l))
            continue
        if in_run:
            out.append('        { ' + reload + ' }')
            in_run = False
        out.append(GPR.sub(lambda m: 'r' + m.group(1), l))
    if not in_run:
        out.append('        { ' + spill + ' }')
    return out


def main():
    path, wants = sys.argv[1], sys.argv[2:]
    src = open(path).read().split('\n')
    res, i, n_done = [], 0, 0
    while i < len(src):
        m = FUNC.match(src[i])
        if not m:
            res.append(src[i]); i += 1; continue
        j = i + 1
        while j < len(src) and src[j] != '}':
            j += 1
        body = src[i + 1:j]
        name = m.group(2)
        new = None
        if (not wants or any(w in name for w in wants)) and not any(MARK in l for l in body):
            new = rewrite(name, body)
        res.append(src[i])
        res.extend(new if new is not None else body)
        n_done += new is not None
        i = j
    open(path, 'w').write('\n'.join(res))
    print(f'{path}: {n_done} functions rewritten')


if __name__ == '__main__':
    main()
