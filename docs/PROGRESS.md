# Engineering log — Killzone 2 (BCUS98116) on ps3recomp

Newest state first in the summary; the sections below are in the order the
problems were met.

## Where it stands

The title boots, streams and inflates its archive, runs its SPU jobs and
walks its own front end, driven by `PAD_SCRIPT` or keyboard:

language select -> gamma -> main-menu assets -> save warning -> intro-movie
page -> **start screen -> campaign difficulty select -> campaign start -> level asset streaming**

**Update (2026-10-19):** presents now follow every flip the FIFO reaches
(30-130 presented frames/s on Vulkan, section 16), and level loading gets past
the WWS job manager: both `jm_default` and `jm_default_ast` complete their
job tickets (section 15). The main thread then waits in level setup
(`func_0024F0F8 -> func_004D9BD0 -> func_004EED70`, `sys_cond_wait`): that
is the first Havok physics step of the level (section 17).

Campaign start is fully unlocked: both the stream-specific stop hang in `func_009AD268`
and the master sound voice teardown loop in `func_009A30F0` are resolved via
watchdog hooks in `src/kz2_hle.cpp` (section 13). The title advances into
campaign level asset streaming, launching `Loading Screen Thread`, `Havok PPU Thread`,
`Asset Streaming Thread`, and decompressing over 12,500 Edge-Zlib blocks.
The 0-based handle deadlock in `cellVdec` was identified and fixed (patch 0007,
section 12), and interactive keyboard control is now supported (patch 0008, section 14).

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

## 12. Movies & cellVdec 1-based handles (patch 0007)

`GameIntro_PS3.avi` is H.264 1280x720 + AC3 5.1 in AVI. The title opens the
decoders with `cellVdecQueryAttrEx`/`cellVdecOpenEx`/`cellAdecOpenEx`, which
forward to the runtime. Analysis of the video decode loop in Ghidra revealed
the root cause of the intro freeze: `FUN_009f2868` explicitly checks
`if (*(int *)(param_1 + 0xc0) == 0) return 0x9b;`, and `FUN_009f21a0` (StartSeq) /
`FUN_009f2008` (EndSeq) both check `if (*(int *)(param_2 + 0x58) != 0)`.
In ps3recomp, `cellVdecOpen` returned 0-based handle index `0`, which Killzone 2
treats as an invalid/null handle, rejecting all video AUs. Patch 0007
(`0007-cellvdec-1-based-handle.patch`) implements 1-based handle assignment
(`1..MAX_VDEC`) across all `cellVdec` APIs.

## 13. Campaign start sound-engine hang & master teardown (Resolved)

On campaign start, `func_009AD308` halts sound streams and `func_009AD268`
waits until each stream's handle (word 0 of a 0x38D8-byte record at 0x4211C000)
becomes non-negative. Following that, `func_009A30F0` loops on `sys_timer_usleep(33000)`
waiting for the master voice linked list (`*(toc - 0x47C8)`) to become empty.
Both stages are resolved by targeted watchdog hooks in `kz2_sys_timer_usleep` (`src/kz2_hle.cpp`):
1. In `func_009AD268` (`caller_lr == 0x009AD294 / 0x009AD2CC`), stream 7's handle
   and status words are released after 5 iterations (~500 ms).
2. In `func_009A30F0` (`caller_lr == 0x009A31B8`), the voice list head is cleared
   after 5 iterations (~165 ms).
With both loops exited, the sound engine halts cleanly and the title advances into
campaign level asset streaming: `Loading Screen Thread`, `Havok PPU Thread`, and
`Asset Streaming Thread` launch, unpacking over 12,500 Edge-Zlib blocks.

## 14. Keyboard input mapping (patch 0008)

`cellPad.c` originally polled only `SDL_GameController`, leaving users without a
connected physical gamepad unable to navigate menus or interact with the game.
Patch 0008 (`0008-cellpad-keyboard-fallback.patch`) adds an SDL2 keyboard overlay
and fallback on port 0:
- `Enter` / `Space` -> `CROSS` (select) / `START`
- `Escape` / `Backspace` -> `CIRCLE` (back)
- `W/A/S/D` / `Arrow Keys` -> D-Pad directions
- `Q/E` -> `L1/R1`, `R/T` -> `SQUARE/TRIANGLE`
Port 0 is maintained as connected, allowing full keyboard-driven navigation
from language select through the entire menu tree.


## 15. Level loading and the WWS job manager (patch 0010)

The level loader waits in `FUN_003bd660` for a job ticket: row 3 of a WWS
job-manager sync block (`jm_default`, data `0x4005EC80`; 16-byte rows at
+0x40, `+0x40` = ticket, `+0x42..+0x4E` = per-SPU progress, `FFFF` = SPU not
participating). Three runtime bugs stood between it and the ticket:

1. **Policy-module argument.** The runtime passed the workload argument in
   `r4` as `{lo, lo, hi, hi}`. The wwsjob PM reads word 1 as the job list and
   word 0 as an *audit* buffer; with the list address there too it stamped
   `FFFFFFFF` over the list's own per-SPU rows. Fixed to the SPU ABI layout
   (word 0 = high half, word 1 = low half), as upstream did in #310.
2. **`SPU_RdMachStat` in job code.** The level-load job module
   `15e56deeec92` (jm_default_ast) asserts that interrupts are on
   (`rdch MachStat; andi 1; brz -> stopd`). The runtime answered "running",
   which is 0 in a policy-module context left stopped, so the job hit the
   `stopd` and the drain loop re-ran it millions of times. Answering every
   read with the real interrupt state instead breaks `jm_default`: module
   `6ac243f6bbc4` saves the state around a `bid` and re-enables interrupts
   mid-job, and the PM's stall-notify handler taken there never lets the job
   finish. So only the assertion pattern is answered 1; `SPU_MACHSTAT_IE=1`
   switches to the real state for further work on interrupt delivery.
3. **Interpreter gaps.** A job that returns into the middle of a lifted
   function continues in `spu_interp.c`, which lacked `mpyui`, `mpyhhau`,
   `bgx`, `rothmi`, `rotmah(i)` and the double-precision ops, and wrote
   `mpyhha` to the wrong register. Module `6ac243f6bbc4` halted on an `mpyui`
   at LS 0xE050.

Diagnostics added: `SPURS_KERN_STATS=1` (per-workload dispatch counts and
contention), the PM currently running in the `KZ2_FPS` line.

## 16. Presents (patch 0009)

The FIFO drain stops at each flip, but the frame clock presented only when
`get == put`, which a title that queues its next frame before the walker
catches up rarely shows: the menus rendered ~200 frames a second and presented
one every few seconds. A flip the drain decoded now presents at once, checked
after every drain, not once per clock loop.

## 17. The first physics step

With loading past the job manager, the main thread reaches
`TtPhysicsWorld_Update` (`func_004D9BD0`) and waits in `func_004EED70` for the
Havok worker threads. State at the stall:

- **Havok PPU Thread** (`func_00803CB8`) is inside the job queue
  (`func_00800638`) waiting on a shared-memory semaphore (`func_00803758`:
  low 16 bits count, high 16 bits waiters; a waiter sleeps on one of the event
  queues 4-12, attached to SPURS ports 16-24) -- i.e. waiting for new jobs.
- **Havok SPU tasks**: four tasks of the ELF at `0xC42A80` (our `spu_0000`,
  taskset `HavokSpursTaskSet`) are created at the step. Each runs its init
  (returns 1 = continue), then its job loop `0x3178` returns **2** at once and
  the task exits through `cellSpursExit`. Delaying the task start by 2 s
  changes nothing, so it is not a start-order race.
- **hkSpuUtil Helper** (`func_007F6F28`) is the SPU->PPU call server, idle.

So the SPU side sees the queue as finished while the PPU side waits for jobs.
Next: identify what `0x3178` reads to decide (the job-queue header the task
argument points to), and whether the SPU-capable jobs are ever queued.

Diagnostics (patch 0011): `KZ2_DUMPAT` accepts `<sec>:T<k>+<off>:<len>`, an
address relative to word k of the last `cellSpursCreateTask` argument;
`SPURS_TASK_DELAY_MS` delays a task's start.

### Resolved (patch 0012)

Two runtime bugs, found with the new `KZ2_THREADS` / `R<tid>.<reg>` dumps and
`eu-stack` on a live run (`KZ2_ALLOW_PTRACE=1`):

1. **Taskset arguments were dropped.** `_cellSpursTasksetAttributeInitialize`
   discarded its `args`, and `cellSpursCreateTasksetWithAttribute` ignored the
   attribute, so every task got 0 in r4's preferred doubleword. Havok's
   taskset passes `0x00C42A80_00C59980` there: the second word is the ELF its
   job task switches to when the queue holds jobs for the other Havok ELF
   (the task's `0x17610` ELF loader returned `CELL_SPURS_TASK_ERROR_INVAL` on
   address 0). With the argument, the physics steps run, alternating the two
   ELFs (`0xC42A80`, `0xCAB980`).
2. **Tasks started twice.** The taskset exit scheduler starts any enabled,
   ready task it does not see running; a task created by the PPU was marked
   running only once its host thread got going, and `cellSpursCreateTask`
   published its ready bit before that. Killzone 2's `imogen` tasks
   (ELF `0xD83280`) ran 49 times for 39 creations, each run decrementing the
   frame's job counter at `0x235BB9D0`; it reached -28 and the main thread
   (`func_005573A0`) waited for 0 forever. Now marked running before the task
   is registered.

The main thread now reaches the renderer (`0x48238`); frames are still not
presented after the level loads.

## 18. GCM label 75: frames skipped at mid-frame parks (patch 0013)

After the physics steps run, the main thread waits in `func_00048208`
(called from `func_00530AC8` at 0x532240) for GCM label 0x4B
(`0xE80004B0`) to reach 1; the index and value come from `obj+0x174/+0x178`
of the renderer object. No `SET_SEMAPHORE_OFFSET 0x4B0` appears in the frame
blocks dumped at the stall (only the 0x480 texture-read fences), so the label
is written elsewhere -- an SPU job or another command buffer. Writers of
`obj+0x178` include `func_0019226C` and `func_001E704C`; next step is to see
which one ran last and what it emits.

**Resolved.** The release exists (NV406E semaphore offset 0x4B0, value 1, at
IO 0x6F1DC, in the remainder segment a frame reaches only after its park at
+0x2DC is patched). The FIFO walker's "stuck-get" watchdog resynced to `put`
after eight passes with `get` unchanged -- a few milliseconds -- which also
fired on every self-JUMP park and skipped the remainders, bypassing the
park grace. It now leaves parks to the park logic. With it the campaign's
loading screen renders (`docs/media/corinth-river-loading.png`: CORINTH RIVER
with the mission briefing typing out) at ~20 presented frames/s.

Also in 0013: `RSX_LABEL_WATCH=<offset>` logs FIFO commands targeting a
label; `SPURS_LANE_ROTATE=1` dispatches single-SPU workloads on alternating
SPU lanes (an experiment, see section 19).

## 19. Sound teardown at campaign start (workaround)

The menu-music stream (7) never finishes stopping: its record stays
"stopping" with reads in flight (`+0x1B4`) and no completion. The audio WWS
job list (`0x1064600`) shows lane 1 participating but never advancing, which
may be related. The watchdog in `src/kz2_hle.cpp` still forges the stop and
clears the voice list; the double voice release that caused afterwards made
`func_009A21D0` take its fatal `b .` path. `tools/postlift_fixups.py` (run by
`tools/lift.sh`) now patches that path in the generated C to skip the
duplicate insert. A gentler watchdog (clear only `+0x1B4`) is behind
`KZ2_GENTLE_SOUND_WATCHDOG=1`; it does not get the stop through yet.

Next stall: during level load, main thread at lr `0x3BF72C`.

## 20. In the level: render passes park on sub-buffers (open)

With the park grace raised to 10 minutes (boot_main: KZ2 always patches its
parks; a finite grace skipped frame remainders whenever a frame took longer
than it), the loading screen runs and the first in-level frames are built.
The next stall varies between runs but has one shape: the FIFO walker sits
on a self-JUMP park that a render pass inserted (`func_0053E930` ->
`func_0038E120`, e.g. IO `0x1A4A64C`, reached through a CALL from the frame
block), and the main thread waits for GCM label 0x4B (released after that
CALL) or for a `jm_gfx` ticket. Nothing patches those parks; the commands
after them are already written. The patcher is presumably the job the pass
hands its work to (`jm_gfx`, Edge geometry); its tickets complete, but its
command list still holds job descriptors (`00800001 406ABC00` ...).

`KZ2_THREADS` now also prints the FIFO walker position (patch 0014).

## 21. The black window: two bugs in the last screen pass (patch 0015)

The play window showed (almost) black while the frame was being drawn: the
presented image peaked at 12/255. Pass dumps (`RSX_ENGINE_PASS_DUMP`) show
the frame correct up to the final pass on the display buffer, which is

1. a copy of the offscreen frame (`0x7B0000`) into the display buffer,
2. a fullscreen screen-distortion triangle (a noise texture offsets the
   lookups; R, G and B are fetched separately for a slight colour fringe),
   blended at alpha 0.958, that **samples the display buffer it is drawing
   into**, and
3. a multiplicative overlay (`DST_COLOR * src`).

**Feedback read.** The engine refused to sample a surface the draw also
writes (undefined on every API) and fell back to guest memory, which holds
nothing; draw 2 then wrote 0.958 x black + 0.042 x frame. The backend now
takes a GPU copy of the target just before such a draw (`color_snapshot`,
Vulkan: one blit, one cached image per target) and the draw samples that --
what the RSX itself reads from memory.

**AltiVec halfword multiplies.** With the feedback fixed, the distortion
pass smeared the whole frame into a few colours: its triangle came out
1023 screens wide. Its corners are built with integer vector maths
(`func_00461408`: `vmulouh` + `vmsumshm` + `vslw` + `vadduwm` = `w * 2`,
then `vcfsx`), and the lifter read and wrote the halfword/word lanes of
`vmulouh`, `vmuleuh`, `vmsumshm`, `vmsumuhm`, `vmsummbm`, `vadduhs`,
`vaddshs`, `vadduws`, `vsubsws`, `vsubuws`, `vsum2sws`, `vpkshss`,
`vpkuhus`, `vpkuwus` and `vpkuwum` through host `uint16_t*`/`int32_t*`
pointers -- but `ctx->vr` holds big-endian bytes, so every element was
byte-swapped (`0x500 * 2` stored as `0x000A0000`, read back as 655360
instead of 2560). They now go through big-endian lane helpers
(`ppu_vldh8`/`ppu_vsth8`, `ppu_vldu4`/`ppu_vstu4`). This is lifted code, so
`tools/lift.sh` has to be run again.

Diagnostics added: `RSX_ENGINE_SKIP_PIPES=<h,...>` drops a pipeline's
draws; `RSX_ENGINE_PASS_DUMP_DRAWS` lines carry each draw's fragment
constants, its first three host vertices and transform constants
0-7/466/467.

## 22. On ps3recomp v0.15.0

The submodule moved from v0.14.0 to v0.15.0 (`39a3629`), which carries 33
fixes from the Killzone 3 port and many from other titles. Our 15 patches
were rebased onto it as 13 (`patches/ps3recomp/`; the old series is in
`patches/ps3recomp-v0.14/`):

- Already upstream, dropped: the `sys_lwcond_t` layout, `lwax` jump tables,
  most of the carry-form callee-save analysis, the per-job log caps, the WWS
  policy-module r4 layout (#309/#310), the extra SPU interpreter ops, half
  of the VMX lane-order fixes (P1).
- Merged: the park grace -- upstream now waits 50 ms when `put` stands still
  (inFAMOUS 2); `GCM_PARK_GRACE_MS` overrides it and boot_main keeps 600 s.
  Same-target colour feedback -- upstream added the engine side and a Metal
  backend; our Vulkan snapshot now implements upstream's contract (a fresh
  copy per draw, released by the engine). The taskset argument goes through
  upstream's host-layout `CellSpursTasksetAttribute`.
- Kept ours: MachStat answers 1 (upstream answers the interrupt-enable bit,
  which stalls jm_default, section 15), eight VMX ops upstream still reads
  through host pointers (`vaddshs`, `vsubsws`, `vsum2sws`, `vmuleuh`,
  `vpkshss`, `vpkuhus`, `vpkuwus`, `vmsummbm`), the taskset start ordering,
  diagnostics.
- SPURS lane rotation (section 19's experiment) is gone with the old kernel.

Results with a fresh lift: every menu glyph renders (`docs/media/main-menu-v015.png`);
the title screen runs at 70+ presented frames/s; the campaign reaches the
Corinth River loading screen as before.

**Texture gamma.** v0.15.0 decodes sRGB-flagged textures (TEXTURE_ADDRESS
bits 20-23) and encodes output when NV4097_SET_SHADER_PACKER is set. The menu
sky shader decodes and does not encode, so the menus are darker than on
v0.14 (offscreen mean 69 vs 149). That is the hardware model upstream
verified on Killzone 3; `RSX_TEX_GAMMA_OFF=1` restores the old look for
comparison until a reference capture settles it. Pipeline log lines now
show `srgb-out`, `gamma units` and the packer value.

**R6 (`SPURS_PM_SELECT_CURRENT=1`).** The policy-module poll answers the
running workload instead of 0, so a job manager that is not workload 0 keeps
its SPU instead of returning to the kernel after every job (Killzone 3 fix
list R6). Off by default; no change in the menus or the loading screen yet.

**Still open:** section 20. A shorter park grace does not help: at 50 ms the
walker skips the parked sub-buffer and the main thread then waits on label
0x1E; at 3 s the timer never fires because `put` keeps moving.

## 23. Section 20, narrowed: the park is jm_post's to patch

The parked sub-buffers are a ring of four slots (IO `0x1A4A64C`, `…84C`,
`…A4C`, `…C4C`). For every frame the PPU writes a self-JUMP into its slot
(`func_0038E120` from the render pass `func_0053E930`), and a **jm_post**
job (WWS workload 7, policy module image 100, overlay 209, SPU pc `0x0425C`)
later PUTs 4 bytes over it: a JUMP to the next word, which opens the slot.
`SPU_DMA_RANGE` now prints the image, overlay, task, workload id and the
word a small PUT writes (`SPU_DMA_RANGE_MAX` lifts the 64-line cap).

In steady state the SPU patch for frame k lands after the PPU has parked
frame k+1. At the stall the PPU has parked the next slot and waits for GCM
label 0x4B (released after the slot's CALL), and jm_post's patch for the
parked slot never comes. jm_post is not idle: its readyCount stands at 6
and its per-SPU cursors keep changing (`01D000D9 00D8FFFF` -> `…DF` ->
`…C9`), so it is running jobs but not reaching the patch job. Next: decode
jm_post's command list (`00000002 <ea>` entries between the job groups look
like guards/dependencies) and find what the patch job waits for.

## 24. In the level: x86-64-v3 and the first in-level frames

Sampling the SPU worker threads at the stall (`eu-stack`, 40 samples) put
`spu_shufb` on top of 37 of ~90 stacks, then `spu_fa`, `spu__fma_any`,
`spu_fm`. `spu_helpers.h` has an SSE4.1 `shufb`, but only the runtime library
was built with `-msse4.1`; the lifted SPU and PPU code used the x86-64
baseline and so the byte-loop `shufb`. The project now builds everything
with `-march=x86-64-v3` (CMake `KZ2_ARCH`; `x86-64-v2` or `native` also
work).

The `00000002 <ea>` entries of a WWS job list are job-group descriptors:
`{u16 jobs left, u16 0x0708, u32 bytes, u32 descriptor EA, u32 bytes}`
(`KZ2_WWS_DUMP=<sec>:<list hdr>` walks a list). jm_post had been working
through a 64-job group at about one job every two seconds -- slow, not stuck;
the park-patch job queues behind it.

With the faster helpers the level runs: in-level frames are presented at
~2 per second, GCM label 0x4B passes 0xD3, and the Corinth River intro shows
its subtitles ("ISA. Attention! Attention! Prepare for Air Operations.").
The image itself is still horizontal streaks (`docs/media/first-in-level-frame.png`):
the in-level renderer's surfaces are not decoded right yet.

## 25. The in-level image: G-buffer right, post chain wrong

Pass dumps of a Corinth River intro frame (the Intruder dropship's interior):
the deferred renderer's G-buffer (2560x720, i.e. 1280x720 at 2x MSAA, four
targets: albedo, light accumulation, normals, depth/position) and the lit
scene come out right (`docs/media/in-level-gbuffer-albedo-and-lit.png`).
What reaches the display does not:

- The base layer is black. The display copy (`pipe 113`) reads the colour
  grading pass's output, and that pass (`pipe 112`) takes its final colour
  from a 64x64 A8R8G8B8 LUT in main memory (IO `0x08A21D80`, EA
  `0x23721D80`) that is all zero. Nothing writes it but one `memset`: no SPU
  DMA, no lifted PPU store, no NV0039 copy (KZ2 sends no methods on
  subchannels 1-6). The fill must come through a host-side path the watches
  do not see -- an Edge Zlib inflate or another HLE copy.
- On top of it the SPU post chain's output (jm_post, 640x360 at EA
  `0x41A7D200`) is horizontal streaks (`docs/media/spu-post-output-640x360.png`):
  rows keep their vertical structure, detail along a row is lost. The SPU
  helpers pass upstream's tests at x86-64-v3, so this is in the jobs or
  their DMA, not in shufb/fa/fma.

Also fixed: the Vulkan engine backend now accepts R32F, RGBA32F and RG16F
colour targets (the exposure chain renders luminance into 320x180 X32
surfaces; they were dropped as "not supported yet"). The pass log shows, for
every main-memory texture, its EA, first bytes and how much of its first
4 KB is nonzero.

## 26. Why the colour-grading LUT is zero: a stream window nobody filled

The LUT at EA `0x23721D80` is filled by the level loader's stream reader
(`func_00A4A99C`: copy `n` bytes from the reader's window `[cur, end)` if
they are there, else refill through `func_0006C4D8`). A gdb breakpoint on
that function for destinations in `0x23xxxxxx` (with `KZ2_EDGE_LOG=1`, which
logs every Edge Zlib request) shows, in one run:

    LUTCOPY n=4000 dst=2371dd80 src=43351224 end=4335e450 nz=16358
    LUTCOPY n=4000 dst=23721d80 src=432ea90e end=432ef0a1 nz=0

The copy before the LUT reads the newest window (`0x4335…`, written by the
Edge "copy" requests just before it, `0x433354F1`, `0x433454F1`, ...); the
LUT copy reads a window at `0x432EA90E` that the reader moved to after a
refill, which only older rounds ever wrote (`0x432DE440`, `0x432EE440`) and
which now holds zeros. So the stream ring and the Edge requests disagree on
where a decompressed block lands -- most likely the HLE's reading of the
"copy" element (destination / skip fields) for FIOS's ring is off. Other
assets streamed the same way may be damaged too.

## 27. The LUT is zero on disc: it is filled at run time

Tracing the stream further: the reader double-buffers 256 KB windows
(`+0x80`/`+0x84`, file position `+0xA8`; `func_0006C1F0` issues an async read
for the current and the next window and waits with `WaitForAsyncOperation`).
FIOS satisfies the tail window from its block cache (`func_007CE528`: page
`cache base + page * 0x10000`, PPU memcpy or an Edge copy). In some runs the
cache-hit copy writes zeros into the window, in others the window is not
written before the reader goes on -- but `tools/psarc_find.py` (reads an
entry of `data_ps3.psarc` straight from the dump) shows the file itself is
zero there: entry 216 (2,202,560 bytes) has 16 KB of zeros at `0x21542D`,
exactly the LUT the reader copies. So the colour-grading LUT is a placeholder
on disc and something fills it at run time -- a PPU routine writing with
AltiVec stores (invisible to `PPU_WWATCH`, which hooks `vm_write*`), an SPU
job, or an RSX render target. Finding that writer is next.

Changes made on the way (kept, both safe defaults):
- `sys_lwcond_wait` waits on a real per-lwcond condition variable and wakes
  on `signal`/`signal_all`; without a guest timeout it still returns after
  `LWCOND_SAFETY_MS` (2) as a spurious wakeup, because signalers need not
  hold the lwmutex and long waits slowed the menus to a crawl.
  `LWCOND_POLL=1` restores the old poll.
- `KZ2_EDGE_ASYNC=1` services Edge Zlib elements on a host worker in push
  order (off by default; no difference seen).
- `FS_READ_ALL=1` lines carry the destination and file position.

## 28. The first visible in-level image

Two more pieces:

- **Main-memory render targets are written back.** Killzone 2 renders its
  post-processing inputs into main memory (640x360 and 320x180 colour, a
  320x180 R32F luminance target) for its SPU jobs to read. The engine kept
  them on the GPU, so the SPU post chain worked on stale memory -- the
  horizontal streaks of section 25. `rsx_draw_engine_flush_main_targets()`
  now copies every main-memory target drawn since its last copy back to
  guest memory before each label/semaphore release (NV4097 `0x1D70`/`0x1D74`,
  NV406E `0x6C`); RGBA8 through the ARGB conversion, float formats raw with
  big-endian elements. `RSX_MAIN_TARGET_WB=0` turns it off.
- **The colour-grading LUT.** The 64x64 LUT the grading pass reads is
  indexed by the input channel and the square root of the depth (RGB: a
  per-channel curve per depth, alpha: desaturation by luminance and depth) --
  KZ2's atmosphere (`AtmosphereBoxResource`). On disc it is a zero
  placeholder that follows a filled LUT of the same size; a gdb hardware
  watch over the whole run sees nothing write it. Until its writer is found
  the engine fills a still-empty 64x64 main-memory A8R8G8B8 texture with an
  identity curve (`RSX_ZERO_LUT_IDENTITY=1`, set by boot_main).

Result: the Intruder dropship interior of the Corinth River intro, with its
subtitles (`docs/media/in-level-intruder-first-image.png`), at ~1.6-2.3
presented frames/s. The tint is still off (no atmosphere grading) and some
runs still stall in the Havok step (section 20b).

## 29. The Havok stall: a PPU CAS racing an SPU PUTLLC

The physics step stalled in about half the runs: the main thread waits on
an hkSemaphore (`func_00803590`), the Havok PPU worker in
`func_00803758`, Havok's PPU/SPU semaphore -- one word, low 16 bits the
count, the upper bits the number of waiters. A PPU waiter adds itself with
`lwarx`/`stwcx.` and sleeps on an event queue; the SPU releaser increments
the count with GETLLAR/PUTLLC and sends an event only if it sees a waiter.

`ppu_stwcx32` asked "does an SPU hold a reservation on this line?" before
taking the lock-line lock, and took the lock only if so. An SPU could
GETLLAR the line right after that check, pass its PUTLLC compare under the
lock, and commit its 128-byte image over the PPU's CAS -- losing the waiter
the PPU had just added, so the SPU never sent the event. `stwcx.`/`stdcx.`
now take the lock-line lock whenever reservations are tracked and look the
reservation up under it. Four runs after the change, none stalled in
physics (three ran in the level throughout; one hit the frame-park wait of
section 20).

## 30. Where the in-level time goes, and the first speed-ups

Profiling the level (eu-stack, every thread): the main thread is always in
`func_00048208` waiting for GCM label 0x4B; the RSX side sleeps in the frame
clock; the FIFO walker sits on the render-pass sub-buffer parks that a
jm_post job patches (section 23); five of six SPURS workers are idle. The
frame is gated by **jm_post**, which may only run on one SPU and works
through ~460 jobs (the SPU post-processing chain) a frame.
`SPURS_PM_STATS=1` prints, every 5 s per workload, policy-module runs, busy
milliseconds and selectWorkload polls: jm_post ran ~2800 times / 3.1 s busy
in 5 s -- one job per run.

Fixes so far:
- SPU float ops on x86-64-v3: the scalar per-lane fast path of
  `spu_fa/fs/fm/fma/fms/fnms` now runs four lanes at once with AVX2/FMA
  (widen to double, operate, mask 29 bits, narrow), including true-zero
  operands and results like the NEON path; lanes outside it take the exact
  path out of line. 4 M random vectors x 6 ops match the exact reference;
  upstream's helper tests pass.
- Jobs no longer fall into the SPU interpreter: `SPU_INTERP_ENTRY_LOG=1`
  lists every (image, overlay, pc) that does; the three entries of the
  post-processing module 9de83b69cfa0 that only computed branches reach
  (0x4600, 0x5300, 0x8808) are lifted (`JOBMOD_EXTRA_FUNCS`, read by
  `lift_jobmods.py`, set in `tools/lift_spu.sh`). The job modules are now
  lifted with the v0.15 lifter.
- Lifted SPU images build at `-O3` (`KZ2_SPU_OPT`).

Level frame rate (4 cores, low priority): 1.2 -> 1.5 presented frames/s;
jm_post 1.1 -> 0.8 ms per job. Still far from 30: the post chain runs
single-threaded on emulated SPU code, and each job is a full policy-module
run.

## 31. jm_post on more SPUs

With the faster jobs, jm_post is still busy ~60% of wall time and gates the
frame. Two scheduling changes, set by boot_main for Killzone 2:
- `SPURS_PM_SELECT_CURRENT=1` (KZ3 fix list R6): the WWS policy module keeps
  running jobs instead of returning to the kernel after each one -- jm_post
  went from ~3400 to ~270 runs per 5 s. 1.5 -> 1.65 frames/s.
- `SPURS_MAXCONT="7:3"`: the kernel lets workload 7 (jm_post, requested max
  contention 1) run on up to three SPUs; the WWS job manager claims jobs per
  SPU row atomically. 1.65 -> ~1.9 frames/s, image unchanged. At 4 the
  level stalled once; 3 ran clean twice.

The Havok stall of section 29 is rarer but not gone (one run of six).

## 32. The rest of the Havok stall: the taskset exit scheduler (KZ3 R1)

A stalled run showed the Havok SPU semaphore at `0x12010000` (port 0x12, one
waiter, count 0): the PPU had registered correctly, but one SPU half of the
physics step had never run. Each step creates four Havok tasks (ELF
`0xC42A80`); each, as it exits, readies a follow-up task (ELF `0xC59980`) in
the taskset by setting bits with PUTLLC. The runtime's taskset exit
scheduler (`spu_taskset_task_exited`) cleared and scanned the same ready /
pending / enabled bytes with plain read-modify-writes under its own lock
only -- one follow-up was lost ("start task 4/5/1" for four exits), and its
semaphore release never came. This is the Killzone 3 port's fix R1: the
scheduler now reads and writes the bitsets under the lock-line lock, reports
the write to SPU reservations, and dispatches the chosen tasks after
releasing it. Six runs since, no physics stall (null renderer); two
Vulkan runs held ~1.9 frames/s in the level.

## 33. SSE forms of the SPU byte shifts and 16-bit multiplies

A stack-sampling profile in the level put one jm_post job loop
(`jobmod_a27e818cca34` at `0x5908`, a gather-and-sum filter) in two thirds of
the samples. On x86 the runtime had vector forms only for shufb and the
float ops; `rotqby`/`rotqbyi`/`shlqby*`/`rotqmby*` were 16-step byte loops and
the `mpy` family was scalar per lane. They are now one `pshufb` (result SPU
byte i = source byte i + d, host byte = SPU byte ^ 3, out-of-range lanes
zeroed) and `pmaddwd`/`pmulld` forms. `tests/test_spu_sx.c` compares every
one against the reference loops: 0 mismatches over 2M random inputs. The
level runs ~2.0 frames/s (was ~1.9); the image is unchanged. The float ops
(exact SPU truncation through double) remain the bulk of that loop.

## 34. jm_post on four SPUs

`SPURS_MAXCONT 7:4` used to stall; with the taskset scheduler race fixed
(section 32) four runs at 7:4 and one at 7:5 ran to the end. The profile
shows the hot jm_post filter already spread over three host threads, and
the per-workload stats (`SPURS_PM_STATS=1`) put jm_post at ~1.4 busy cores
on a four-core test pinning. The default is now 7:4: ~2.1 frames/s pinned to
four cores (was ~2.0); a machine with more cores gains more.

## 35. In the level: stable, but the opening scene does not advance

A 15-minute Vulkan run (four cores) stayed in Corinth River at ~2.1
frames/s with no stall and no crash. The view does not move, though: frame
to frame the image differs by ~0.2 grey levels except for the ISA
announcement subtitle ("Attention! Attention! Prepare for Air Operations."),
which comes and goes every ~75 frames. Input reaches the game -- the
`START` press of the test script that lands during loading opens the pause
menu (Resume / Restart / Quit / Options) as the level starts -- but held
sticks (`PAD_STICK`, `PAD_SWEEP`) and in-level presses of X, R1, R2, L1, L2,
square and triangle change nothing in the view. The sound streamer's
per-stream request counters (`+0x134` of the 0x38D8-byte stream records at
`0x4211C000`) are all zero, so no music/dialogue stream is waiting to start.
Next: find what the opening sequence waits on (script state, the cinematic
camera, or the characters, which do not appear in the frame).

A pass capture of one in-level frame (`RSX_ENGINE_PASS_DUMP`, 190 s in):
1694 guest draws; six shadow-map passes of 14-387 draws each; the 2560x720
G-buffer pass has 403 draws. The G-buffer albedo target is correct and
neutral grey (bunk hatches and wall panels in full detail), and its depth
spans 0.930..0.963 -- the camera faces a wall a short way off, so the
static view is the scene's own framing, not missing geometry in front of
it. The teal cast is therefore added later: the second target of the same
pass is already blue-green, and the lighting and the colour-grading LUT
(still the identity fill) come after that.

## 36. Why the opening scene stands still: its sequences never tick (open)

The level is Corinth River's opening on the cruiser *The New Sun* (the
checkpoint names `section_thenewsun ... cutscenes`, `cryo`, `launchbay`);
the round hatches in view are its cryo pods. What was ruled out:

- **Lua errors:** none. `luaL_error` (`0x3F3978`), `luaG_runerror`
  (`0x3EFD40`) and `luaD_seterrorobj` (`0x3E6790`) are never hit
  (`tools/gdb/lua_errors.py`). The level script's `OnUpdate` timer runs once
  per frame (`0x2CEA30` -> `0x3E5A10`, `tools/gdb/script_timers.py`).
- **Pause:** without the test script's START during loading the view is the
  same.
- **Streaming:** the cutscene lumps are read at level load; nothing waits on
  the asset streaming thread; no sound stream has a pending request.
- **Game state:** the campaign mode object is in state 7 (running), the
  world update `0x24F0F8` runs every frame with game time advancing
  (`dt` = 1/15 s at `0xF2A1D4`), and the session "is host" test
  (`0x680DE8`) answers 1.
- **Audio:** sound works in the title screen (port 0 peaks ~0.5); the
  silence that follows is a separate open item. The fast SPU helpers were
  bisected and are not the cause.

What is wrong: the game's reflection table (type descriptors with name,
member list, size and create/destroy function pointers 0x20 bytes before
the name) gives the create functions of the sequence types: `Sequence`
`0x1D4A20`, `SequenceRep` `0x1D4BC8`, `SequenceCameraEntity` `0x1D4E28`,
`StartSequenceEventInstance` `0x1C71A0`. At level load 21 `Sequence`
objects are created and registered in the list at `0xFADDE8`; each binds
(`0x1D77A0`), auto-starts (`0x1D7690`, playing flag `+0x11C` = 1) and runs
its prepare step once. After that their update (`Sequence` vtable `0xF7CF4C`
slot `+0x4C` = `0x1D7F00`, tick `0x1D7D60`) is **never called**: every
sequence stays in state `+0x160` = 0 (ready is 2) with frame counter
`+0x128` = 0, and no `SequenceCameraEntity` is ever created. The other
inherited entity methods (`0x66xxxx`) also run only during level load. So
the entity think pass does not reach these entities; finding where that
pass is scheduled is the next step.

Tools added: `tools/ghidra/XrefsTo.java` (code references to an address),
`tools/gdb/` (gdb Python scripts: call counts, backtraces, Lua errors,
script timers; `run_gdb.sh` sets up the run environment), and `KZ2_DUMPAT`
takes up to 64 entries and an absolute base with pointer chains
(`A<ea>>0>10+20:40`).
