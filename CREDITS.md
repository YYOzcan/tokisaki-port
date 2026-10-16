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

It is included as a git submodule at `external/ps3recomp`, pinned to v0.14.0
(`a2c2010`). Clone with `git clone --recursive`, or run
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
