# Credits

Tokisaki Port is a thin, title-specific layer on top of other people's work.
Most of what runs is not ours. This file says what is whose.

## The toolkit and runtime — ps3recomp

**[sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp)** by Ned Heller
(sp00nznet) and its contributors (see its `CONTRIBUTORS.md`), MIT licensed.

Everything below comes from it, unmodified unless listed in `patches/ps3recomp/`:

- the PPU loader, lifter and the 6 million lines of C++ it generates from the
  disc's `EBOOT.ELF` (`tools/ppu_loader.py`, `tools/ppu_lifter.py`);
- the SPU disassembler, lifter, job-module and workload tooling
  (`spu_lifter.py`, `build_spu_workloads.py`, `lift_jobmods.py`, `wrap_spu_elf.py`);
- the entire runtime: the guest address space, PPU thread model, lv2 syscalls,
  ~95 HLE system libraries (cellGcmSys, cellSpurs, cellFs, cellSaveData,
  cellFont, cellAudio, ...), the SPU runtime and the WWS job-manager support,
  the RSX command processor and the Vulkan backend;
- the project template whose `main.cpp` our `src/boot_main.cpp` starts from.

It is included as a git submodule at `external/ps3recomp`, pinned to v0.15.0
(`39a3629`; the v0.14.0 patch series is kept in `patches/ps3recomp-v0.14/`). Clone with `git clone --recursive`, or run
`git submodule update --init` after a plain clone.

## The Linux boot harness — Twisted Metal port

`src/boot_main.cpp` is adapted from two MIT-licensed sources:

- ps3recomp's `templates/project/main.cpp` (overall boot order, VFS root,
  guest-callback hook, Windows path);
- the Linux harness of **[Kurtisone/twistedmetal](https://github.com/Kurtisone/twistedmetal)**
  (`linux/boot` branch, `src/boot_main.cpp`) by **Kurtisone**: the POSIX
  frame clock (1 ms FIFO drain + 60 Hz `cellGcm_request_tick` + present on
  `cellGcm_take_flip_pending_synced`), the 256 MB host stack for the guest main
  thread, the Vulkan/null backend selection and the `mmap(MAP_NORESERVE)` guest
  space.

Kurtisone's bring-up notes for Twisted Metal (`docs/bringup.md`,
`docs/first-pixels.md`) also showed the way on two problems Killzone 2 shares:
FIOS deadlocking on the `sys_lwcond_t` layout, and servicing Sony's Edge Zlib
SPURS task on the host. Our fixes for both were written for this title, but
the diagnosis was theirs first.

## Bugs others found first

The lifter fixes in `patches/ps3recomp/0002` (`lwax` switch tables) and
`0003` (carry-form register definitions in the callee-saved analysis) were
written independently, but only after reading the *names* of open branches in
**[undecuple/ps3recomp](https://github.com/undecuple/ps3recomp)**
(`fix/lifter-lwax-jump-tables`, `lifter-cs-carry-forms`), the fork behind the
Killzone 3 port. undecuple found these bugs before we did; their own patches
may differ from ours and should be preferred once merged upstream.

They have since been merged: ps3recomp **v0.15.0** contains its own versions
of `0001` (lwcond layout), `0002`, `0003` and the `0x1D74` label of `0005`
(the park grace there is erikomis' inFAMOUS 2 fix). The submodule is still on
v0.14.0, so the patches stay until it is moved up.

The policy-module argument layout in `0010` (`r4` = the u64 workload argument
in the preferred doubleword, word 0 = high half) is the same fix as
**undecuple's** upstream PR #310 ("pass the policy module's u64 argument in the
preferred doubleword"), re-applied here by hand because of the pinned
submodule. It is what lets Killzone 2's WWS job lists complete.

## SPU register caching — inFAMOUS PC port

`tools/spu_regcache.py` is taken from **[whompay/inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn)**
(the inFAMOUS PC port by **whompay**), `ps3recomp/tools/spu_regcache.py`, which
ships in that repository's copy of ps3recomp under ps3recomp's MIT license.
It rewrites the lifted SPU code so each function keeps the SPU registers in
locals instead of the context in memory. Our changes are marked `kz2:` in the
file: the output replaces `spu_recomp.c` under the same names (CMake
`KZ2_SPU_REGCACHE`), without their run-time twin selection and their
`spu_helpers_vec.h`. Nothing else from that repository is used; its
title-specific code carries no license and is not copied.

## Killzone 3 PC port — Project Helghast

**[undecuple/project-helghast](https://github.com/undecuple/project-helghast)**
(Killzone 3 by static recompilation, on ps3recomp v0.18 with a Direct3D 12
backend; MIT) is a port of the next game of the same engine. Its patch notes
are the source of these changes here, re-written for this port's ps3recomp
v0.15 series (patch 0053): SSE4.1/AVX2 forms of the SPU integer, mask,
generate-controls, float compare and estimate helpers (its patches 1097,
1131, 1132 -- ours are new code, bit-identical to this runtime's reference
loops, `runtime/spu/tests/test_spu_helpers_x86.c`), the walker's wait at an
SPU-patched park (1087) and the overlay source index (1090). Its Direct3D 12,
Windows and Killzone 3 specific code is not used.

## Other software

- **zlib** — host-side inflate for Edge Zlib requests.
- **stb_truetype** (via ps3recomp) — glyph metrics from the firmware fonts.
- **Ghidra** (NSA) — decompilation during reverse engineering (`tools/ghidra/`).
- **RPCS3** — the reference that ps3recomp's HLE behaviour is checked against;
  the firmware fonts are read from the user's own RPCS3 `dev_flash`. No RPCS3
  code is used.

## Game data

Killzone 2 © Sony Interactive Entertainment, developed by Guerrilla Games.
No game code, data, keys or firmware are in this repository: the lift and all
assets are produced locally from the user's own disc and firmware.

## How this repository was written

The port is developed by **YYOzcan** with an AI coding agent (Anthropic's
Claude); commits it wrote carry a `Co-Authored-By: Claude` trailer. The
engineering log in `docs/PROGRESS.md` records what was found and changed, and
why.

The verbatim MIT license texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Killzone 3 port fix list — undecuple

The Killzone 3 port's write-up of general ps3recomp fixes
([gist](https://gist.github.com/undecuple/2845432bf13e485634de122084d3fd56),
[ps3recomp #214](https://github.com/sp00nznet/ps3recomp/issues/214)), offered
under ps3recomp's MIT license. Most of it reached us through upstream v0.15.0
(VMX lane order, TLS, GCM IO table, depth textures, texture gamma, the WWS
policy-module argument). From the part not yet upstream we implemented, from
its description and not its code, R6 (the policy module's `selectWorkload`
answers the running workload; opt-in `SPURS_PM_SELECT_CURRENT=1`).
