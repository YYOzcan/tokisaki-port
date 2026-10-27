#!/usr/bin/env python3
"""spu_regcache.py -- keep the lifted SPU code's registers in locals (spu_recomp_rc.c).

Taken from the inFAMOUS PC port's copy of ps3recomp (whompay/inFAMOUSreborn,
ps3recomp/tools/spu_regcache.py, "v95"; MIT, see THIRD_PARTY_NOTICES.md), with
this port's changes marked "kz2:". Their description:

The lifter emits every SPU op as  ctx->gpr[D] = spu_op(ctx->gpr[A], ...);  so every
operand is a load from and every result a store to the spu_context in memory, and the
LS accessors write through a uint8_t* (ctx->ls) which may alias ctx->gpr -- after every
spu_ls_write128 the compiler must reload every register. This pass keeps the SPU
registers of each lifted function in u128 LOCALS (no address taken -> no aliasing,
clang keeps them in xmm registers):

  * entry:            u128 rN = ctx->gpr[N];   for every register the pure code uses
  * pure lines        (register ops, LS read/write, gotos, labels): ctx->gpr[N] -> rN
  * any other line    (calls, channels, stop, trampolines, returns, halts):
                      spill the registers the pure code writes back to ctx->gpr BEFORE it,
                      run it unchanged (it reads/writes ctx->gpr), reload AFTER it.
                      "if (pure cond) { impure body }": spill inside the braces only.
  * end of function:  spill (falls through to the caller).

ctx is therefore exact at every point any other code can observe it.

kz2: the output replaces spu_recomp.c in the build (CMake KZ2_SPU_REGCACHE) with
the same symbol names -- no run-time twin selection (--twin restores theirs),
and no spu_helpers_vec.h (this runtime has none). Every statement of this
lifter's output is one line with no else/switch/loop, which the line-based
analysis requires; the pass refuses a function it cannot classify.
Usage: spu_regcache.py <spu dir>
"""
import os, re, sys

GPR = re.compile(r'ctx->gpr\[(\d+)\]')
FHEAD = re.compile(r'^(?:static\s+)?void\s+\w+\(spu_context\*\s*ctx\)\s*\{\s*$')
ASSIGN = re.compile(r'ctx->gpr\[(\d+)\]\s*=(?!=)')
NORET = re.compile(r'\breturn\b|\bSPU_RET\b|\bSPU_RET_REG\b|\bSPU_FAST_XFER\b|\bSPU_TAILCALL\b')

def is_pure(s):
    t = GPR.sub('', s).replace('spu_ls_read128(ctx,', '').replace('spu_ls_write128(ctx,', '')
    # kz2: spu_unsupported() can halt the SPU; keep ctx exact around it.
    return not re.search(r'\bctx\b|\breturn\b|\bextern\b|g_spu_|\bSPU_|^\s*#|spu_unsupported', t)

def split_if(s):
    """'if (C) { B }' on one line -> (indent, C, B) or None."""
    st = s.lstrip()
    if not st.startswith('if ('): return None
    ind = s[:len(s) - len(st)]
    i = 3; dep = 0
    for j in range(i, len(st)):
        if st[j] == '(': dep += 1
        elif st[j] == ')':
            dep -= 1
            if dep == 0: break
    else: return None
    cond = st[4:j]; rest = st[j + 1:].rstrip()
    if not (rest.startswith(' {') and rest.endswith('}')): return None
    body = rest[2:-1]
    d = 0
    for ch in body:
        if ch == '{': d += 1
        elif ch == '}':
            d -= 1
            if d < 0: return None
    if d != 0: return None
    return ind, cond, body

def loc(s): return GPR.sub(lambda m: 'r' + m.group(1), s)

LABEL = re.compile(r'^\s*(loc_[0-9A-Fa-f]+):\s*$')
GOTO = re.compile(r'^\s*(?:if \((.*)\) )?goto (loc_[0-9A-Fa-f]+);\s*$')

def xform_func(body, dirty=True):
    """v95: as v94, but each spill writes back only the registers that MAY be dirty at that point
    (written by pure code since the last full reload, forward may-analysis over the function's
    labels/gotos). After an impure line every used register is reloaded, so locals == ctx there
    and the dirty set is empty; spilling a clean register is a value no-op, so the analysis can
    only remove stores that write back the value ctx already holds."""
    kinds = []          # (kind, payload)
    for s in body:
        if not s.strip() or is_pure(s):
            m = GOTO.match(s)
            kinds.append(('g', (s, m.group(2), m.group(1) is not None)) if m else ('p', s)); continue
        if re.search(r'\bgoto\b', s): raise ValueError('impure goto: ' + s)
        sp = split_if(s)
        if sp and is_pure('if (' + sp[1] + ')'): kinds.append(('if', sp)); continue
        kinds.append(('x', s))
    used, wr = set(), set()
    for k, v in kinds:
        if k in ('p', 'g'):
            t = v if k == 'p' else v[0]
            used.update(int(n) for n in GPR.findall(t))
            wr.update(int(n) for n in ASSIGN.findall(t))
        elif k == 'if':
            used.update(int(n) for n in GPR.findall(v[1]))
    LSD = '\tuint8_t* spu_rc_ls = ctx->ls;  /* v95: LS base (spu_helpers_vec.h LS accessors) */'
    if not used:
        if any('spu_ls_' in l for l in body): return [LSD] + list(body), 0
        return body, 0
    U = sorted(used); W = sorted(wr)
    n = len(kinds)
    def xret(v): return (not re.search(r'\bif\b', v)) and NORET.search(v)
    # ---- forward may-dirty analysis ----
    labels = {}
    for i, (k, v) in enumerate(kinds):
        if k == 'p':
            m = LABEL.match(v)
            if m: labels[m.group(1)] = i
    succ = []
    for i, (k, v) in enumerate(kinds):
        nx = [i + 1] if i + 1 <= n else []
        if k == 'g':
            if v[1] not in labels: raise ValueError('goto to unknown label ' + v[1])
            succ.append(([i + 1] if v[2] else []) + [labels[v[1]]])
        elif k == 'x' and xret(v): succ.append([])
        else: succ.append(nx)
    din = [None] * (n + 1); din[0] = frozenset()
    work = [0]
    while work:
        i = work.pop()
        if i >= n: continue
        k, v = kinds[i]; d = din[i]
        if k == 'p': out = d | frozenset(int(x) for x in ASSIGN.findall(v))
        elif k == 'x': out = frozenset()
        else: out = d          # 'g' (reads only), 'if' (taken path reloads all or returns)
        for j in succ[i]:
            if din[j] is None: din[j] = out; work.append(j)
            elif not out <= din[j]: din[j] = din[j] | out; work.append(j)
    # ---- backward liveness of the LOCALS (v95): a spill reads its registers, so an impure line
    # only needs to reload what is read -- by pure code or by a later spill -- before being
    # rewritten. Registers it does not reload hold a stale local that nothing observes.
    def uses_defs(t):
        u = set(int(x) for x in GPR.findall(t))
        d = set(int(x) for x in ASSIGN.findall(t))
        if len(d) == 1 and t.count(';') == 1:
            m = ASSIGN.search(t); rhs = t[m.end():]
            u = set(int(x) for x in GPR.findall(t[:m.start()])) | set(int(x) for x in GPR.findall(rhs))
            return u, d
        return u, set()                     # several statements: no kill (conservative)
    def dset(i): return set(din[i]) if (dirty and din[i] is not None) else (set(W) if din[i] is not None else set())
    lin = [set() for _ in range(n + 1)]
    lin[n] = dset(n)
    ud = [uses_defs(v if k == 'p' else v[0]) if k in ('p', 'g') else None for k, v in kinds]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            k, v = kinds[i]
            lo = set()
            for j in succ[i]: lo |= lin[j]
            if k == 'p': u, d = ud[i]; new = u | (lo - d)
            elif k == 'g': new = ud[i][0] | lo
            elif k == 'if':
                new = set(int(x) for x in GPR.findall(v[1])) | dset(i) | lo
            else: new = dset(i)
            if new != lin[i]: lin[i] = new; changed = True
    def reload_of(i):
        if not LIVE: return reload
        lo = set()
        for j in succ[i]: lo |= lin[j]
        return ' '.join('r%d = ctx->gpr[%d];' % (r, r) for r in sorted(lo & used))
    def spill_of(i):
        regs = W if not dirty else sorted(din[i] or ())
        return ' '.join('ctx->gpr[%d] = r%d;' % (r, r) for r in regs)
    reload = ' '.join('r%d = ctx->gpr[%d];' % (r, r) for r in U)
    out = [LSD, '\tu128 ' + ', '.join('r%d = ctx->gpr[%d]' % (r, r) for r in U) + ';']
    for i, (k, v) in enumerate(kinds):
        if k == 'p': out.append(loc(v))
        elif k == 'g': out.append(loc(v[0]))
        elif k == 'if':
            ind, c, b = v
            sp = spill_of(i)
            tail = '' if NORET.search(b) else ' ' + reload + ' spu_rc_ls = ctx->ls;'
            out.append('%sif (%s) { %s%s%s }' % (ind, loc(c), sp + ' ' if sp else '', b.strip(), tail))
        else:
            ind = v[:len(v) - len(v.lstrip())]
            sp = spill_of(i)
            if sp: out.append(ind + sp)
            out.append(v)
            if not xret(v):
                rl = reload_of(i)
                out.append(ind + (rl + ' ' if rl else '') + 'spu_rc_ls = ctx->ls;')
    if din[n] is not None:     # reachable fall-through off the end of the function
        sp = ' '.join('ctx->gpr[%d] = r%d;' % (r, r) for r in (sorted(din[n]) if dirty else W))
        if sp: out.append('\t' + sp)
    return out, 1

def xform_file(src):
    lines = src.split('\n'); out = []; i = 0; nf = 0
    while i < len(lines):
        s = lines[i]; out.append(s); i += 1
        if FHEAD.match(s):
            j = i
            while lines[j] != '}': j += 1
            body, n = xform_func(lines[i:j], DIRTY); nf += n
            out.extend(body); i = j
    return '\n'.join(out), nf

DIRTY = True
LIVE = True

def twin(dst, src):
    """v95: rename the transformed image's symbols to <P>_rc_spu_* so it links NEXT TO the original
    spu_recomp.c, declare them, and hand the runtime the (original, rc) pairs from a constructor
    (spu_rc_add_pairs in runtime/spu/spu_channels.c; SPU_REGCACHE picks the copy at run time)."""
    names = re.findall(r'^(?:static\s+)?void\s+(\w+)\(spu_context\*\s*ctx\)\s*\{\s*$', src, re.M)
    if not names: return dst, 0
    m = re.match(r'(\w+)_spu_func_[0-9A-Fa-f_]+$', names[0])
    P = m.group(1)
    if any(not n.startswith(P + '_spu_') for n in names): raise ValueError('mixed prefixes in ' + P)
    statics = set(re.findall(r'^static\s+void\s+(\w+)\(spu_context\*', src, re.M))
    ren = re.compile(r'\b' + re.escape(P) + r'_spu_(func_[0-9A-Fa-f_]+|recomp_register)\b')
    out = ren.sub(lambda mm: P + '_rc_spu_' + mm.group(1), dst)
    lines = out.split('\n')
    first = next(i for i, l in enumerate(lines) if FHEAD.match(l))
    decl = ['/* v95: register-cached twins of this image\'s functions */']
    decl += ['%svoid %s_rc_spu_%s(spu_context* ctx);' % ('static ' if n in statics else '', P, n[len(P) + 5:]) for n in names]
    lines[first:first] = decl + ['']
    pub = [n for n in names if n not in statics]
    tail = ['', '/* v95: (original, register-cached) pairs for the runtime registry. */',
            'extern void spu_rc_add_pairs(const void* const* pairs, unsigned n);',
            'static const void* const s_spu_rc_pairs[] = {']
    tail += ['    (const void*)%s, (const void*)%s_rc_spu_%s,' % (n, P, n[len(P) + 5:]) for n in pub]
    tail += ['};', '__attribute__((constructor)) static void %s_rc_pairs_ctor(void)' % P,
             '{', '    spu_rc_add_pairs(s_spu_rc_pairs, %du);' % len(pub), '}', '']
    return '\n'.join(lines + tail), len(pub)

def main():
    global DIRTY, LIVE
    root = sys.argv[1]; tot = 0
    if '--no-dirty' in sys.argv: DIRTY = False
    if '--no-live' in sys.argv: LIVE = False
    plain = '--twin' not in sys.argv       # kz2: same names, replaces spu_recomp.c
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d, 'spu_recomp.c')
        if not os.path.isfile(p): continue
        if d == 'wwsplug':
            # plug_lift.py writes this folder's own spu_recomp_rc.c (one symbol prefix per plugin, wwsplugNN_).
            # A fresh pipeline runs this step before the plugin step, so the folder only exists here when the
            # pipeline is re-run over an existing install (--regen, or an update that regenerates): skip it,
            # exactly as on a fresh run.
            print('%-40s skipped (lifted by plug_lift.py)' % d)
            continue
        src = open(p, encoding='utf-8', errors='surrogateescape').read()
        dst, nf = xform_file(src)
        if not plain: dst, _ = twin(dst, src)
        if '--vec' in sys.argv:             # kz2: off -- no spu_helpers_vec.h here
            dst = re.sub(r'^(#include "[./]*(?:runtime/spu/)?spu_helpers\.h"[^\n]*)$',
                         r'\1\n#include "spu_helpers_vec.h"', dst, count=1, flags=re.M)
            if 'spu_helpers_vec.h' not in dst: raise ValueError('no spu_helpers.h include in ' + d)
        hdr = '/* GENERATED by ps3recomp/tools/spu_regcache.py from spu_recomp.c -- v95 register caching */\n'
        open(os.path.join(root, d, 'spu_recomp_rc.c'), 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(hdr + dst)
        tot += nf
        print('%-40s %5d functions cached' % (d, nf))
    print('total', tot)

if __name__ == '__main__': main()
