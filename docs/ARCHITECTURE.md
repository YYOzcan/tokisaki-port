# Architecture

This replaces the first draft of this file, written before any code existed.
That draft described a home-grown runtime (`src/runtime/memory.*`,
`spu_manager.*`) that was never built and was removed in commit `ac0d57e`;
the port has used ps3recomp's runtime ever since.

## What runs

```
your disc ──► EBOOT.ELF (decrypted locally, never committed)
                │
                ├─ ps3recomp ppu_loader.py  ─► out/EBOOT.*.json   (functions, imports, segments)
                ├─ ps3recomp ppu_lifter.py  ─► recompiled/ppu_recomp_*.cpp   (67,196 functions)
                └─ tools/lift_spu.sh        ─► spu_recompiled/          (SPU images, WWS PM, job modules)
                                                    │
src/boot_main.cpp  src/kz2_hle.cpp  src/kz2_spu_pm.c │
          │                 │                        │
          └──────── linked with ps3recomp_runtime ◄──┘   (external/ps3recomp, built from source)
                                 │
                         build/killzone2  (native x86-64, Linux; Windows path untested)
```

Every guest function becomes a C++ function `func_XXXXXXXX(ppu_context*)`;
guest registers live in the context struct, guest memory is reached through
big-endian accessors (`vm_read32` ...). Firmware imports become
`ps3_hle_call(nid)` and land in ps3recomp's HLE libraries, or in
`src/kz2_hle.cpp` where this title needs something the runtime lacks.

## Memory model

The PPU uses 32-bit effective addresses. The runtime reserves one host block
the size of that address space and treats every guest pointer as an offset
into it (`host = vm_base + ea`). This models the *address map* — the ELF at
0x00010000, sys_memory heaps at 0x40000000, the GCM label/control window,
thread stacks at 0xD0000000 — not the console's physical memory, which is
256 MB XDR (main) + 256 MB GDDR3 (RSX local). The reservation is
`MAP_NORESERVE`, so only pages the title touches are backed. RPCS3 uses the
same technique.

## Threads

Each guest PPU thread is a host thread. Because guest calls are host calls,
deep guest call chains become deep host stacks, so guest threads get a large
host stack *reservation* (256 MB, ps3recomp's `PPU_HOST_STACK_BYTES`; the main
thread gets the same in `boot_main.cpp`). It is virtual address space, not
committed memory.

## SPU

SPU programs are lifted ahead of time and dispatched by content fingerprint:

| Kind | Where it lives | Lifted at |
|---|---|---|
| 23 SPU ELF images (SPURS tasks, e.g. `edgezlib_inflate_task`) | embedded in the EBOOT | their ELF addresses |
| WWS job-manager policy module (all `Wws_Job` workloads) | raw, guest 0xD8B000, 0x2E80 bytes | LS 0xA00 |
| 36 WWS job modules (`C0DEC0DE` header) | embedded in the EBOOT | LS 0x4000 overlays |

The policy module's SPU interrupt vector (LS 0xA2C) and the return points of
the stub it generates at runtime are added as extra entries; without them the
module could never finish loading a job.

## Graphics

ps3recomp's RSX command processor walks the title's GCM FIFO and drives its
register-file draw engine on Vulkan (`-DPS3RECOMP_RSX_VULKAN=ON`, guest vertex
and fragment programs translated through glslang). `KZ2_RSX=null` selects the
headless backend for diagnostics; `tools/run.sh` uses Vulkan.

## Where the title-specific code is

| File | Purpose |
|---|---|
| `src/boot_main.cpp` | process entry, guest space, frame clock, backend pick, diagnostics |
| `src/kz2_hle.cpp` | Edge Zlib on the host, cellFont SDK-3 entry points and firmware fonts, 9-argument cellSaveData |
| `src/kz2_spu_pm.c` | registers the lifted WWS policy module |
| `patches/ps3recomp/` | fixes to the toolkit itself (lwcond ABI, two lifter fixes) |
| `tools/` | lift/build/run scripts and the reverse-engineering helpers |

Progress and the reasoning behind each change: [PROGRESS.md](PROGRESS.md).
