# Tokisaki Port Architecture (Killzone 2 PC Port)

## Overview
Tokisaki Port is a high-performance native PC port framework for **Killzone 2** (PlayStation 3) utilizing static recompilation principles.

## Target Hardware vs Host Architecture
| Component | PS3 (Guest) | PC (Host) |
|---|---|---|
| Main CPU | Cell Broadband Engine (1 PPE + 8 SPEs @ 3.2 GHz) | Modern x86_64 / ARM64 Multithreaded CPU |
| Vector Engine | AltiVec / VMX128 + SPU 128-bit SIMD | AVX2 / AVX-512 / NEON |
| Endianness | Big-Endian | Little-Endian |
| Memory | 256 MB XDR + 256 MB GDDR3 (RSX) | Unified Host Virtual Memory (4GB+ mapping) |
| GPU | Reality Synthesizer (RSX / NV47 core) | Vulkan 1.3 / DirectX 12 Backend |

## Core Subsystems
1. **Memory Translation (`src/runtime/memory.*`)**:
   - 4 GB flat virtual address space mapping with `mmap`.
   - Fast 1:1 guest address conversion (`host_base + guest_addr`).
   - Big-Endian to Little-Endian transparent memory wrappers (`be_t<T>`).

2. **SPU Subsystem (`src/runtime/spu_manager.*`)**:
   - Emulates 6 user-accessible Synergistic Processing Units.
   - Dedicated 256 KB Local Storage (LS) per SPU.
   - High-throughput DMA (Memory Flow Controller - MFC) channel between LS and XDR memory.
   - Workload dispatch for Killzone 2's custom deferred rendering, skinning, and Havok physics jobs.

3. **Recompilation Pipeline**:
   - Decrypt & unpack EBOOT.BIN -> `EBOOT.ELF` + `SPU_*.ELF`.
   - Lift PowerPC 64-bit and SPU instructions to C++20 / LLVM IR.
   - Recompile statically into native host binaries (`out_*.cpp`).
