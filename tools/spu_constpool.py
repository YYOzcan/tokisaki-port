#!/usr/bin/env python3
"""Tokisaki Port -- a lifted SPU function specialised on its job module's constants.

A software-pipelined SPU loop loads its shuffle controls and scale factors
from the module's constant pool once, before the loop, and the lifted C reads
them from the local store into ctx->gpr: memory the compiler cannot see
through, so every shufb in the loop computes its byte indices and checks for
the special selectors again, every iteration. This makes a copy of the
function in which

  - the registers are C locals (tools/spu_regpromote.py's rewrite), and
  - each load of the local store at a fixed address before the function's
    first label (code that runs once, at entry) is the quadword the module
    image holds there, a constant,

and turns the function itself into a guard: when the local store holds those
quadwords (compared at entry, before any of them is read) it runs the copy,
otherwise the original, renamed. Either way the same values are computed;
SPU_CP_CHECK=1 runs both and compares them (spu_cp_verify, for functions with
no channel I/O or calls).
    tools/spu_constpool.py <spu_recomp.c> <module.bin> <ls base> <function name> ..."""
import re, sys, importlib.util, pathlib

spec = importlib.util.spec_from_file_location('rp', pathlib.Path(__file__).with_name('spu_regpromote.py'))
rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)

LOAD = re.compile(r'^(\s*)ctx->gpr\[(\d+)\] = spu_ls_read128\(ctx, (0x[0-9A-Fa-f]+)\);\s*$')


def main():
    path, binpath, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    names = sys.argv[4:]
    img = open(binpath, 'rb').read()
    src = open(path).read().split('\n')
    out, i, done = [], 0, 0
    while i < len(src):
        m = rp.FUNC.match(src[i])
        if not m or m.group(2) not in names:
            out.append(src[i]); i += 1; continue
        name = m.group(2)
        j = i + 1
        while src[j] != '}':
            j += 1
        body = src[i + 1:j]
        consts, spec_body, in_prologue = {}, [], True
        for l in body:
            if rp.LABEL.match(l):
                in_prologue = False
            lm = LOAD.match(l) if in_prologue else None
            if lm:
                a = int(lm.group(3), 16)
                off = (a & 0x3FFF0) - base
                if 0 <= off and off + 16 <= len(img):
                    words = [int.from_bytes(img[off + 4 * k: off + 4 * k + 4], 'big') for k in range(4)]
                    consts[a & 0x3FFF0] = words
                    w = ', '.join(f'0x{x:08X}u' for x in words)
                    spec_body.append(f'{lm.group(1)}ctx->gpr[{lm.group(2)}] = (u128){{ ._u32 = {{ {w} }} }};')
                    continue
            spec_body.append(l)
        new = rp.rewrite(name + '_cp', spec_body)
        if not consts or new is None:
            out.extend(src[i:j + 1]); i = j + 1; continue
        out.append(f'static void {name}_orig(spu_context* ctx) {{')
        out.extend(body)
        out.append('}')
        out.append(f'static void {name}_cp(spu_context* ctx) {{')
        out.extend(new)
        out.append('}')
        out.append(src[i])
        checks = []
        for a, words in sorted(consts.items()):
            be = b''.join(x.to_bytes(4, 'big') for x in words)
            arr = ', '.join(f'0x{b:02X}' for b in be)
            checks.append(f'memcmp(ctx->ls + 0x{a:05X}, (const uint8_t[16]){{ {arr} }}, 16)')
        out.append('        /* tools/spu_constpool.py: the constant-pool copy when the pool is the image\'s */')
        out.append('        if (__builtin_expect(!(' + ' || '.join(checks) + '), 1)) {')
        pure = all(not rp.impure(l) or re.fullmatch(r'\s*SPU_RET\(ctx\);\s*', l) for l in body if not rp.LABEL.match(l))
        if pure:   # run twice and compared only when it touches nothing but registers and LS
            out.append(f'            if (__builtin_expect(spu_cp_check_on(), 0)) {{ spu_cp_verify(ctx, {name}_orig, {name}_cp, "{name}"); return; }}')
        out.append(f'            {name}_cp(ctx); return; }}')
        out.append(f'        {name}_orig(ctx);')
        out.append('}')
        done += 1
        i = j + 1
    open(path, 'w').write('\n'.join(out))
    print(f'{path}: {done} functions specialised')


if __name__ == '__main__':
    main()
