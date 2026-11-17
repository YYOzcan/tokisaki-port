#!/usr/bin/env python3
"""Tokisaki Port -- speculative float runs in the register-local SPU copies.

In a function rewritten with registers as C locals (tools/spu_regpromote.py,
the constant-pool copies of tools/spu_constpool.py), every fa/fs/fm/fma/fms/
fnms is the RZ-mode instruction plus a range test and a branch to the exact
path. The branches cut a loop body into blocks of a few instructions. Here a
straight run of plain register statements (no label, branch, call, return or
local-store write) with at least two of those ops becomes

    { __m128i _bad = zero;
      the run on fresh locals, the float ops as spu_*_spec (no branch; the
      lanes that need the exact path OR-ed into _bad);
      if (_bad is clear) the registers the run wrote take their new values;
      else the run again as it was, from the untouched registers; }

Nothing outside the run can see the difference: its inputs are untouched until
the commit, it reads the local store but does not write it, and where _bad is
clear the spec ops' values are the ordinary fast path's (spu_helpers.h). The
idea is the inFAMOUS 2 port's SPU_LIFT_SPEC_FLOAT (SamiEnazi/infamous2-port,
MIT); this pass is written for this port's lifted form.
    tools/spu_specfloat.py <spu_recomp.c> [function-name-substring ...]
rewrites in place the functions ending in _cp (or naming one of the
substrings)."""
import re, sys

FUNC = re.compile(r'^(?:static\s+)?void\s+(\w+)\s*\(\s*spu_context\s*\*\s*ctx\s*\)\s*\{\s*$')
STMT = re.compile(r'^(\s*)r(\d+) = (.+);\s*$')
FLT = re.compile(r'^spu_(fa|fs|fm|fma|fms|fnms)\((.*)\)$')
REG = re.compile(r'\br(\d+)\b')
NOP = re.compile(r'^\s*/\* nop \*/;\s*$')
MARK = '/* specfloat */'
MIN_FLT = 2


def plain(rhs):
    """A right-hand side with no effect but register and local-store reads."""
    if any(k in rhs for k in ('?', '{ r', 'goto', 'return', 'spu_ls_write', '"')):
        return False
    for m in re.finditer(r'(\w+)\s*\(\s*ctx\b', rhs):
        if m.group(1) != 'spu_ls_read128':
            return False
    return 'ctx->' not in rhs


def emit(run, out, counter):
    stm = [(ind, int(n), rhs) for (ind, n, rhs, _) in run if rhs is not None]
    if sum(1 for _, _, rhs in stm if FLT.match(rhs)) < MIN_FLT:
        out.extend(x[3] for x in run)
        return 0
    ind = stm[0][0]
    cur, body = {}, []
    for _, n, rhs in stm:
        new = REG.sub(lambda m: cur.get(int(m.group(1)), m.group(0)), rhs)
        fm = FLT.match(new)
        if fm:
            new = f'spu_{fm.group(1)}_spec({fm.group(2)}, &_bad)'
        k = counter[0]; counter[0] += 1
        body.append(f'{ind}    const u128 _s{k} = {new};')
        cur[n] = f'_s{k}'
    commit = ' '.join(f'r{n} = {v};' for n, v in sorted(cur.items()))
    out.append(f'{ind}{{ {MARK} __m128i _bad = _mm_setzero_si128();')
    out.extend(body)
    out.append(f'{ind}    if (__builtin_expect(_mm_testz_si128(_bad, _bad), 1)) {{ {commit} }}')
    out.append(f'{ind}    else {{')
    out.extend('    ' + x[3] for x in run)
    out.append(f'{ind}    }} }}')
    return 1


def rewrite(body):
    out, run, counter, n = [], [], [0], 0
    def flush():
        nonlocal run, n
        if run:
            n += emit(run, out, counter)
        run = []
    for l in body:
        m = STMT.match(l)
        if m and plain(m.group(3)):
            run.append((m.group(1), m.group(2), m.group(3), l)); continue
        if NOP.match(l):
            run.append((None, None, None, l)); continue
        flush()
        out.append(l)
    flush()
    return out, n


def main():
    path, wants = sys.argv[1], sys.argv[2:]
    src = open(path).read().split('\n')
    res, i, runs, funcs = [], 0, 0, 0
    while i < len(src):
        m = FUNC.match(src[i])
        if not m:
            res.append(src[i]); i += 1; continue
        j = i + 1
        while j < len(src) and src[j] != '}':
            j += 1
        body = src[i + 1:j]
        name = m.group(1)
        take = (any(w in name for w in wants) if wants else name.endswith('_cp')) \
            and any('/* regpromote */' in l for l in body) and not any(MARK in l for l in body)
        if take:
            body, k = rewrite(body)
            runs += k; funcs += k > 0
        res.append(src[i]); res.extend(body)
        i = j
    open(path, 'w').write('\n'.join(res))
    print(f'{path}: {runs} speculative runs in {funcs} functions')


if __name__ == '__main__':
    main()
