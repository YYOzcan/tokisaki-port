# Tokisaki Port (Killzone 2 PC Port)

Static recompilation project for **Killzone 2** (PlayStation 3) targeting native PC execution.

---

## ⏳ Naming & Inspiration: Kurumi Tokisaki (時崎 狂三)

Bu proje, adını ve felsefi ilhamını *Date A Live* serisinin ikonik karakteri **Kurumi Tokisaki**'den (時崎 狂三) almaktadır.

### Neden "Tokisaki Port"?
- **Zaman Manipülasyonu ve Zamansal Geçiş (Zafkiel)**: 
  Kurumi Tokisaki, zamanı bükebilen, geçmişe ve geleceğe müdahale edebilen melek **Zafkiel**'in (刻々帝) efendisidir. Bu port projesi de benzer bir felsefeyle hareket eder: 2009 yılında PlayStation 3'ün karmaşık **Cell Broadband Engine** mimarisine mühürlenmiş olan Killzone 2 ikili kodunu (binary), statik yeniden derleme yoluyla zamanın ötesine taşıyarak modern C++20 standartlarında yerel (native) x86_64 mimarisine dönüştürür.
- **Klonlar ve Eşzamanlılık (City of Devouring Time)**:
  Kurumi'nin kendi zaman çizelgelerinden çağırdığı sayısız klonunun eşzamanlı çalışması, PlayStation 3'ün 6 adet **SPU (Synergistic Processing Unit)** iş parçacığının ve PPU mimarisinin modern çok çekirdekli PC işlemcilerinde yüksek paralellikte çalıştırılması prensibiyle birebir örtüşür.
- **Zarafet ve Kusursuz Hassasiyet**:
  Cell mimarisinin asenkron DMA kuyrukları, vektörel AltiVec/SPU talimatları ve karmaşık bellek modelleri; Kurumi'nin saat kadranı üzerindeki mermileri gibi kusursuz bir zamanlama ve milisaniyelik senkronizasyon gerektirir.

---

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
