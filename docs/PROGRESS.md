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
sequence is ready (`+0x160` = 2) with its frame counter `+0x128` at 0, and
no `SequenceCameraEntity` is ever created. The other
inherited entity methods (`0x66xxxx`) also run only during level load. So
the entity think pass does not reach these entities; finding where that
pass is scheduled is the next step.

Tools added: `tools/ghidra/XrefsTo.java` (code references to an address),
`tools/gdb/` (gdb Python scripts: call counts, backtraces, Lua errors,
script timers; `run_gdb.sh` sets up the run environment), and `KZ2_DUMPAT`
takes up to 64 entries and an absolute base with pointer chains
(`A<ea>>0>10+20:40`).

## 37. Entity updates run on fibers: host batch runner -- the opening cutscene plays

The entity think scheduler (`0x684798`, called through the entity manager
every world update) picks the entities that asked to update (`+0xE4` bit 3;
visibility through the representation's last drawn frame, update range) and
`0x684560` hands them to a fiber job system as one batch of jobs
`{OPD 0x66E420, &entity}`:

- `0x72438` runs a batch: five fiber contexts (stack, TOC, entry, argument)
  and a switch into the first;
- `0x7203C` is the switch: save the callee-saved registers (`0x71E10`),
  pick the next runnable fiber (`0x72680`), load its registers -- `r1`
  included -- and branch to its entry (`0x71F24`);
- the job `0x66E420` waits for its parent entity (`0x72600`), updates the
  entity (`0x66E288`) and ends (`0x72760`), switching to the next fiber.

A hand-written context switch cannot be lifted to C: it cannot move to
another stack or jump to an entry address, and the lifted code ran into the
next function in the binary instead. No entity job ever ran, so after the
level loaded nothing updated -- the sequences sat on frame 0, the camera
never moved, the characters never appeared. (The Killzone 3 port replaced
the same mechanism with host code, fix list P3.)

`tools/postlift_fixups.py` now routes the three functions to
`src/kz2_fiber.cpp`: the batch runs its jobs one after the other on the
calling thread, each in a frame below the caller's; a job's end returns;
the parent wait returns (the batch is sorted parents first, `0x684628`).
One trap: `ppu_stwcx32` keeps the reservation in `g_active_ctx`, not in the
context the lifted code passes, so each job is made the active context while
it runs and the caller's is restored after -- otherwise every later `stwcx.`
on the main thread failed and the allocator's semaphore spun forever.

With that the opening cutscene plays: Garza walks Sev to the window of *The
New Sun* ("Come on Sev, let's go. Time for us to deploy!", "Look at that.").
Applying it to an existing lift: `python3 tools/postlift_fixups.py`, then
rebuild (two chunks and the new file, under a minute at `-j 8`).

A 12-minute run after the fix: the cutscene ends and gameplay starts -- the
HUD shows the first objective, "Follow Garza to the Launch Bay", in first
person on the walkway of *The New Sun* -- at ~3.1 presented frames/s on four
cores, with no stall to the end of the run. README screenshots:
`docs/media/new-sun-cutscene-garza.png`, `docs/media/new-sun-first-gameplay.png`.

## 38. Audio after the title screen (open)

- The title screen's music reaches the cellAudio port (peaks ~0.4-0.6).
  Measurements in the menus are unreliable: `KZ2_DUMPAT` fires from the
  frame clock, which pauses during loads, so a dump labelled "23 s" can land
  much later. A fresh profile and the existing one both have music in the
  main menu when sampled there; the two `PROFILE.DAT` files differ in six
  bytes, none of them a volume.
- In the level the port carries ~1e-37: silence. The voice records
  (`0x0108xxxx`, 768 bytes each) hold sample data around 0.003-0.02 and
  volumes ~0.4-0.7, while the bus meters the mixer writes at `0x01171E00`
  read about -360 dB. The signal is lost between the voices and the buses:
  a gain stage -- 3D attenuation against the listener, or a group volume --
  is the next place to look (`ESoundMasterVolumeGroup`, `Ducker`,
  `GetSFXVolume` / `GetSpeechVolume` / `GetMusicVolume` in the EBOOT).
- The fast SPU helpers were bisected (scalar float, scalar int, byte shifts
  and each multiply) and are not involved.

## 39. Gameplay frame rate and an occasional stall at level start

Without the four-core pinning (all 16 host threads, `nice`), gameplay runs
at ~4.3-4.6 presented frames/s (pinned to four cores: ~3.1). Two runs of
eight stopped for good right after the level's first entity batch (0
frames/s for minutes); the others went on. The one caught with thread dumps
has the main thread in `0x57A6C0` -> `0x5573A0`, which sleeps 10 us at a
time until the first word of each record in the list at `0x11C27D8`
(count, array) drops to 0 and then frees the record's buffers: a pending
count that something else -- most likely SPU jobs -- decrements when done.
One decrement never arrived. Next: identify who decrements those counters
(watch the word with `PPU_CASWATCH` / `SPU_DMA_RANGE` in a stalled run).

Tracing the record the stalled run waited on (`0x235BB9D0`, the same
address every run, first of 17 in the list): the main thread increments its
pending count with `lwarx`/`stwcx.` in `0x557A10` (lr `0x557B94`) when it
starts a job for it, and stores the buffer count at `+4` with plain stores
(`0x557500`, `0x5573A0`). Each job is a SPURS task (SPU image 18, created
with the record as its argument) that decrements the count on its 128-byte
line `0x235BB980` with GETLLAR/PUTLLC (`pc 0x5EE0`; `SPU_ATOM_EA=235BB9D0`
shows the attempts, several PUTLLCs fail and retry as expected). PUTLLC
compares the whole line with its GETLLAR copy under the lock-line lock and
`ppu_stwcx32` takes the same lock, so the count itself is serialized; the
PPU's plain stores to the same line are the remaining suspect. These tasks
only run since entity updates do (section 37). Next: catch a stalled run
and read the record (`KZ2_DUMPAT=...:235BB9D0:0x20`) -- a count of
0xFFFFFFFF would mean a lost increment, a positive count a lost decrement
or a task that never ran.
Four more unpinned runs with that dump did not stall (count 1 at 200 s, the
normal in-flight value); the stall rate is about one run in eight.

## 40. Where the gameplay frame went: one GPU round trip per draw

A whole-process sampling profile in gameplay (`tools/prof_all.sh`, eu-stack of
every thread every 0.5 s; `tools/prof_report.py` summarises) showed only ~1.6
of 16 host threads busy. The guest main thread spent 78% of its time waiting
for GCM label 0x4B, and the FIFO thread was nearly always inside the Vulkan
backend's `vk_submit_and_wait`: every operation (draw, clear, upload,
snapshot) was its own command buffer, submitted and waited on. A level frame
has ~1700 draws.

The backend now records into one open command buffer and submits only when
the host needs a result (readbacks, present, the engine's `submit_and_wait`)
or a per-batch resource runs out (patch 0014):
- a full memory barrier opens every operation (all images stay in GENERAL),
  so each one still sees everything recorded before it;
- vertices, indices, constants and upload staging come from one 128 MB
  host-coherent ring, reset after each wait; constants are copied once while
  they stay the same;
- each draw gets a descriptor set from a pool reset after each wait (or the
  previous draw's, when nothing it names changed);
- images, framebuffers, pipelines and modules released while a batch is open
  are destroyed after it ran; readbacks reuse one cached staging buffer.
`PS3RECOMP_VK_SYNC=1` waits on every operation again;
`PS3RECOMP_VK_SUBMIT_STATS=1` prints submissions, operations and wait time.

Menus went from ~100 to ~250 presented frames/s and the campaign loads in
about a third of the time (level start at ~70 s instead of ~200 s). In the
level the frame rate barely moved: the waits that remained are at the points
the host really needs the GPU, about 24 a frame.

Two findings about those waits:
- **The NVIDIA dGPU stays at 330-550 MHz** (P5/P8, 4-8 W, 30-40% busy) under
  this stop-and-go load; `nvidia-settings GPUPowerMizerMode=1` does not change
  it on this laptop. The integrated Radeon 680M (`PS3RECOMP_VK_DEVICE=1`)
  halves the wait time (5-6.5 instead of ~4.3 frames/s at that point).
- Most of the remaining FIFO-thread time was textures that sample a render
  target in another format through guest memory (section 41).

## 41. Targets read in another format, built on the GPU

`RSX_ENGINE_RT_LOG=<n>` lists the textures the engine resolves through guest
memory. In the level, every frame:
- the 2560x720 D24S8 depth buffer read as A8R8G8B8 (deferred lighting
  rebuilds positions from it; bound in ~67 draws);
- the 2560x720 RGBA8 G-buffer target read as Y16_X16_FLOAT;
- the Z16 shadow map read as DEPTH16 (1024x1024) and X16 (640x360).
Each costs a GPU wait, a readback, a store to guest memory, a content hash,
a CPU decode and an upload -- and the texture cache made a new GPU image for
every changed texture and destroyed the old one.

- The cache re-decodes a changed texture into the image it already has
  (`RSX_TEX_REUPLOAD=0` turns that off).
- The backend's new `reinterpret` entry builds the texture on the GPU: the
  target is copied into the batch ring, a compute pass
  (`shaders/rsx_vk_reinterpret.comp`, compiled into
  `rsx_vk_reinterpret_spv.h`) turns each pixel into the texel the decoder
  would have made of the guest bytes, and that is copied into the texture --
  no wait, no CPU work. The engine keeps one result per (address, size,
  format, crossbar) and rebuilds it only when the target's write serial moved.
- The Z16 kinds stay on the guest-memory path: with them on the GPU the level
  start stalled in two of two runs (something reads the shadow map's guest
  bytes). `RSX_GPU_REINTERPRET_MASK` (default 5 = D24S8->ARGB8 and
  RGBA8->Y16_X16_FLOAT; 7 adds Z16; 0 = all off) selects them.

## 42. The fma fast path: TwoSum instead of the boundary test

The AVX2 forms of `fma/fms/fnms` (section 30) sent every lane whose double
result had its 29 low bits clear to the exact per-lane path -- every exactly
representable result, the common case in the post-processing filters --
which is why `spu__xv_fix3` was a quarter of all SPU samples. Like upstream's
AVX2 path (#369) and the NEON one, a lane now leaves the fast path only when
TwoSum says the double add actually rounded onto that boundary.
`tests/test_spu_fma.c` compares all six float ops with the exact reference on
4 M random and image-filter-like vectors: 0 mismatches.

## 43. The level-start stall: lost taskset bit updates (upstream #340, #388)

With the faster FIFO thread the level-start stall of section 39 showed in
about every second run, in two shapes: the main thread in `0x5573A0`
(pending count) or in `0x4FAAB0`, a semaphore held by a worker that waits on
an event queue for an SPU task (`0x4F7668`: create task, wait for its
completion events) that never reports. Both are SPURS tasks that never
finish.

The taskset's ready/pending/enabled/signalled bitsets share its first
128-byte line with the SPU task library, which updates them with
GETLLAR/PUTLLC under the lock-line lock. Three host writers did a plain
read outside that lock and a write inside it, undoing any SPU commit in
between: `cellSpursCreateTask`'s free-slot search and `add_task`'s
enabled/ready bits (fixed upstream in #388 by undecuple, Killzone 3),
the signalled bit in `spu_taskset_signal_task` / `spu_taskset_wait_signal`
(#340, Kurtisone, Twisted Metal), and the host policy module's
ready->running move. All four now update the raw bytes under the lock-line
lock and drop the line's reservations. Four runs since, none stalled (each
~5 frames/s in gameplay with two instances side by side).

A per-doubleword PUTLLC commit (keep a PPU plain store that lands between
the compare and the copy) was tried and reverted: the level start crashed in
`memcpy` into the stack region twice and stalled once -- a line written
piecemeal is visible half-written for longer than the 128-byte copy.

## 44. Where the FIFO thread's time goes (KZ2_RSX_TIMES)

`KZ2_RSX_TIMES=1` makes the runner's frame clock print, every 5 s, how the
FIFO thread spent its time: inside drains (and how much of that was waiting
for the GPU), and between drains with the FIFO empty (waiting for the PPU),
parked on a jump to itself (waiting for an SPU job to patch it) or other.
In gameplay it read **drain 93-100%, GPU wait 30-39%, parked 0-7%**: the FIFO
thread is the bottleneck, and the guest main thread's long waits on GCM
label 0x4B are waits for it. (A shorter sleep between drains,
`KZ2_DRAIN_US`, changed nothing.)

Changes since (patch 0015):
- **Consecutive draws into one framebuffer share a render pass instance**
  (rasterization order already orders their attachment writes, and every
  other operation closes the pass): ~5 draws per pass in the level.
  `PS3RECOMP_VK_RP_MERGE=0` turns it off. The image is unchanged; the GPU
  wait barely moved, so per-draw passes and barriers were not what the GPU
  was spending its time on.
- **The batch ring lives in device-local, host-visible memory** when the
  device has it (VRAM through the BAR on the RTX 4050): vertex fetches and
  constant reads no longer cross PCIe. With two instances running the GPU
  wait fell from ~35% to 8-20%; alone it stays near 30% because the NVIDIA
  GPU drops to ~450 MHz under one instance's bursty load (section 40).
  `PS3RECOMP_VK_RING_VRAM=0` keeps it in host memory.
- `PS3RECOMP_VK_PREFER=integrated` (`KZ2_GPU=igpu tools/play.sh`) picks an
  integrated GPU. On the Radeon 680M gameplay runs at **6-8.5 frames/s**
  (FIFO thread: drain ~97%, GPU wait ~15%) against 4-5 on the RTX 4050.
- **Linear textures decode by rows**: `rsx_texture_decode` walked every texel
  through `decode_texel`; linear images that only byte-swap components (R16,
  R16G16(F), RGBA16F, R32F, RGBA32F) or rotate A8R8G8B8 now take a row loop.
  The Z16 shadow map that still goes through guest memory every frame
  (section 41) was the FIFO thread's largest CPU cost. Compared against the
  original decoder on random data for 18 formats: identical.

## 45. Stalls left: jm_post on several SPUs, and Z16 on the GPU

- **Z16 read as DEPTH16/X16 on the GPU** (`RSX_GPU_REINTERPRET_MASK=7`)
  parks the RSX for good at the level start, 4 runs of 4, even with the
  taskset fixes and with the guest copy still stored at the next label or
  whenever the FIFO goes idle (`rsx_draw_engine_idle_flush`). DEPTH16 alone
  is enough. Not understood; Z16 stays on the guest-memory path.
- On the integrated GPU (a faster FIFO thread) the level start stalls in
  about half the runs; on the RTX 4050, none of ten runs since section 43
  stalled. The stalled runs show the main thread in `0x4EED70` waiting for
  the completions of a batch of SPU tasks (`+0x210` of them, on a condition
  signalled by the thread that services event queue 6, which has nothing
  pending: `PS3_EVQSTAT`), the RSX parked, and one SPU worker running a
  jm_post job (`jobmod_7eb6fb9f6647` or `9de83b69cfa0`) that keeps going --
  a job polling for something. Killzone 2 builds jm_post for one SPU
  (contention 1); since section 34 it runs on up to four
  (`SPURS_MAXCONT=7:4`), where a job can poll for work a job behind it in the
  list has not done yet. With `SPURS_MAXCONT=7:1` two integrated-GPU runs did
  not stall (at 3.6-5 frames/s); with 7:2 one of two did. The default stays
  the discrete GPU with 7:4.

## 46. jm_post back on one SPU; the integrated GPU by default on laptops

The stalls of section 45 point at the four-SPU jm_post override:
- the stalled runs show a jm_post job running on and on while the main
  thread waits for SPU task completions that never come;
- Killzone 2 gives jm_post a maximum contention of 1 -- its post-processing
  jobs are written to run one after another -- and `SPURS_MAXCONT=7:4`
  (section 34) let up to four run at once;
- with `7:1` five integrated-GPU runs did not stall (with 7:4 about half
  did; with 7:2 one of two).
The override is gone: jm_post runs on one SPU as the title asks
(`SPURS_MAXCONT=7:4` still works, opt-in). The cost is real on the RTX 4050
(~4.5 -> ~2.5 frames/s, the RSX then waits for jm_post) but small on the
Radeon 680M, so `PS3RECOMP_VK_PREFER=integrated-on-laptop` is the new
default: the integrated GPU is preferred when the discrete one is a laptop
part (its name says "Laptop"); a desktop keeps its discrete GPU.
`KZ2_GPU=dgpu|igpu tools/play.sh` overrides.

Also (patch 0016): `mfc_for`, the lookup of an SPU context's DMA engine that
every channel access does, took a shared lock each time -- ~11% of a busy
SPU thread in the post-processing jobs. Each thread now remembers its last
(context, engine) pair; any release of an engine bumps a generation that
drops them all.

Gameplay on the defaults, one instance: **5.2-6.3 presented frames/s**, the
FIFO thread busy 52-72% (GPU wait 12-13%) and parked 28-48% waiting for
jm_post, which keeps one SPU ~72% busy. Six runs, no stall.

## 47. Sound in the level: the campaign-start watchdog is no longer needed

The silent level of section 38 was the port's own doing. Since section 13 a
`sys_timer_usleep` hook forged the stop of the menu-music stream (stream 7)
at campaign start and, for the master teardown, cleared the sound engine's
voice list by hand -- the stream never finished stopping on its own then.
With the SPURS fixes of sections 32 and 43 it does: with the hook off the
campaign starts, no `[kz2-sound]` line appears, and the level has sound --
the cellAudio port (`KZ2_DUMPAT=...:58000000:0x4000`, `logs/amp.py`) carries
samples around 0.005-0.08 where it read ~1e-37 before, while the menu
music is ~0.3-0.5. The hook is now opt-in (`KZ2_SOUND_WATCHDOG=1`).

One of six runs without it stalled right after the level's first entity
batch (the others reached gameplay with sound); with it, none of six did.
That stall was not caught with thread dumps yet.

## 48. SPU float fast path: denormal and extended-range operands

`SPU_FF_WHY=1` tallies why float lanes leave the AVX2 fast path (and prints
24 examples). In gameplay ~490 M lanes a minute did, all of two kinds: `fm`
with an extended-range operand (biased exponent 255, e.g. `x * 0x7FFFF800`)
and `fma` with a denormal one (integer bit patterns). Neither needs the exact
path: SPU singles treat a denormal operand as a signed zero and exponent 255
as an ordinary larger number, both exact in double. Operands are now flushed
and widened by hand instead of with `cvtps_pd` (patch 0017); results outside
the normal range still go per lane. `tests/test_spu_fma.c` now feeds
denormal and extended operands too: 0 mismatches against the exact
reference. Fallback lanes fell to ~70 M a minute (mostly fma results that
really rounded onto a boundary) and jm_post's busy time by about a fifth.

## 49. Keyboard and mouse as a first-person pad

The keyboard fallback put WASD on the d-pad and had no right stick, L2/R2 or
L3/R3: without a game controller the level could not be played. With
`PS3_PAD_KBM` on (the default; patch 0018) WASD is the left stick, the mouse
the right stick (motion since the last poll, `PS3_PAD_MOUSE_SENS` per pixel,
default 3), left/right/middle click R1/L1/R3, Left Shift L3, V R3, Left Ctrl
Circle, G R2, Z L2; the arrows stay the d-pad. The mouse is captured
(SDL relative mode) while the game window has the keyboard focus; F1
releases and recaptures it. A connected controller keeps its sticks unless
the keys or the mouse are in use. The layout is in `tools/play.sh` and the
README.

## 50. The level-start stall, again: lost completions of a WWS job batch

Caught with stacks in the run directory where it shows most: the main thread
waits in `0x4D9100` / `0x4FAAB0` for the semaphore at `*(0xF137F4)`, held by
the worker in `0x4F7668` (via `0x4DA800`/`0x4FB298`), which started a batch
of jobs and counts their completion events on an event queue
(`0x4F7668` itself, then `0x4EEA68`). The events never come. At the stall
two SPU workers are busy: a job of module `576019cad526` (the batch's own
jobs, by the look of it) and a jm_post job of `7eb6fb9f6647`; the queue has
nothing pending (`PS3_EVQSTAT`).

Not fixed. `src/kz2_hle.cpp` now gives the three waits that only count
completions (`0x4F7668`, `0x4EEA68`, and `0x4EBD08`, which feeds the main
thread's `0x4EED70`) a synthesized empty event when they have waited
`KZ2_LOST_EVENT_S` seconds (default 15, 0 = off) without a single frame
presented, and logs `[kz2-evq]`. Whether that brings a stalled level back
is not verified yet -- the runs since did not stall.
Follow-up: with `KZ2_LOST_EVENT_S=15` a stalled run logged ten synthesized
events (queue 6, `0x4EBEFC`) and stayed stalled, so the hook is off by
default now. The stall does not depend on the GPU reinterpretation either
(`RSX_GPU_REINTERPRET_MASK=0`: one of four runs stalled). It shows most in
two run directories with the same saves (about half their runs) and in
roughly one run in six elsewhere; it was one in eight before this session.

## 51. Sparse texture re-hashing; where the frame goes now

The texture cache hashed every cached texture in full at its first use in a
frame (~10% of the FIFO thread). Upstream's fix from the inFAMOUS 2 port
(a594fa0, erikomis) is in (patch 0019): 64 bytes of every 2 KB plus the
last 64 each frame, the full hash when that differs, every eighth frame,
for textures of 16 KB or less, and after a store the engine made itself
(`PS3RECOMP_ENG_TEXHASH_FULL=1` restores the old behaviour).

With it the FIFO thread is busy 40-58% of the time and parked 42-60% --
waiting for jm_post, which now gates the frame. Where jm_post's SPU time
goes (section 46's profile): the job modules' own code (`9de83b69cfa0`,
`7eb6fb9f6647`, `a27e818cca34`, `9935c8f700db` ...), and runtime overhead
per lifted block: `spu_task_launch_check` (diagnostic bookkeeping on every
trampoline step), `spu_lookup` (indirect branches), DMA copies. That is
the next place to work on, together with the level-start stall (sections 45
and 50).

## 52. The stalled job batch sends its own completions

The module whose job was running at every caught stall (`576019cad526`)
posts its completions itself, through the SPU interrupt mailbox
(`wrch SPU_WrOutIntrMbox` at LS 0x161FC, then the answer from
`SPU_RdInMbox`). The runtime answered a full queue (these are 50 entries)
with EBUSY at once, so fire-and-forget code would lose the event; a user
event now waits up to `SPU_EVENT_FULL_WAIT_MS` (2000) for room and logs
`[spu-evt] queue N full` (patch 0020). In four runs the queue never filled
and one of them stalled anyway: the job does not get to its send. Next:
find what that job loops on (its DMA helper is at LS 0x16294, its tag wait
at 0x453C) in a stalled run.

Two more observations on the stall. The run directories that "stall more"
have byte-identical saves; what they share is being the second of two
instances started 12 s apart, i.e. the more loaded one -- a single instance
stalls far less often (about one run in eight of the single runs since
section 46). In a stall caught with ten stack samples the jm_post worker
keeps moving through different jobs, four SPU workers are idle, and the
main thread waits in `0x4EED70`; `SPURS_PM_SELECT_CURRENT=0` (the policy
module yields after every job) made it worse, three stalls in four.

The chain in that capture, for the next look: the main thread
(`0x4D9BD0` -> `0x4EED70`) waits on a condition for `+0x210` completions; the
thread that signals it (`0x803CB8` -> `0x4EBD08`) waits on event queue 6 for
the completion events; those come from SPURS tasks that `0x4EEA28` starts
through `cellSpursCreateTaskWithAttribute` (`0x7F80C0`), each dispatched on
its own host thread (`spu_workload_dispatch_async`). Queue 6 has nothing
pending, no SPU worker runs such a task, and no `[spu-evt]` drop or
queue-full line was logged: a task is either never started or ends without
posting. Next: count, per stalled run, the tasks `0x4EEA28` creates, the
`[spu_workload] async ... RETURNED` lines for them and the pushes into queue
6 (`PS3_EVQSTAT=1`), to tell which one goes missing.

## 53. The level-start stall found: a SPURS task started under another task's id

Counting per stalled run (`PS3_EVQSTAT=1`, the `CreateTask` / `dispatch HIT`
/ `RETURNED` lines) showed the stall is not a lost event: the last thing
that happens is a Havok physics step (tasks of ELF `0xC42A80`, follow-ups of
`0xC59980`), and the queue the waits count on gets nothing more. A dump of
every tracked taskset once two 5-second windows pass without a frame
(`[taskset-dump] no-frames`, boot_main + `spu_taskset_dump_all`) showed
the Havok taskset with tasks 1 and 2 **enabled, ready and marked running by
the runtime -- with no thread running them**.

Cause: a dispatched task learned which task it was from two single-slot
globals (`g_ydkj_real_taskset_ea`, `g_ydkj_real_taskid`) that
`cellSpursCreateTask` sets on a PPU thread and the exit scheduler sets on
SPU threads, then read back in `spu_workload_dispatch_async`. Two
dispatches at once and a job took the other's id: it ran with the other
task's context, and on exit cleared the other task's bits, leaving its own
marked running forever. The physics step waited for it.

Fix (patch 0021): the dispatching thread names the task in thread-local
storage (`spu_workload_dispatch_target`), which the dispatch reads first.
Also, an SPU commit to a taskset's first line (its ready/pending/enabled
bitsets) now runs the start scan too (`spu_taskset_line_committed`;
`SPU_TASKSET_LINE_SCAN=0` turns it off) -- real SPURS has idle SPUs polling
those bits, and here a task readied after the last exit's scan was never
started.

Six runs since in the two-instance setup that stalled about half the time:
five reached gameplay, one stopped earlier (at 4279 frames, during the
load) with every taskset clean -- a different stop, not caught yet.

## 54. A real profiler; the cheap float path; jm_post on four SPUs again

`tools/kzprof.c` samples every thread of the game through
`perf_event_open` (no perf package needed; `kernel.perf_event_paranoid=2`
lets a user sample its own processes) at 1 kHz of task clock and resolves
the samples against the binary's symbols. `tools/prof_perf.sh` runs the game
to gameplay and profiles it (`logs/NAME.txt` report, `NAME.raw` samples,
`NAME.nm.gz` symbols), `tools/kzprof_mods.py` turns runs into CPU ms per
presented frame per job module, and `tools/kzprof_ips.py` shows the hottest
instructions of one function. 1000 samples a second instead of 40 stacks.

What it showed in gameplay (Radeon 680M, the machine in power-saver):
- the main thread is not the bottleneck: half its busy time is a spin
  (`func_003BD660`) on SPU job progress counters, and ~2/3 of its time it
  sleeps in `func_00048208`, a 30 us `sys_timer_usleep` poll of an RSX label;
- the frame is RSX scene drawing (FIFO thread) followed by jm_post on one
  SPU, which the FIFO then waits for -- serial;
- in jm_post, the float ops' manual widening (patch 0017) dominated: ~45
  x86 instructions per `fm`.

Patch 0022:
- **float fast path, tier 1**: operands flushed (denormal -> +0), widened by
  `cvtps2pd`, the result's 29 low bits cleared and narrowed; an exponent-255
  operand widens to Inf/NaN and a result >= 2^128 narrows to Inf, which sends
  the whole vector to the manual-widening path (now tier 2), as does a result
  that only rounding put on a 24-bit boundary or on 2^-126 (TwoSum error for
  fa/fma). fm 9.1 -> 3.0 ns, fma 13.2 -> 5.3, fa 10.6 -> 4.4 (throughput
  loop). `tests/test_spu_fma.c` (now with operands whose results land near
  2^-126 and 2^128): 0 mismatches over 8M vectors per op.
- **shufb** without the three byte-order swaps (the control byte sits at
  the result's host byte; its index is the low nibble xor 3), and the
  constant selectors behind one test; **fi**, **cflts/cfltu/csflt/cuflt**
  as SSE/AVX2 code instead of out-of-line per-lane loops. `tests/test_spu_conv.c`:
  0 mismatches over 3M vectors, every scale.
- per hop of the trampoline drain: the lockstep and tail-return gates
  inline, the depth guard's ring per thread (one ring all SPU threads wrote
  was a shared cache line); `spu_lookup` answers from a per-thread cache
  (every job module registers functions at the same LS addresses, so the
  hash chains were long); the LS read's image-2 probe is opt-in.
- the PPU store watch (`barrier_watch_hit`, twice per 64-bit store) is one
  branch when no watch is set.
- vertex fetch: the element decoder switches once per element instead of
  once per component; the reference remap uses a table indexed by
  `vertex_id - min` when one base index and a narrow range allow it (same
  first-use order, so the same output; a scratch test against the old code:
  0 differences); the Z16 depth store converts 8 pixels at a time.
- `cellGcm_complete_flip_now` + `KZ2_FLIP_IMMEDIATE=1` (boot_main): a
  presented flip completes at once instead of on the next 60 Hz beat.

**jm_post on four SPUs** (`SPURS_MAXCONT=7:4`, section 46 turned it off for
stalls): with the task-id fix of section 53 the run that used to stall half
the time went to gameplay and stayed there; 5.2 -> 8.5 frames/s, with the
new helpers 9.1. Its rate sat exactly on 60/7 = 8.6: the guest waits for the
flip, which completed only on the 60 Hz beat.

Tried and dropped: keeping SPU registers in C locals inside lifted functions
(a post-pass on the lifter's output). The SPU has 128 registers, x86 16: the
compiler spilled the locals to the stack and added entry/exit copies -- the
hot jm_post loop grew from 2528 to 3627 instructions.

## 55. The FIFO thread: batches in flight, a vertex cache; SPU floats in MXCSR mode

With jm_post on four SPUs the FIFO thread became the limit (drain ~80% of
its time, a fifth of that waiting for the GPU). Patch 0023:
- **batches in flight** (Vulkan backend): three slots, each with its own
  command buffer, fence, descriptor pool, deferred destructions and third of
  the (now 192 MB) ring. A batch is submitted without waiting once it holds
  `PS3RECOMP_VK_ASYNC_OPS` operations (default 192) or its ring part is full,
  and the present no longer waits either (the swapchain's acquire semaphore
  waits for the slot that used it last); only a readback waits, for every
  slot. The legacy (pre-engine) paths, which share one vertex buffer and
  descriptor set, wait for everything first.
- **vertex cache** (draw engine): a draw's key hashes its layout, every
  fetched attribute's format, its references and the guest bytes they read;
  a key seen twice keeps its decoded vertices, and an entry unused for 120
  frames is dropped (`RSX_VTX_CACHE=0` off, `RSX_VTX_CACHE_STATS=1`). In
  gameplay 93% of the draws hit (~40 MB held); the FIFO thread's own code fell
  from 42 to 33 ms of CPU per frame.
- index lists become references 8 at a time (AVX2, byte swap + widen +
  interleave with the base index; a group holding the restart index goes
  through the scalar loop), and the attributes are fetched attribute by
  attribute with one loop per type (`rsx_vertex_fetch_all`, compared against
  the per-vertex path: 0 differences over 20000 random plans).
- **SPU floats in MXCSR mode**: the outermost `spu_run_with_halt` on a
  thread sets round-toward-zero + denormals-are-zero + flush-to-zero (and
  `g_spu_fp_rz`), which inside the normal range is exactly the SPU's single
  precision: fa/fm/fma become one `addps`/`mulps`/`vfmadd` plus a range test
  (an exponent-255 operand, or a result of magnitude FLT_MAX, where a
  round-toward-zero overflow lands, takes the exact paths, which stay exact in
  this mode); zero results get their sign cleared. Double precision and
  `frds` put the thread's own MXCSR back for their duration. `SPU_FP_RZ=0`
  keeps the old paths. Both float tests now run in both modes: 0 mismatches
  (fa/fs/fm/fma/fms/fnms 16M vectors, conversions and fi 6M). fm/fa/fma
  2.0/1.9/2.8 ns (tier 1: 3.6/5.2/5.8). The post-processing module
  `a27e818cca34` fell from 86 to 61 ms of CPU per frame, `9de83b69cfa0` from
  25 to 17.

Gameplay (one instance, power-saver, `SPURS_MAXCONT=7:4 KZ2_FLIP_IMMEDIATE=1`):
9.1 -> 10.5 frames/s. The FIFO thread is busy ~90% again (GPU waits ~19%).

## 56. Where the FIFO thread waited for the GPU

`PS3RECOMP_VK_SUBMIT_STATS=1` now also splits the batch ring's traffic
(vertices, indices, VS/PS constants) and counts the waits by caller
(printed as offsets from `vk_submit_and_wait`; add `nm`'s address of it and
`addr2line`). In gameplay the FIFO thread waited ~23 times a frame, ~0.94 ms
each:
- **8 a frame: the Z16 shadow map stored into guest memory** -- each time
  the engine waited for the GPU, then the readback waited again.
- 3-4 a frame: main-memory colour targets stored for the SPU post-processing
  (`flush_main_targets` at a label release) -- needed.
- 1 a frame: the present read the whole frame back to the host, which only a
  frame dump (or the headless test hook) uses.

Patch 0025:
- `rsx_draw_backend.readback_ordered`: the Vulkan readbacks order themselves
  after everything recorded and wait for their own result, so the engine's
  wait before them is skipped (two GPU round trips per readback became one).
- the present's full-frame readback only for a dump, or with
  `PS3RECOMP_VK_PRESENT_READBACK=1` (default on headless, off in a window).
- **Z16 on the GPU, without a guest copy**: `RSX_GPU_REINTERPRET_MASK`
  defaults to 7 (Z16 included). It was off for "parks the RSX for good" --
  observed before the SPURS task-id race of section 53 was fixed; it does not
  any more. The guest copy that was still owed is no longer stored
  (`RSX_Z16_GUEST_STORE=1` stores it): nothing visible reads it, and the
  frames compared with and without show the same lighting.
- the depth-via-guest log (`RSX_ENGINE_RT_LOG`) prints before the GPU path is
  tried, so it counts candidates, not fallbacks.

Gameplay: 10.8 -> ~12.3 frames/s; FIFO thread drain 70% (GPU wait 6%), parked
30% -- waiting for the SPU post-processing again.

## 57. Lifted SPU code: results stay in vector registers, no LS diagnostics

A per-thread timeline from timestamped samples (`tools/kzprof_timeline.py
logs/NAME [start_ms] [span_ms] [bucket_ms]`: one character per bucket per
thread, the job module or runtime area it ran most) showed the frame plainly:
the FIFO thread stops ~30 ms a frame while the post-processing filter
`a27e818cca34` runs on four SPUs at once, ~20 ms per burst.

Its loop (`0x5908`, gather and sum) compiled to ~27 x86 instructions per SPU
instruction. Two things in it, patch 0026:
- **results through rax:rdx**: the float helpers' slow paths were
  out-of-line functions returning `u128`, which the SysV ABI returns in a
  general-register pair; merging that with the inline result made every
  fast-path result go `vmovq`/`vpextrq` and two 64-bit stores too. The slow
  paths now return `__m128i` and the helpers return once.
- **`SPU_LS_DIAG=0`** for the lifted SPU sources (CMake option
  `KZ2_SPU_LS_DIAG`, off): every local-store access checked two diagnostic
  gates (`SPU_LS_LOWREAD` probe, `SPU_LS_WATCH`); the runtime keeps them.
  With the cold calls gone the compiler also keeps more in registers.
  `a27e818cca34` 67 -> 53 ms of CPU per frame; gameplay 12.7 -> ~14 frames/s.

Also in 0026: the Vulkan draw sets only the state that changed since the
last draw in the same command buffer (pipeline, viewport, scissor, stencil
reference, depth bounds, descriptor set, vertex buffer; the ring is bound
once as index buffer and draws start at `firstIndex`) -- no measurable change;
a frame dump reads back only the frames it writes; the SPURS kernel's
workload pass interval and idle backoff are tunable
(`SPURS_KERNEL_POLL_US`, default 1000; `SPURS_IDLE_BACKOFF=0`).

Measured and not kept: jm_post on six SPUs (12.3 vs 12.4); FIFO drain every
250 us (12.4, same); SPURS pass every 250 us (11.6) or without the idle
backoff (9.7 -- idle workloads' policy modules then take SPU slots).

## 58. Sound no longer drifts behind the picture; an index cache

**Audio drift (fixed).** The Linux cellAudio mixer mixed a 5.3 ms block,
then slept 5 ms (2 ms with less than four blocks queued) -- ~196 blocks a
second against the device's 187.5. SDL's queue has no bound, so the delay of
the sound behind the picture grew by ~2.5 s a minute: `AUDIO_RATE=1` (now on
Linux too) showed 538,112 samples queued -- 11 s -- four minutes into the
level. The loop now mixes only when the device has room for a block
(`audio_backend_room`, four blocks ahead) and paces on the clock without a
device: 187.5 blocks/s, the queue steady at 1024 samples (21 ms).

**Index cache** (draw engine): the references a draw's index batches turn
into -- the unique list, restart cuts and the occurrence map the topology is
rebuilt from -- keyed by the index bytes and everything else that shapes
them, kept on a key's second sighting (`RSX_IDX_CACHE=0` off;
`RSX_IDX_CACHE_VERIFY=1` decodes anyway and compares: 3 million draws, 0
differences). A hit reads the occurrence map in place and hands the vertex
cache the references' hash it already knows.

**VS constants**: the 7.5 KB block was copied by the engine and compared by
the backend on every draw; `rsx_dispatch.const_gen` counts constant writes
and the block is copied and bound only when it or the viewport transform
changed.

With these the FIFO thread's own code fell further, but gameplay stays at
~13.5-14 frames/s: the FIFO thread now waits for the SPU post-processing a
third of the time (`[rsx-times] parked 31-33%`).

## 59. The frame is a relay; a 15-minute session

The timeline (`tools/kzprof_timeline.py`) shows the frame as a relay between
two parties that rarely overlap: the FIFO thread draws the scene while the
SPUs are mostly idle, then the post-processing jobs (`e569adaf4ffe`, then
`a27e818cca34` on four SPUs) run while the FIFO thread waits for them, then
the FIFO thread finishes the frame. Both halves are on the critical path:
~45 ms of FIFO work and ~25 ms of SPU work per ~72 ms frame.

Tried in this round:
- **sparse vertex-span hashing** (patch 0028): spans of 16 KB or more go
  into the vertex-cache key as 64 bytes of every 2 KB plus the last 64; their
  full hash is checked every 8 frames, and a change the samples missed drops
  the copy (`stale by the full check` in `RSX_VTX_CACHE_STATS`). The backend's
  GPU copy is named by the key and the full hash together, so it cannot
  outlive a change. `RSX_VTX_SPARSE=0` hashes everything. `vc_hash` 12 -> 10%
  of the FIFO thread.
- **the main thread's progress wait on the host** (`kz2_spu_progress_wait`,
  applied to `func_003BD660` by `tools/postlift_fixups.py`): the same wait
  with `pause` between passes. No frame-rate change; kept (it no longer runs
  flat out on a core an SPU worker shares). `KZ2_SPIN_WAIT=0` restores it.
- `-mtune=znver3` for the lifted SPU code: no gain (`a27e818cca34` 54 -> 57 ms).
- the FIFO thread's remaining libc time has no single owner (eu-stack:
  per-draw memsets and copies spread over `sink_end`, `vk_begin`, the
  descriptor writes).

**15-minute session** (`tools/longrun.sh 900`): from the title to gameplay
and on, ~14 frames/s throughout, resident memory 862 -> 882 MB over the last
ten minutes, no stop.

## 60. Profile-guided SPU code; making SPURS react faster is slower

**PGO for the lifted SPU code** (CMake `KZ2_SPU_PGO=gen|use`, runtime
`KZ2_PGO=<start>,<end>`): an instrumented build zeroes its counters at frame
`<start>` and writes the profile at `<end>` (the runs are killed, so nothing
is written at exit); `llvm-profdata merge` into `pgo/spu.profdata` (ignored
by git: it is derived from the title's code) and `use`. `a27e818cca34` 54 ->
49 ms, `9935c8f700db` 14 -> 12 ms of CPU per frame; the frame rate stays at
~14 because the FIFO half of the relay is as long.

**SPURS reacting faster** (patch 0029, `SPURS_LINE_WAKE=1`): an SPU commit to
an instance's workload line (ready counts, signals) woke the kernel thread at
once instead of at its next 1 ms pass -- 13.9 -> 12.5-13.4 frames/s, the same
direction as the 250 us pass of section 57: more passes start more idle
policy modules, which take SPU slots. Off by default. Per 5 s
(`SPURS_PM_STATS=1`): jm_post (w7) 9,354 policy runs, 7.6 s busy; w0 15,896
short runs, 0.9 s; the two idle workloads ~1,900 runs each at ~7 us.

## 61. Played by hand: input fixes, a launcher, the stop at Corinth River (open)

The first sessions played by hand (2026-10-24) found what the scripted runs
could not:
- **Esc quit the game** (patch 0030): the Vulkan window loop took Esc for
  "close", while the keyboard layout uses it for Circle (back) -- backing out
  of a menu ended the game. Only closing the window quits now.
- **stuck keys in menus** (patch 0031): without a game controller a key's
  pressure value was set to 255 and never cleared, and Killzone 2 reads
  pressure -- it saw the key held for good; also `SDL_PumpEvents` ran on the
  guest thread while the renderer pumps on its own. Pressure now follows the
  keys, and only the window's thread pumps.
- `tools/launcher.py` (desktop entry *Killzone 2*): play, campaign quick start
  (PAD_SCRIPT), GPU, mouse speed, and a "stable mode", on by default:
  `SPURS_MAXCONT=7:1`, jm_post on one SPU as the title builds it.

**Open: the picture stops at level transitions.** Three hand sessions, three
stops, each where a new level loads:
- the campaign, right after the weapon pick-up that ends the prologue: the
  game saves the checkpoint, frames stop; the main thread waits in
  `func_00048208` for an RSX label (`[HOTREAD] spinning on 0xE80004B0`),
  every taskset clean;
- a Skirmish map load from the menu: `[rsx-times]` shows the FIFO thread
  parked 99% -- on a jump to itself, waiting for the guest to patch it;
- continuing from that checkpoint (Corinth River): frames stop and so do the
  FIFO thread's 5 s reports -- it does not get out of `drain()` -- while SPU
  tasksets keep running.
The scripted runs never crossed a transition (the player stands at the start
of the prologue). Prime suspect: jm_post on four SPUs (`SPURS_MAXCONT=7:4`,
the default again since section 54), which stalled the level start half the
time in section 45 until the task-id fix of section 53. Not yet confirmed:
reach a transition with `7:1` (the launcher's stable mode) and with
`KZ2_RSX_TIMES=1`.

## 62. On a desktop: Ryzen 5 5600, RTX 3060; a full-strength build; register caching measured

The tree moved from the laptop (Ryzen 7 170, Radeon 680M + RTX 4050 Laptop)
to a desktop: Ryzen 5 5600 (Zen 3, 6 cores / 12 threads), 32 GB DDR4,
GeForce RTX 3060 (NVIDIA 615.78), CachyOS.
- The copy kept no symbolic links and no execute bits: modes restored from
  the git index, the venv rebuilt (Python 3.14), `run/vfs` is now a full copy
  of the dump instead of a link (it works either way).
- No Ninja or Vulkan headers on the system and no root: Ninja from pip in
  `.venv`, Vulkan-Headers v1.4.328 passed as `PS3RECOMP_VULKAN_INCLUDE_DIR`
  (`CMAKE_ARGS` in `tools/build.sh`).
- Firmware fonts: RPCS3 v0.0.43 installs the disc's own update
  (`PS3_UPDATE/PS3UPDAT.PUP`, firmware 2.52) with `--installfw`; it asks
  twice ("Install firmware?", "Old firmware detected") in message boxes, so
  an unattended install needs the answers given (a two-line `LD_PRELOAD` that
  returns Yes from `QMessageBox::question` did it). 2.52 has every
  `SCE-PS3-*` font Killzone 2 opens.
- The laptop's last, uncommitted experiment (2026-10-26: inline PPU stores
  with the reserved-line bitmap test, live reservation counts per line, a
  test-and-test-and-set lock-line lock) stopped the game in its last profile
  -- 71% of all samples in `spu_lockline_lock`, four SPU threads at 100%. It
  is kept as `patches/wip/inline-stores-resv-count.patch` (not applied) and
  the submodule is back on the series; upstream has since done the same
  inline bitmap test (`29154bc6`, v0.18.0).

**Full strength** (`KZ2_MAX=1 tools/build.sh`): `-march=native`, the lifted
PPU chunks at `-O2` (32 GB allow ~3 GB per job), the SPU code with
`pgo/spu.profdata`. ~7 min at `-j8` vs ~4.5 min for the default. Gameplay
with the test script, 90-180 s, headless: default build 11.2-13.0 frames/s,
full strength 12.4-13.4.

**Register caching from the inFAMOUS port** (`tools/spu_regcache.py`, from
whompay/inFAMOUSreborn's ps3recomp, MIT): keeps each lifted SPU function's
registers in locals instead of `ctx->gpr`. It applies cleanly here -- every
statement of our SPU lifter's output is one line, no `else`/loops, every
`goto` has a register-only condition; 24,165 functions -- but it is slower:
11.4-11.6 frames/s, `a27e818cca34` 39 -> 51 ms of CPU per frame. Its hot loop
(`0x5908`) uses ~80 SPU registers; as locals they no longer fit the 16 x86
vector registers and spill to the stack (970 stack accesses, 3,394
instructions vs 2,624 with 291 `ctx` accesses -- clang already keeps
`ctx->gpr` in registers between accesses). Kept opt-in
(`KZ2_SPU_REGCACHE=1 tools/lift_spu.sh`, CMake `KZ2_SPU_REGCACHE=ON`).

**Where the frame goes on six cores** (`tools/prof_perf.sh`, 120 s
warm-up): 5.1 cores busy on average, so the CPU itself is nearly full; one
SPU worker at 99% runs `jobmod_73d064721abc` (62 ms of CPU per frame; absent
from the laptop's profiles at 210 s), the SPU runtime (drain, lock line,
coherency) 72 ms, `a27e818cca34` 39 ms. One run in six stopped at the level
start (frames end ~60 s in; one thread spins in `spu_coh_reserve` under
`jobmod_0f8584c7c87a`): the occasional stop of section 61 is still there.

## 63. The test runs play Corinth River; two upstream fixes; a cheaper SPU float check

**What the test script plays now.** Frame dumps (`PS3RECOMP_VK_DUMP`,
`PS3RECOMP_VK_DUMP_EVERY=400`) show that `run_test/` holds a campaign in
progress -- the launcher's quick start uses that directory, and the hand
sessions of section 61 saved there -- so the script's Cross on the main menu
picks *Continue* and the game loads the Corinth River checkpoint, the
battle after the prologue ("Rico: Sev, this way!"). Every desktop number of
section 62 is from there, not from The New Sun, which is why they do not
compare with the laptop's. It also means the third stop of section 61 --
continuing from that checkpoint -- no longer happens: a dozen runs loaded it
and played on. `run_bench_tpl/` keeps a copy of those saves and
`KZ2_RUN_TEMPLATE=<dir>` (longrun, prof_perf) starts each run from a fresh
copy, since the game saves as it goes.

In that scene one SPU worker is sometimes 99% busy with `73d064721abc`
(a windowed sum of squares at `0x4898`, 85 ms of CPU a frame) and sometimes
does not run it at all, at the same frame rate: it is not on the frame's
path. The frame is: the FIFO thread drains for ~half of it and is parked
the other half (`KZ2_RSX_TIMES=1`: drain 50%, parked 50%) waiting for the
post-processing jobs to patch the command buffer (section 23); no thread is
saturated.

**Two upstream commits that apply to the series as they are** (patch 0037;
most of v0.18.0's performance work does not apply without the refactors
before it): `13a7676b` copies only the reserved lines of a PUT under the
lock-line lock, `06a37689` stops the EA->IO lookup behind every SPU PUT at
the highest slot ever used instead of walking all 512 (`gcm_note_spu_put`
left the profile). Their tests pass (`test_spu_lockline_peer` 56/56,
`test_gcm_ea2io_range`). Also in 0037: `[spu-spin]` prints the whole
128-byte line and r3-r6.

**The SPU float fast path tests the result only** (patch 0038). In the
round-toward-zero MXCSR mode an `fa`/`fm`/`fma` lane needs the exact slow
path when an operand has exponent 255 (the SPU's extended range) or the
result overflowed. IEEE arithmetic already turns any exponent-255 operand
into an exponent-255 result (x + Inf = Inf, Inf * 0 = NaN, Inf - Inf = NaN),
and an overflow lands on FLT_MAX under round toward zero, so the result's
magnitude alone decides -- four of an `fa`'s ~12 instructions and seven more
of an `fma`'s. `tests/test_spu_fma.c` (8 M random vectors, extended and
denormal operands, both MXCSR modes): 0 mismatches; with the threshold
moved off by one it reports 1.37 M, so it does test this. CPU per frame:
`9935c8f700db` 18.7 -> 12.6 ms, `78f2ef397e8f` 14.2 -> 12.7,
`a27e818cca34` 36.8 -> 34.6. Frame dumps unchanged.

**`getenv` on hot paths** (patch 0038): an `LD_PRELOAD` counter found ~9,000
calls a second in the level, from `spu_run_with_halt` (every SPU start,
`SPU_ARGWATCH`), the task dispatch (four switches) and the FIFO walker
(upstream `3b27abdb`'s list, done by hand); each is now read once. ~140 a
second remain, none per packet.

Measured and not kept: the lifted SPU code with `-fstrict-aliasing` (code
size and thread-local reloads unchanged), and MangoHud off (it is enabled
system-wide here; no change in frame rate).

## 64. The relay, measured; more SPUs for the other workloads; what did not help

Desktop, Corinth River from `run_bench_tpl/`, 160 s runs, the mean of the
last 14 five-second windows (~±0.5 frames/s between runs).

**The frame.** Per 5 s (`PS3RECOMP_VK_SUBMIT_STATS=1`): ~90,000 draws in
~19,000 render passes (~1,400 draws and ~300 passes a frame), 6.9 GB through
the batch ring (vertices 533 MB, indices 316 MB, VS constants 438 MB). The
FIFO thread is ~31% busy; `KZ2_RSX_TIMES`: drain ~48%, parked ~52%. A 1 ms
timeline (`kzprof_timeline.py NAME 8000 170 1`) shows the relay of section
59 plainly: while the FIFO thread draws, most SPU workers are idle; while it
is parked, the post-processing chain runs -- `e569adaf4ffe`, `a27e818cca34`
on four SPUs, then `9935c8f700db` and `78f2ef397e8f` **one after the other on
one SPU** (~25 ms). Gdb samples of the main thread: half in its own code, a
quarter in `sys_cond_wait` under `func_004EED70` (the Havok step), and in
`func_004D3D00`, which waits for a job counter with `sys_timer_usleep(1)`.

**More SPUs for the other workloads** (`boot_main.cpp`, default
`SPURS_MAXCONT=0:6,2:6,3:6,4:6,5:6,7:4,8:6,9:6`): the title limits most of
its workloads to fewer SPUs than it has; raised, their jobs run side by side.
`SPURS_PM_STATS=1` per 5 s: w7 (jm_post) ~5.9 s of SPU time, w3 ~3.0 s, w8
~2.0, w4 ~1.5, w5 ~1.0, w9 ~0.8. Frame rate: 13.53 / 13.45 with `7:4`;
7:4 + 8:4 13.74; all others at 4: 14.27, 14.73; all at 6 including jm_post:
14.31; **jm_post at 4, the others at 6: 15.53, 14.56, 14.59**.
`SPURS_MAXCONT=7:4` restores the old behaviour.

**VS constants by what the program reads** (patch 0039): before ~800 of the
~1,400 draws a frame a constant changed, and the engine then copied all 514
vec4 (8 KB) into its block, the backend compared and copied them, and the
draw copied them into the ring. `rsx_vp_analyze_constants` names the
highest constant a program reads (any constant-type source field counts;
a relative `a0` read or a missing end bit means all 512); only that prefix
and the viewport tail are compared and written, the ring slot stays full
size so the shader layout does not change, and a program that reads more
than was last written refreshes it (`vp_gen`, bumped by every transform
program word, keys the analysis). VS constants through the ring 438 -> 275 MB
per 5 s (skinned meshes index their bones, so they still take all); frame
dumps unchanged; no frame-rate change measurable.

**Tried and not kept**, all within noise or worse:
- FIFO drain every 200 us (`KZ2_DRAIN_US=200`): parked stays ~50% -- the
  parks wait for SPU work, not for the next poll.
- SPURS kernel pass every 250 us: 12.4-13.0, worse, as on the laptop.
- 1 us timer slack for every thread (`KZ2_TIMER_SLACK_NS`, kept as an
  opt-in): 13.1 vs 13.7.
- the main thread's progress wait sleeping after 4,096 passes
  (`KZ2_SPIN_SLEEP_US=20|50`, opt-in): 13.46 / 13.43 vs 13.53.
- **`--merge-chunks` for the job modules** (`JOBMOD_LIFTER_ARGS`, patch
  0039): the SPU lifter splits loops at every branch target, so e.g.
  `73d064721abc`'s bit-unpacking loop (`0x6254` body, `0x6280` test) returns
  to the drain loop twice per iteration. Lifted with merged chunks (gotos
  77 -> 928 in that module), the drain then resumed at a lifted entry
  millions of times (`[spu] drain-resume return_pc=0x032C0 at lifted entry
  0x06E90`, 31 M lines -- past its own 8-line cap) and the frame rate fell to
  10. Not understood yet; the default lift is back. Splitting less is still
  the largest unclaimed SPU saving.
- PGO from this scene (`KZ2_PGO=10000,10700`, merged 2:1 with the laptop's
  New Sun profile into `pgo/spu.profdata`): `a27e818cca34` 35.1 -> 33.2 ms;
  kept.

## 65. Stability with the new SPURS limits; the merged-chunk livelock, narrowed

**Stability.** With `SPURS_MAXCONT=0:6,2:6,3:6,4:6,5:6,7:4,8:6,9:6`: four
runs in a row to gameplay (`tools/soak.sh 4 170`, 14.3-15.0 frames/s in the
last minute) and three two-instance hunts (`tools/stallhunt.sh 2 230`, six
runs at once in pairs): ten of ten reached gameplay and kept presenting.

**`[spu] drain-resume` is normal** (patch 0040). Its counter was an `int`
capped at 8; it now counts in 64 bits and prints the first 8 and every
2^k-th. The plain lift resumes ~20,000 times a second in the level -- every
WWS job module's body ends `bi $r0` to a continuation whose host frame is
gone (spu_drain.c's comment), `return_pc=0x32C0`, entries 0x40DC / 0x412C.
The 31 M lines of section 64 were the old counter wrapping after 2^32
resumes, i.e. a real livelock.

**The livelock under `--merge-chunks`** (job module `b5dd5976bcbd`, overlay
202): the drain resumes at `0x6E90`; that chunk branches to `0x6DB4`, whose
lifted function is `r3 = r15; SPU_RET(ctx)` -- and r0 holds `0x6E90`, so the
return lands where it started, 4.3 billion times. The routine is a memcpy
(`0x6D58`..`0x6EC0`, merged into one group). Things ruled out:
- interrupts and the per-hop checks: a back-edge yield (return to the drain
  at a merged loop head when an interrupt is pending, and every 1,024
  iterations; 4,062 sites) changed nothing -- reverted;
- the module alone: with only `b5dd5976bcbd` (or only `73d064721abc`) merged
  there is no livelock in 240 s; it needs several merged modules, so the
  stale r0 is probably left by another merged module's code on the same SPU
  (a link register that the chunked lift rewrites at a trampoline and the
  merged one does not).
The next step is a watch on r0 at the `0x6D58` entry under the merged lift.
Until then the plain lift stays, and loops split across chunks keep paying
two drain round trips per iteration (`73d064721abc`'s `0x6254`/`0x6280`).

## 66. Cheaper lifted calls; SPURS priorities; what the FIFO thread does

**The drain after every lifted call, inline** (patch 0041). Every `brsl`
in the lifted SPU code (~6,300 sites in the job modules) calls
`spu_drain_call(ctx, return_pc)` after the callee: an out-of-line call, the
depth guard (another call), an interrupt frame, a trampoline loop. In the
common case the callee returned on its own -- no trampoline pending, pc at
the return address -- and all that does is reset `drain_ret_pc`.
`spu_drain_call_inl` (spu_context.h, the lifted sources only via
`SPU_USE_DISPATCH_MT`) does that inline for calls less than 256 deep and
falls back otherwise. `spu_drain_call` 4.1 -> 2.5% of all samples,
`spu_depth_guard` 1.0 -> 0.2%, the SPU runtime ~82 -> ~72 ms of CPU a frame;
frame rate unchanged (14.48 / 15.00 before, 14.75 / 14.16 after). Kept: the
same semantics for less CPU.

**SPURS priorities** (patch 0042, opt-in). The kernel took, for a free
SPU, the first eligible workload in round-robin order; the per-SPU priority
bytes were only read as "0 = not on this SPU". `SPURS_PRIO_LOG=1`: Killzone
2 gives most of its workloads priority 8, w4 7, w8 and w9 1, jm_post (w7)
8. `SPURS_PRIORITY=1` makes a free SPU take a workload with work before one
without and then the best priority, as the real kernel does: 14.73 frames/s;
jm_post at priority 1 (`SPURS_PRIO_OVERRIDE=7:1`, also `9:2,8:2`): 14.57.
Neither changes anything, so the order is not what holds the
post-processing back: when its jobs are ready there are free SPUs. Off by
default. (The `SPURS_MAXCONT` parsing moved into `spurs_maxcont_override`,
same behaviour.)

**Where the FIFO thread is** (30 gdb samples in gameplay): 17 between
drains (parked or sleeping), 13 inside `cellGcm_rsx_process_fifo`, every one
of those on the CPU -- vertex fetch and its plan, texture slots and hashing,
descriptor writes, the fragment program's structural hash. No hidden wait
inside the drain: its half of the frame is CPU work, ~24 ms of it a frame.

**What 60 frames/s would take**, from sections 59-66: the frame is the
FIFO thread's ~35 ms of draw translation followed by ~30 ms of the SPU
post-processing chain, and neither overlaps the other. Every latency knob
(drain interval, SPURS pass, timer slack, spin backoff, priorities) is
neutral; only more parallelism moved it (section 64). Left, largest first:
1. **SPU code that does not leave a loop every iteration**: the lifter
   starts a function at every branch target, so a loop with a branch inside
   runs as several C functions joined by trampolines (two drain round trips
   per iteration in `73d064721abc`'s bit loop). `--merge-chunks` removes
   them but livelocks (section 65: a stale r0 after merged code); fixing it
   is the largest single SPU saving.
2. **A second RSX thread**: the draw engine decodes on the FIFO thread and a
   submission thread records Vulkan and fills the ring. Needs the label and
   semaphore writes held until the submission thread has read what the
   draws reference, or the guest reuses its buffers early.
3. **ps3recomp v0.16-v0.18** (578 commits since our pin): its SPU drain,
   lock-line, vertex-fetch and draw-engine performance work does not apply
   to this series without the refactors before it; a rebase of the 42
   patches is the way to get it.
4. Killzone 2 was made for 30 frames/s; above that its own timing has not
   been tested.
