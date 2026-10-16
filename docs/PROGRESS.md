# Engineering log — Killzone 2 (BCUS98116) on ps3recomp

Newest state first in the summary; the sections below are in the order the
problems were met.

## Where it stands

The title boots, streams and inflates its archive, runs its SPU jobs, and
walks its own front end: language select -> gamma correction -> main-menu
assets (interface, speech, 23 MB of music) -> save warning -> the intro
movie page. The display buffer shows the menus (background and button
prompts; text is still missing). Next walls: menu text, the intro FMV
(cellVdec), and the Vulkan path's speed (~0.5 fps; the null backend runs the
game logic at full speed).

## 1. Pipeline

| Step | Tool | Output |
|---|---|---|
| Loader metadata | `ppu_loader.py` | 34,145 OPD functions, 342 imports / 23 libraries |
| PPU lift | `ppu_lifter.py --code-end 0xbba45c --chunk-lines 150000` | 67,196 functions, 41 chunks |
| SPU images | `extract_spu_images.py` + `build_spu_workloads.py` | 23 images |
| WWS policy module | raw 0x2E80 bytes at guest 0xD8B000, LS 0xA00 | fingerprint `DDD59DC528B4CED6` |
| WWS job modules | `tools/extract_jobmods_elf.py` + `lift_jobmods.py` | 36 C0DEC0DE modules at LS 0x4000 |
| Build | clang -O1 for the chunks | ~3 min at -j8, ~0.6 GB per job |

Killzone 2 links its WWS job modules into the EBOOT rather than shipping an
SDATA archive, so ps3recomp's `extract_jobmods.py` had nothing to open; the
port's extractor scans the ELF for the `0xC0DEC0DE` marker. `imageSize`
includes bss, so each image is clamped to the start of the next module.

## 2. FIOS deadlock — `sys_lwcond_t` layout (patch 0001)

First boot hung with the FIOS media threads blocked on the scheduler's
lightweight mutex. The runtime wrote `sys_lwcond_t` as `{be64 lwmutex; u32
queue}`; the PS3 ABI is `{be32 lwmutex; be32 queue}`. FIOS reads the mutex
pointer out of its condvar inline, found 0 and never released the lock it
was waiting under. With the ABI layout FIOS comes up, opens
`data_ps3.psarc` and reads its 436 KB TOC.

## 3. Edge Zlib on the host

FIOS hands every compressed psarc block to Sony's `edgezlib_inflate_task`
(SPU image at 0xCC3500) through `_cellSpursLFQueuePushBody`, which the
runtime does not implement, then waits on a SPURS event flag. The queue
element was captured from the title:

```
+00 src EA (raw deflate, past the 2-byte zlib header)   +04 dst EA
+08 compressed size   +0C expected size   +10 work-to-do counter EA | inflate bit
+14 event flag EA     +18 flag bits       +1C skip begin / skip end
```

`src/kz2_hle.cpp` services it synchronously with zlib, decrements the
counter and sets the flag. The archive's lumps load.

## 4. `lwax` switch tables (patch 0002)

The main thread called an address in the middle of `func_003AC15C`:
`lwax r0,r9,r0; add r0,r0,r9; mtctr r0; bctr` is an offset jump table
whose load is the sign-extending `lwax`, which the lifter did not accept as
the indexed load. Allowing it found 13 more dispatchers (221 → 234).

## 5. cellFont SDK 3.x entry points

`cellFontInitializeWithRevision` and
`cellFontInitLibraryFreeTypeWithRevision` are not registered; unregistered
imports return 0 without running, so `cellFontInit` never happened and
`cellFontOpenFontset` got `lib = NULL`. Both now forward to the runtime;
`cellFontGetCharGlyphMetrics` forwards to the render-metrics query.

## 6. The front end renders, the display does not

`RSX_ENGINE_PASS_DUMP` on the Vulkan backend shows, per guest frame:

1. surface `0x7B0000` (1280x720, pitch 10240): clear + draw, four times, with
   swizzled textures at `0x318AB00`, `0x31AAB80`, `0x7ACB80`, `0x31B0100` —
   the Helghast sky with dropships (`docs/media/first-render-menu-sky.png`);
2. surface = display buffer N: clear colour, one full-screen quad whose
   fragment program is `tex0 * col0`, `tex0` = display buffer N-1.

No NV3089/NV0039/NV308A transfer and no SPU job reads `0x7B0000`, so the sky
never reaches the display. Killzone 2 composed its frames with SPU
post-processing (`jm_gfx` and friends); none of the `jm_*` WWS workloads has
received a job yet (their workload data stays zero), consistent with the
title still being in its boot sequence.

## 7. Where the boot idles

* The main thread spends ~100% of its time in the frame pacer
  (`func_00047170`, polling a GCM label at `0x20002008`).
* Per frame it calls only `cellPadGetData`, `cellSysutilCheckCallback`,
  `cellGcmSetUserHandler`, `_cellGcmSetFlipCommand`.
* The last new import it ever calls is `sceNpTrophyGetTrophyUnlockState`.
* The SVO bootstrap state (`func_002B7A58`, references `SVO/SVOBootstrap`)
  runs once; the offline SVML loader (`func_002B28A0`, references
  `Offline SVML` / `source:interface/svo/svml/`) never runs.
* Pressing START/CROSS (`PAD_SCRIPT`) changes nothing.

Resolved in sections 8-11.

## 8. SPU jobs actually run

The WWS policy module finishes a job load in its SPU interrupt handler at
LS 0xA2C, which builds a register save/restore stub at 0x3FEC0 and returns
through 0xA84/0xA90/0xAD0. Nothing branches there statically, so the lift
had no functions at those addresses; the handler ran in the fallback
interpreter and the PM ended mid-handler. With them as `--extra-funcs` the
audio jobs stream into LS 0x4000 and dispatch to their lifted overlays.

## 9. The SVO browser could not parse a URL (patch 0004)

Found by decompiling the chain with Ghidra (`tools/decompile.sh`) and
following it in gdb: navigate -> page stack (0x0117E740) -> engine `Load`
(`func_003AB808`, vtable slot 5) -> `FUN_003AA120` -> loader factory
(`FUN_0039C164`), which returned NULL because no protocol handler accepted
the parsed scheme -- it was empty. The URL parser copies the scheme with an
inlined `lswi r5,r31,15; stswi r5,r11,15`, which the lifter emitted as
`/* TODO */`. Implementing lswi/stswi (and four unsigned VMX ops left as
TODO) let the browser load `index.jsp` and move on to language select.

## 10. Frames stopped: an unhandled fence method (patch 0005)

The streaming VRAM ring is fenced with NV4097 `0x1D74`
TEXTURE_READ_SEMAPHORE_RELEASE on label 0x48. The runtime handled only the
back-end form `0x1D70`, so the label froze and the ring allocator
(`func_000461C0` / `func_00A45844`) waited forever.

## 11. Two FIFO problems

- The title parks the RSX on a self-JUMP in the middle of each frame and
  patches it when the frame is complete; the walker resynced to `put` at
  once and skipped the frame's tail, fence included, then looped over stale
  segments. `GCM_PARK_GRACE_MS` waits for the patch (boot_main sets 3 s).
- The title's mmapper heap (handed out upward from 0x11000000) grew across
  the runtime's GCM window at 0x20000000; its allocator wrote strings over
  the control register and `put` went odd. The window now lives at
  0xE8000000 (`ppu_hle_inject_base`), the HLE scratch heap at 0xE9000000.

Result: frames flow, the display buffer shows the language-select menu
(`docs/media/first-menu-frame.png`), and the front end can be driven with
`PAD_SCRIPT` into the main menu.

## Diagnostics added

| Tool | Use |
|---|---|
| `tools/snapshot.sh [sec]` | run under gdb, dump every thread's stack twice |
| `tools/sample.sh warm count interval` | sample the guest main thread |
| `tools/import_audit.py` | firmware imports with no HLE handler |
| `tools/psarc_map.py` | which psarc members a run read (`PS3_FSLOG_READS=3`) |
| `KZ2_PEEK=ea,...` | print a guest word and the word at +8 of what it points to, every second |
| `KZ2_DUMPAT=sec:ea:len,...` | timed guest hex dumps |
| `-DKZ2_FTRACE=ON`, `KZ2_FTRACE=start:len` | guest functions the main thread enters in a window |
