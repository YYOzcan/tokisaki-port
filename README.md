# Tokisaki Port (Killzone 2 PC Port)

[![Project Status](https://img.shields.io/badge/Status-Not%20Started%20(0%25)-red.svg)]()
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Target Platform](https://img.shields.io/badge/Target-PC%20(x86__64)-lightgrey.svg)]()

[English](#english) | [Türkçe](#türkçe)

---

<a id="english"></a>
## English

Static recompilation project for **Killzone 2** (PlayStation 3) targeting native PC execution.

### 📊 Project Status: 0% (Not Started / Pre-Alpha)

> [!NOTE]
> **Current Progress: 0%**
> The project has not functionally started yet. Only the foundational repository architecture, CMake build system, and basic runtime skeletons (guest memory and SPU dispatchers) have been set up. Reverse engineering, binary disassembly, instruction lifting, and graphics pipeline porting have not begun. This tracker will be updated with every milestone.

#### Roadmap
- [ ] **Phase 0: Runtime & Build System Scaffolding** (Initial layout complete, 0% game code)
- [ ] **Phase 1: ELF Decryption, Extraction & Symbol Analysis** (0%)
- [ ] **Phase 2: Static Recompilation (PPU & SPU to C++20)** (0%)
- [ ] **Phase 3: Syscall & OS Layer Stubs (libsysmodule, sys_spu)** (0%)
- [ ] **Phase 4: RSX Rendering Pipeline (Vulkan Backend)** (0%)
- [ ] **Phase 5: Audio, Input & Game Loop Execution** (0%)
- [ ] **Phase 6: In-Game Playability & Bug Fixes** (0%)

### ⏳ Naming & Inspiration: Kurumi Tokisaki (時崎 狂三)

This project derives its name and philosophical inspiration from **Kurumi Tokisaki** (時崎 狂三), the iconic character from the *Date A Live* series.

- **Temporal Manipulation and Translation (Zafkiel)**:
  Kurumi commands **Zafkiel** (刻々帝), the Angel of Time, wielding mastery over past and future timelines. Tokisaki Port follows a parallel vision: liberating Killzone 2's binary from its 2009 entrapment within the complex **Cell Broadband Engine** architecture, bridging time via static recompilation to execute natively within modern C++20 standards on x86_64 host hardware.
- **Clones and Concurrency (City of Devouring Time)**:
  Kurumi's ability to summon and coordinate countless temporal clones perfectly mirrors the parallel dispatch of PlayStation 3's 6 user-accessible **SPU (Synergistic Processing Unit)** cores alongside the PPU onto modern high-core-count PC threads.
- **Ruthless Precision and Timing**:
  The asynchronous DMA queues, AltiVec/SPU vector instruction sets, and intricate memory barriers of the Cell architecture demand the exact same millisecond-grade precision as Kurumi's bullets traversing the clock dial.

### Key Features
- **C++20 Runtime Core**: Built on the modern C++20 standard for maximum efficiency and deterministic execution.
- **Cell SPU Pipeline**: Multi-threaded emulation of Cell Synergistic Processing Units with hardware-accelerated DMA channels.
- **Big-Endian Transparent Memory Mapping**: Flat 4 GB virtual address space reservation allowing zero-overhead 1:1 pointer translation.
- **Modern Hardware Optimization**: Tailored for contemporary multi-core architectures with SIMD support (AVX2 / AVX-512).

### Project Layout
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

### Building
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Running
```bash
./build/tokisaki-port [path/to/EBOOT.ELF]
```

---

<a id="türkçe"></a>
## Türkçe

**Killzone 2** (PlayStation 3) için geliştirilen, yerel PC çalıştırmasını hedefleyen statik yeniden derleme (static recompilation) projesidir.

### 📊 Proje Durumu: %0 (Henüz Başlamadı / Ön Geliştirme)

> [!NOTE]
> **Mevcut İlerleme: %0**
> Proje henüz fiilen başlamamıştır. Şu ana kadar yalnızca temel repo altyapısı, CMake derleme sistemi ve başlangıç çalışma zamanı iskeleti (konak bellek ve SPU yöneticisi taslağı) oluşturulmuştur. Tersine mühendislik, ELF çözümleme, kod çevirimi ve grafik iş hattı portlama çalışmaları henüz başlamamıştır. Her aşama kaydedildikçe bu alan güncellenecektir.

#### Yol Haritası
- [ ] **Aşama 0: Repo ve Çalışma Zamanı İskeleti** (İlk yapılandırma hazır, %0 oyun kodu)
- [ ] **Aşama 1: ELF Şifre Çözümü, Ayrıştırma ve Sembol Analizi** (%0)
- [ ] **Aşama 2: Statik Kod Çevrimi (PPU & SPU'dan C++20'ye)** (%0)
- [ ] **Aşama 3: Syscall ve İşletim Sistemi Stub'ları (libsysmodule, sys_spu)** (%0)
- [ ] **Aşama 4: RSX Grafik İş Hattı (Vulkan Arka Yüzü)** (%0)
- [ ] **Aşama 5: Ses, Giriş Kontrolleri ve Oyun Döngüsü** (%0)
- [ ] **Aşama 6: Oyun İçi Oynanabilirlik ve Hata Giderme** (%0)

### ⏳ İsimlendirme ve İlham: Kurumi Tokisaki (時崎 狂三)

Bu proje, adını ve felsefi ilhamını *Date A Live* serisinin unutulmaz karakteri **Kurumi Tokisaki**'den (時崎 狂三) almaktadır.

- **Zaman Manipülasyonu ve Zamansal Geçiş (Zafkiel)**:
  Kurumi Tokisaki, zamanı bükebilen, geçmişe ve geleceğe hükmedebilen melek **Zafkiel**'in (刻々帝) efendisidir. Bu port projesi de aynı vizyonla hareket eder: 2009 yılında PlayStation 3'ün karmaşık **Cell Broadband Engine** mimarisine hapsolmuş olan Killzone 2 ikili kodunu (binary), statik yeniden derleme vasıtasıyla zamanın ötesine taşıyarak modern C++20 standartlarında yerel (native) x86_64 mimarisine dönüştürür.
- **Klonlar ve Eşzamanlılık (City of Devouring Time)**:
  Kurumi'nin kendi zaman çizelgelerinden çağırdığı çok sayıda klonunun senkronize çalışması; PlayStation 3'ün 6 adet **SPU (Synergistic Processing Unit)** çekirdeğinin ve PPU yapısının modern çok çekirdekli PC işlemcilerinde yüksek paralellikte koşturulması mimarisiyle doğrudan örtüşür.
- **Kusursuz Hassasiyet ve Zamanlama**:
  Cell mimarisinin asenkron DMA kuyrukları, AltiVec/SPU vektör yönergeleri ve karmaşık bellek bariyerleri; Kurumi'nin saat kadranı üzerindeki mermileri gibi milisaniyelik bir zamanlama hassasiyeti gerektirir.

### Temel Özellikler
- **C++20 Çalışma Zamanı Çekirdeği**: Yüksek performans ve belirlenimli yürütme için modern C++20 standardıyla geliştirildi.
- **Cell SPU İş Hattı**: Donanım hızlandırmalı DMA simülasyonuna sahip çok iş parçacıklı Cell SPU emülasyonu.
- **Big-Endian Şeffaf Bellek Eşleme**: Sıfır ek yüklü 1:1 işaretçi çevrimi sağlayan 4 GB düz sanal bellek tahsisi.
- **Modern Donanım Optimizasyonu**: SIMD destekli (AVX2 / AVX-512) güncel çok çekirdekli işlemciler için tasarlandı.

### Proje Dizini
```
├── CMakeLists.txt         # Kök derleme yapılandırması (C++20)
├── config/                # Port yapılandırmaları ve oyun profilleri
│   └── config.toml
├── docs/                  # Mimari ve mühendislik notları
│   └── ARCHITECTURE.md
└── src/
    ├── common/            # Veri tipleri, endian dönüştürücüleri, SIMD yapıları
    ├── runtime/           # Bellek yöneticisi, SPU havuzu, çekirdek çalışma zamanı
    └── main.cpp           # Ana PC başlatıcı giriş noktası
```

### Derleme
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Çalıştırma
```bash
./build/tokisaki-port [path/to/EBOOT.ELF]
```
