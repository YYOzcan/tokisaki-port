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
