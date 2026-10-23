# Tokisaki Port (Killzone 2 PC Port)

[![Project Status](https://img.shields.io/badge/Status-In--game%20(slow)-orange.svg)]()
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Target Platform](https://img.shields.io/badge/Target-PC%20(x86__64)-lightgrey.svg)]()

> [!WARNING]
> **Work in progress — playable only in a limited sense.**
> The campaign starts, the opening cutscene on *The New Sun* plays and you can play the first level with keyboard and mouse or a controller, with sound — but at **~5-6 frames/s** (Radeon 680M; the game's own clock then runs at about a third of real time), and the level start can still stop during the load now and then (the Havok physics stall behind most of those is fixed). Details below and in docs/PROGRESS.md §40-53.
> **Devam eden bir çalışma — ancak sınırlı anlamda oynanabilir.** Kampanya başlıyor, *The New Sun* açılış sinematiği oynuyor ve ilk bölüm klavye+fare ya da kontrolcüyle, sesli olarak oynanabiliyor; ancak **~5-6 kare/s** hızında (Radeon 680M; oyunun kendi saati gerçek zamanın yaklaşık üçte biri hızında akıyor), ve bölüm yüklenirken ara sıra hâlâ takılabiliyor (bu takılmaların çoğunun sebebi olan Havok fizik sorunu düzeltildi).
>
> Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp); the boot harness is adapted from [Kurtisone/twistedmetal](https://github.com/Kurtisone/twistedmetal). Developed with an AI coding agent. See [CREDITS.md](CREDITS.md).

[English](#english) | [Türkçe](#türkçe)

---

<a id="english"></a>
## English

Static recompilation project for **Killzone 2** (PlayStation 3) targeting native PC execution.

### 📊 Project Status: boots, streams data, renders its first frames (pre-alpha)

Built on **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** (v0.15.0, pinned as a submodule, with this port's fixes in `patches/ps3recomp/`). Linux/Vulkan is the primary host.

| Stage | State |
|---|---|
| EBOOT → ELF, PPU lift | **done** — 34,145 OPD functions → 67,196 lifted, 41 chunks, ~6.1 M lines of C++, 234 switch tables |
| SPU lift | **done** — 23 embedded SPU ELFs, the WWS job-manager policy module, 36 WWS job modules |
| Build & link | **done** — ~118 MB native x86-64 binary (clang, ~3 min at -j8) |
| Boot | CRT, TLS, SPURS ×2, FIOS, cellGcm 720p, display buffers/tiles/zcull, cellFont, cellSaveData, cellAudio, trophies |
| Asset streaming | **works** — FIOS reads `data_ps3.psarc`; Edge Zlib requests are inflated on the host |
| Rendering | Vulkan, batched submissions; menus at ~250 presented frames/s; the level renders its deferred passes, shadows and post-processing (teal cast: the colour-grading LUT is still an identity fill) |
| Front end | language select → gamma → main menu → save warning → start screen → campaign difficulty → campaign start |
| Movies / gameplay | intro movie player deadlocks (skipped via `tools/make_vfs_nointro.sh`); Corinth River loads in ~70 s, the opening cutscene on *The New Sun* plays and gameplay starts ("Follow Garza to the Launch Bay") at **~5-6 frames/s** on the Radeon 680M (~2.5 on the RTX 4050 Laptop, whose clocks stay low under this load) |
| Audio | menus and level |
| Input | keyboard + mouse (first-person layout) or an SDL game controller |
| Stability | the Havok-step stall at level start is fixed (§53); an occasional stop during the level load remains |

![Gameplay: first objective on The New Sun, "Follow Garza to the Launch Bay"](docs/media/new-sun-first-gameplay.png)

![Opening cutscene: Garza at the window of The New Sun](docs/media/new-sun-cutscene-garza.png)

![First in-level image: the cryo bay of The New Sun before entity updates ran](docs/media/in-level-intruder-first-image.png)

![Main menu on ps3recomp v0.15.0: every glyph present](docs/media/main-menu-v015.png)

![Campaign loading screen: Corinth River with the mission briefing](docs/media/corinth-river-loading-v015.png)

Full engineering log: [docs/PROGRESS.md](docs/PROGRESS.md).

#### Roadmap
- [x] **Phase 0: Runtime & Build System** (ps3recomp submodule, CMake, Linux/Vulkan runner)
- [x] **Phase 1: ELF extraction & analysis** (imports, SPU images, job modules)
- [x] **Phase 2: Static recompilation** (PPU + SPU lifted and linked)
- [ ] **Phase 3: HLE / OS layer** — in progress (lwcond ABI, Edge Zlib, cellFont fixed; SVO front end next)
- [ ] **Phase 4: RSX rendering** — first passes render; final composition missing
- [ ] **Phase 5: Audio, input, game loop**
- [ ] **Phase 6: Playability**

### Name
The repository is named after Kurumi Tokisaki (*Date A Live*), a character who commands time — a nod to bringing a 2009 game forward. The name is the only thing it borrows; the engineering is below.

#
### Play in a window / Pencerede oynatma

```
tools/play.sh
```

| Key / Tuş | Pad | | Key / Tuş | Pad |
|---|---|---|---|---|
| W A S D | left stick (move) | | Mouse / Fare | right stick (look) |
| Left click / Sol tık | R1 (fire) | | Right click / Sağ tık | L1 (aim) |
| Space | Cross (jump) | | Left Ctrl | Circle (crouch) |
| R / F | Square (reload) | | T / C | Triangle (weapon) |
| G | R2 (grenade) | | Z | L2 (cover) |
| Left Shift | L3 (sprint) | | V / middle click | R3 (melee) |
| Enter | START (pause) | | Esc / Backspace | Circle (back) |
| Arrow keys / Ok tuşları | D-pad | | Tab | SELECT |
| F1 | release / recapture the mouse | | | |

An SDL game controller also works. On a hybrid laptop the integrated GPU is
used by default (`KZ2_GPU=dgpu tools/play.sh` for the discrete one);
`PS3_PAD_MOUSE_SENS` sets the look speed.

## Credits
Most of what runs is other people's work: the **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** toolkit and runtime (Ned Heller / sp00nznet and contributors), and the Linux boot harness of **[Kurtisone/twistedmetal](https://github.com/Kurtisone/twistedmetal)**, both MIT. Bugs fixed in `patches/ps3recomp/` were first found by others; see **[CREDITS.md](CREDITS.md)** and **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**. This port is developed by YYOzcan with an AI coding agent (Claude); the reasoning behind every change is in [docs/PROGRESS.md](docs/PROGRESS.md).

### Project Layout
```
├── CMakeLists.txt          # links the lifted game against the ps3recomp runtime
├── external/ps3recomp/     # toolkit + runtime (git submodule, pinned)
├── patches/ps3recomp/      # this port's fixes to the toolkit (tools/apply_patches.sh)
├── src/
│   ├── boot_main.cpp       # Linux/Windows runner: guest VM, frame clock, backend pick
│   ├── kz2_hle.cpp         # Killzone 2 HLE overrides (Edge Zlib, cellFont, ...)
│   ├── kz2_spu_pm.c        # registers the lifted WWS job-manager policy module
│   └── kz2_ftrace.cpp      # function-entry histogram (diagnostic builds)
├── tools/                  # lift / build / run / diagnostics scripts
└── docs/                   # PROGRESS.md (engineering log), ARCHITECTURE.md
```
Game files, the lifted C++ (`recompiled/`, `spu_recompiled/`) and loader output (`out/`) are generated from **your own** disc and are never committed.

### Building (Linux)
Requirements: clang, CMake ≥ 3.20, Ninja, Python ≥ 3.10, SDL2, Vulkan headers, glslang, zlib; a decrypted `EBOOT.ELF` in `game_dump/PS3_GAME/USRDIR/`.
```bash
git clone --recursive https://github.com/YYOzcan/tokisaki-port   # or: git submodule update --init
tools/apply_patches.sh      # this port's ps3recomp fixes
tools/lift.sh               # PPU: EBOOT.ELF -> recompiled/   (~1 min)
tools/lift_spu.sh           # SPU: images, policy module, job modules -> spu_recompiled/
BUILD_JOBS=8 tools/build.sh # ~0.6 GB RAM per job
```

### Running
```bash
tools/run.sh                # Vulkan window; KZ2_RSX=null for headless
```
`run/vfs/PS3_GAME` links to the dump, `/dev_hdd0` maps to `run/hdd0`, firmware fonts come from RPCS3's `dev_flash` (`PS3_DEV_FLASH`).

Diagnostics: `tools/snapshot.sh` (all thread stacks under gdb), `tools/sample.sh` (main-thread sampler), `tools/import_audit.py` (imports without HLE), `tools/psarc_map.py` (which archive members were read), `KZ2_PEEK` / `KZ2_DUMPAT` (watch guest memory).

---

<a id="türkçe"></a>
## Türkçe

**Killzone 2** (PlayStation 3) için geliştirilen, yerel PC çalıştırmasını hedefleyen statik yeniden derleme (static recompilation) projesidir.

### 📊 Proje Durumu: açılıyor, veri akıtıyor, ilk karelerini çiziyor (ön-alfa)

**[ps3recomp](https://github.com/sp00nznet/ps3recomp)** (v0.15.0, alt modül olarak sabitlendi; bu porta ait düzeltmeler `patches/ps3recomp/` altında) üzerine kuruludur. Ana hedef Linux/Vulkan'dır.

| Aşama | Durum |
|---|---|
| EBOOT → ELF, PPU çevirimi | **tamam** — 34.145 OPD fonksiyonu → 67.196 çevrilmiş fonksiyon, 41 parça, ~6,1 M satır C++ |
| SPU çevirimi | **tamam** — 23 gömülü SPU ELF, WWS iş yöneticisi policy module'ü, 36 WWS iş modülü |
| Derleme | **tamam** — ~118 MB yerel x86-64 çalıştırılabilir dosya |
| Açılış | CRT, TLS, 2×SPURS, FIOS, cellGcm 720p, ekran tamponları, cellFont, cellSaveData, cellAudio, kupalar |
| Varlık akışı | **çalışıyor** — FIOS `data_ps3.psarc` okuyor; Edge Zlib istekleri host'ta açılıyor |
| Grafik | Vulkan, toplu gönderim; menüler ~250 kare/s; bölümde ertelenmiş aydınlatma, gölgeler ve son işlem çiziliyor (deniz mavisi ton: renk tablosu hâlâ birim dolgu) |
| Menüler | dil seçimi → gama → ana menü → kayıt uyarısı → başlangıç ekranı → zorluk seçimi → kampanya başlangıcı |
| Video / oynanış | giriş videosu oynatıcısı kilitleniyor (`tools/make_vfs_nointro.sh` ile atlanıyor); Corinth River ~70 s'de yükleniyor, *The New Sun* açılış sinematiği oynuyor ve oynanış başlıyor ("Follow Garza to the Launch Bay"): Radeon 680M'de **~5-6 kare/s** (RTX 4050 Laptop'ta ~2,5: saat hızı bu yükte düşük kalıyor) |
| Ses | menülerde ve bölümde |
| Kontrol | klavye + fare (FPS düzeni) veya SDL kontrolcü |
| Kararlılık | bölüm başındaki Havok fizik takılması düzeltildi (§53); yüklemede ara sıra bir durma kalıyor |

Ayrıntılı mühendislik günlüğü: [docs/PROGRESS.md](docs/PROGRESS.md).

#### Yol Haritası
- [x] **Aşama 0: Çalışma zamanı ve derleme sistemi**
- [x] **Aşama 1: ELF çıkarımı ve analiz**
- [x] **Aşama 2: Statik çevirim (PPU + SPU)**
- [ ] **Aşama 3: HLE / işletim sistemi katmanı** — sürüyor
- [ ] **Aşama 4: RSX grafik** — ilk geçişler çiziliyor, son kompozisyon eksik
- [ ] **Aşama 5: Ses, giriş, oyun döngüsü**
- [ ] **Aşama 6: Oynanabilirlik**

### İsim
Depo adını *Date A Live*'daki, zamanı yöneten karakter Kurumi Tokisaki'den alır — 2009'dan kalma bir oyunu bugüne taşımaya bir gönderme. Karakterden alınan tek şey isimdir.

### Katkılar ve Lisanslar
Çalışan kodun büyük kısmı başkalarının emeğidir: **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** araç seti ve çalışma zamanı (Ned Heller / sp00nznet ve katkıda bulunanlar) ile **[Kurtisone/twistedmetal](https://github.com/Kurtisone/twistedmetal)** projesinin Linux açılış iskeleti; ikisi de MIT lisanslıdır. `patches/ps3recomp/` içindeki hataları ilk bulanlar başkalarıdır. Ayrıntılar: **[CREDITS.md](CREDITS.md)**, **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**. Bu port YYOzcan tarafından bir yapay zekâ kodlama ajanı (Claude) ile geliştiriliyor; her değişikliğin gerekçesi [docs/PROGRESS.md](docs/PROGRESS.md) içinde.

### Derleme (Linux)
Gerekenler: clang, CMake ≥ 3.20, Ninja, Python ≥ 3.10, SDL2, Vulkan başlıkları, glslang, zlib; `game_dump/PS3_GAME/USRDIR/` içinde şifresi çözülmüş `EBOOT.ELF`.
```bash
git clone --recursive https://github.com/YYOzcan/tokisaki-port   # veya: git submodule update --init
tools/apply_patches.sh      # bu portun ps3recomp düzeltmeleri
tools/lift.sh               # PPU çevirimi -> recompiled/
tools/lift_spu.sh           # SPU çevirimi -> spu_recompiled/
BUILD_JOBS=8 tools/build.sh
```

### Çalıştırma
```bash
tools/run.sh                # Vulkan penceresi; başsız için KZ2_RSX=null
```
Oyun dosyaları ve diskinizden üretilen C++ kodu depoya asla eklenmez.
