# Tokisaki Port (Killzone 2 PC Port)

Static recompilation project for **Killzone 2** (PlayStation 3) targeting native PC execution.

## Features
- **C++20 Runtime Core**: High performance, modern standard compliant.
- **Cell SPU Pipeline**: Multi-threaded emulation of Cell Synergistic Processing Units with DMA simulation.
- **Big-Endian Transparent Memory Mapping**: 4 GB flat virtual address space with zero-overhead address translation.
- **Native Target**: Designed for x86_64 host systems with SIMD acceleration (AVX2/AVX-512).

## Project Layout
```
├── CMakeLists.txt         # Root build configuration (C++20)
├── config/                # Port configurations and game profiles
│   └── config.toml
├── docs/                  # Architecture & engineering notes
│   └── ARCHITECTURE.md
└── src/
    ├── common/            # Data types, endian conversion, vector structs
    ├── runtime/           # Memory manager, SPU worker manager, runtime orchestrator
    └── main.cpp           # Host executable entry point
```

## Building
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Running
```bash
./build/tokisaki-port [path/to/EBOOT.ELF]
```
