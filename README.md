# Tokisaki Port (Killzone 2 PC Port)

[English](#english) | [Türkçe](#türkçe)

---

<a id="english"></a>
## English

Static recompilation project for **Killzone 2** (PlayStation 3) targeting native PC execution.

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
