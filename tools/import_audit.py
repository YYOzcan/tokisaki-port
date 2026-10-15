#!/usr/bin/env python3
"""Tokisaki Port -- list Killzone 2 firmware imports with no HLE handler.

Compares out/imports_named.json (tools/gen_imports.py) against the NIDs in the
generated build/ppu_hle_nids.cpp and the context handlers the PPU scaffold
registers by name (ps3_compute_nid("...")) plus src/kz2_hle.cpp.

    .venv/bin/python -I tools/import_audit.py
"""
import json, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "external", "ps3recomp", "tools"))
from nid_database import compute_nid  # noqa: E402

have = set()
gen = open(os.path.join(ROOT, "build", "ppu_hle_nids.cpp")).read()
have |= {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})", gen)}
for d in ("external/ps3recomp/runtime/ppu", "src"):
    for f in os.listdir(os.path.join(ROOT, d)):
        if f.endswith((".cpp", ".c")):
            src = open(os.path.join(ROOT, d, f), errors="replace").read()
            have |= {compute_nid(n) for n in re.findall(r'ps3_compute_nid\("([^"]+)"\)', src)}
            have |= {int(x, 16) for x in re.findall(r"register_ctx\(0x([0-9A-Fa-f]{8})", src)}

imports = json.load(open(os.path.join(ROOT, "out", "imports_named.json")))
missing = [i for i in imports if int(i["nid"], 16) not in have]
for i in sorted(missing, key=lambda i: (i["lib"], i.get("name") or "")):
    print(f'{i["lib"]:<22} {i["nid"]}  {i.get("name") or "?"}')
print(f"\n{len(missing)} of {len(imports)} imports have no handler", file=sys.stderr)
