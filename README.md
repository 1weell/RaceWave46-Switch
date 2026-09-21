# RaceWave46 — Nintendo Switch Port

An unofficial Nintendo Switch port of RaceWave46, maintained by **[1weell](https://github.com/1weell)**.

This repository is focused on the Switch port work: ARM64 integration, Switch startup and window/bootstrap handling, controller input, NRO packaging, runtime safety fixes, the `1weell` application identity and the performance configuration used for hardware testing.

## Requirements

- Nintendo Switch running a compatible homebrew environment such as hbmenu or Sphaira.
- Your own compatible **Wave Race 64 USA Rev 1** ROM. No ROM or game assets are included.
- A compiled `WaveRace64Recompiled.nro` supplied separately by the maintainer.

## Installation

1. Create `sdmc:/switch/RaceWave46/` on the Switch SD card.
2. Copy `WaveRace64Recompiled.nro` into that directory.
3. Launch it through hbmenu or Sphaira.
4. Select/import your own Wave Race 64 USA Rev 1 ROM when prompted.

## Port details

The port uses **devkitPro/devkitA64**, **libnx**, **SDL2** for input/events and the **RT64 Vulkan/NVK** rendering path. The Vulkan/NVK bring-up is based on **[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)**, with the necessary integration work carried into this port.

The tested performance configuration disables diagnostics and was validated at the standard Switch clocks used during testing:

- CPU: **1.7 GHz**
- GPU: **300 MHz**
- Memory: **1600 MHz**

Performance and compatibility can vary with firmware, renderer, thermal conditions and the selected game settings.

## Credits and acknowledgements

- **[1weell](https://github.com/1weell)** — Nintendo Switch port, ARM64/devkitPro integration, port-specific runtime fixes, performance configuration and hardware testing.
- **[DomazinUS](https://github.com/DomazinUS)** — original RaceWave46 project and Windows port.
- **[Wiseguy / Mr-Wiseguy](https://github.com/Mr-Wiseguy)** — creator of **[N64Recomp](https://github.com/N64Recomp/N64Recomp)**, the static recompilation tool that makes this project possible. Thanks to its contributors and maintainers as well.
- **[danfromtico](https://github.com/danfromtico)** — **[mesa-switch](https://github.com/danfromtico/mesa-switch)** and the Vulkan/NVK bring-up work used as the basis for the Switch rendering path.
- **[Darío / DarioSamo](https://github.com/DarioSamo), Wiseguy and the [RT64 contributors](https://github.com/rt64/rt64/graphs/contributors)** — RT64 and its rendering foundation.
- **[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) contributors** — runtime, input and frontend infrastructure.
- **Nintendo and the original Wave Race 64 team** — for the original game and its enduring character.

This is an unofficial fan project and is not affiliated with or endorsed by Nintendo. Third-party components retain their applicable licenses; see [LICENSE](LICENSE), [COPYING](COPYING) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
