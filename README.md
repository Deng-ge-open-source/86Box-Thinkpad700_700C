# 86Box ThinkPad 700 / 700C

[English](README.md) | [简体中文](README.zh-CN.md)

Experimental IBM ThinkPad 700 (type 9552) emulation built on 86Box 6.0. This
fork adds the machine, firmware-facing hardware behavior, IBM 486SLC support,
MCA ESDI boot path, monochrome LCD presentation, and the input/display fixes
needed to run the IBM Reference Diskette and a period operating system.

![ThinkPad 700 Reference Diskette](docs/images/thinkpad700-final-reference-disk.png)

> [!IMPORTANT]
> This repository contains emulator source code only. IBM BIOS ROMs, Reference
> Diskettes, diagnostics, operating systems, hard-disk images, and other
> copyrighted software are **not** distributed here. Supply legally obtained
> copies yourself. The local `local-assets/` directory is intentionally ignored
> by Git.

## Project status

- Base: 86Box 6.0, upstream commit `4fef696a4`.
- Machine exposed by the UI: `[MCA] IBM ThinkPad 700 (9552)`.
- Internal machine name: `ibmtp700`.
- Primary tested guest: IBM Reference Diskette 1.20 and a 640×480 period
  Windows installation on a 370-cylinder MCA ESDI image.
- Primary development host: Linux ARM64. The source remains portable and the
  normal 86Box Windows and Linux build systems are retained.
- Status: experimental. The ThinkPad 700 path is usable, but it has not yet
  received upstream review and should not be treated as cycle-exact hardware
  preservation.

This is an independent community project. It is not affiliated with or
endorsed by IBM, Lenovo, or the upstream 86Box project.

## What was implemented

The work is larger than a machine-table entry. The firmware exercises several
PS/2- and ThinkPad-specific paths that previously did not exist in 86Box.

### ThinkPad 700 planar and firmware

- Added the IBM ThinkPad 700 / 9552 machine definition and the `ibmtp700`
  runtime device.
- Added the monochrome `F1EE` planar identity used by the 700. The preserved
  `39G9996` image identifies the 700C (`F1EC`), so the emulator converts the six
  planar-ID operands in its loaded copy and repairs the affected 64 KiB bank
  checksums. The user's ROM file is never modified.
- Added the machine-specific ROM shadow behavior and the correct 128 KiB high
  ROM window at `FE0000h–FFFFFFh`, while retaining the full low shadow used by
  POST.
- Implemented the system-control, VPD/serial EEPROM, planar and storage status
  registers queried by firmware and the Reference Diskette.

### IBM 486SLC and memory behavior

- Added IBM 486SLC/25 selection and ThinkPad-specific CPU configuration.
- Implemented the SLC cache-control registers and the 16 KiB, four-way cache
  tag behavior used by POST diagnostics.
- Added interpreter and dynamic-recompiler handling for the firmware's control
  register, stack, interrupt and return sequences.
- Corrected 4 MiB onboard RAM plus expansion topology, the 16 MiB presence
  state, reserved-memory mapping, NVRAM/EBDA values, DMA invalidation and warm
  reset behavior.

### MCA ESDI, system partition and boot

- Extended the MCA ESDI controller with the command, geometry, status and
  power-management behavior required by the 9552 firmware.
- Added support for the 370-cylinder, 64-head, 32-sector geometry used by the
  tested image.
- Implemented the Reference Diskette to hard-disk hand-off and documented the
  hidden 3 MiB system partition / IML requirement behind `I9990303`.
- Added `tools/thinkpad700/patch_tp700_media.py`, which validates a local work
  copy, restores only the reserved system-partition area when necessary, and
  refuses unexpected media layouts. It does not contain IBM data.

### Keyboard, mouse and floppy

- Corrected 8042 command-parameter ordering and IRQ12 timing required by the
  PS/2 pointing-device initialization.
- Added the ThinkPad POST mouse negotiation behavior and reliable mouse release
  handling.
- Implemented the PS/2 floppy-controller power/status responses used by the
  diagnostic and Reference Diskette paths.

### LCD and host integration

- Added a fixed 640×480 LCD presentation for the ThinkPad 700 path.
- Centered 720×400 and 640×400 firmware/text modes without resizing the host
  window, with safe horizontal margins for POST, PC DOS and Reference Diskette
  text.
- Added nearest-neighbor grayscale rendering, brightness/contrast/inversion
  hooks, fullscreen handling, and a lightweight SDL software-texture path for
  ARM64 hosts without useful GL acceleration.
- Added optional JSON status output and the companion panel bridge under
  `tools/thinkpad700/panel/` for a physical ThinkPad-style LED/control panel.

### Verified firmware results

The local test system completed POST, entered the Reference Diskette, performed
automatic configuration, handed off to the hard disk and entered Windows. The
implementation fixes the device/configuration causes of the observed `00160`,
`00164`, `00169`, `00201`, `12902`, `08611`, `00191` and `I9990303` reports;
it does not hide their strings or bypass the corresponding firmware tests.

See [the detailed development record](docs/DEVELOPMENT-NOTES.zh-CN.md) for the
original verification notes and [the upstream README](docs/UPSTREAM_README.md)
for general 86Box information.

## Repository layout

```text
src/                         86Box source plus ThinkPad 700 changes
cmake/, CMakeLists.txt       upstream build system
configs/thinkpad700/         redistributable configuration template
docs/                        technical notes and selected screenshots
scripts/                     local launch helpers
tools/thinkpad700/           deterministic media and panel tools
local-assets/                ignored local ROMs/media/runtime data
build/                       ignored build output
```

`local-assets/` is deliberately excluded by `.gitignore`. On the original
development machine it also holds historical research, locally owned media and
the migrated runtime. None of it should be added with `git add -f` unless its
license and redistribution rights have been checked independently.

## Required user-supplied files

At minimum, machine startup requires a legally obtained 256 KiB firmware dump
at this ROM-relative path:

```text
roms/machines/ibmtp700/39G9996.bin
```

The filename and location matter because 86Box ROM paths are hardcoded. You
will also need your own bootable floppy or disk image. For the fully verified
Reference Diskette workflow, provide your own Reference Diskette image and a
compatible ESDI hard-disk image containing the required IBM system partition.

Do not run the media helper on your only copy. Work on copies and keep known
hashes/backups of the originals.

## Build on Linux

The following Ubuntu/Debian packages match the official 86Box build guide:

```bash
sudo apt update
sudo apt install build-essential cmake extra-cmake-modules pkg-config \
  ninja-build libfreetype-dev libsdl2-dev libpng-dev libopenal-dev \
  librtmidi-dev libfluidsynth-dev libsndfile1-dev libserialport-dev \
  qtbase5-dev qtbase5-private-dev qttools5-dev libevdev-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libslirp-dev
```

Configure and build a normal x86-64 Linux release:

```bash
git clone https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C.git
cd 86Box-Thinkpad700_700C
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/flags-gcc-x86_64.cmake
cmake --build build/linux -j"$(nproc)"
```

For an ARM64 Linux host, enable the new recompiler and use the included ARM64
toolchain preset:

```bash
cmake --preset flags-gcc-aarch64-regular
cmake --build out/build/flags-gcc-aarch64-regular -j"$(nproc)"
```

The executable is normally under the selected build directory's `src/`
subdirectory. If the preset is unsuitable for your distribution, configure a
plain Ninja directory and add `-DNEW_DYNAREC=ON`.

## Build on Windows

The most reproducible native Windows route is an **MSYS2 UCRT64 or MINGW64**
shell. Update MSYS2, then install the dependencies listed by the official 86Box
build guide:

```bash
pacman -Syu
pacman -S --needed git \
  $MINGW_PACKAGE_PREFIX-ninja \
  $MINGW_PACKAGE_PREFIX-cmake \
  $MINGW_PACKAGE_PREFIX-gcc \
  $MINGW_PACKAGE_PREFIX-pkgconf \
  $MINGW_PACKAGE_PREFIX-openal \
  $MINGW_PACKAGE_PREFIX-freetype \
  $MINGW_PACKAGE_PREFIX-SDL2 \
  $MINGW_PACKAGE_PREFIX-zlib \
  $MINGW_PACKAGE_PREFIX-libpng \
  $MINGW_PACKAGE_PREFIX-rtmidi \
  $MINGW_PACKAGE_PREFIX-fluidsynth \
  $MINGW_PACKAGE_PREFIX-libslirp \
  $MINGW_PACKAGE_PREFIX-libsndfile \
  $MINGW_PACKAGE_PREFIX-qt5-static \
  $MINGW_PACKAGE_PREFIX-qt5-translations \
  $MINGW_PACKAGE_PREFIX-vulkan-headers
```

Build from the same MSYS2 shell:

```bash
git clone https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C.git
cd 86Box-Thinkpad700_700C
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows -j"$(nproc)"
```

Locate `86Box.exe` below `build/windows/`. Keep the DLLs produced or installed
by the MSYS2 build beside the executable when copying it to another machine.
Windows on ARM requires the upstream LLVM/vcpkg ARM64 toolchain and Windows 11;
that path is separate from the MSYS2 x86-64 build above.

## Configure and run a VM

You can import [`configs/thinkpad700/86box.cfg.example`](configs/thinkpad700/86box.cfg.example)
or create the VM in the UI with these settings:

| Setting | Value |
| --- | --- |
| Machine | `[MCA] IBM ThinkPad 700 (9552)` |
| CPU | IBM 486SLC/25 |
| Memory | 4 MiB initially; 16 MiB is the tested maximum |
| Video | Internal |
| Mouse | PS/2 |
| Storage controller | Integrated MCA ESDI |
| Tested disk geometry | 370 cylinders, 64 heads, 32 sectors |
| Floppy | 3.5-inch 1.44 MB |

Place the ROM tree either under the VM path or pass it explicitly:

```bash
./build/linux/src/86Box -R /path/to/rom-root /path/to/vm/86box.cfg
```

On Windows PowerShell the equivalent shape is:

```powershell
& .\build\windows\src\86Box.exe -R C:\path\to\rom-root C:\path\to\vm\86box.cfg
```

The exact executable subdirectory can vary with the generator. The `rom-root`
argument must contain `roms/machines/ibmtp700/39G9996.bin`.

## Original development-machine launcher

The migrated Linux workstation can still start the verified VM with:

```bash
./scripts/launch-thinkpad700.sh
```

That launcher expects the ignored historical layout under
`local-assets/legacy-work/`, validates/repairs only the local working copies,
sets the ROM path and starts the existing ARM64 build. Override the local data
location with `TP700_WORK_ROOT=/another/path` if needed.

The optional media tool can be run separately:

```bash
TP700_WORK_ROOT="$PWD/local-assets/legacy-work" \
  python3 tools/thinkpad700/patch_tp700_media.py
```

It intentionally fails on unexpected sizes, bytes, checksums or FAT layouts.

## Controls used by the verified Linux build

- Click the guest display to capture the mouse.
- `Ctrl+End` releases the mouse; the middle button is a fallback.
- `Ctrl+Alt+Page Up` toggles fullscreen in the SDL frontend.
- Exit fullscreen before using the host window close button.

## Development workflow

Keep all new source work in this repository. Generated builds, ROMs, VM state,
disk images, logs and capture sequences belong in ignored directories. Before
opening a pull request:

```bash
git diff --check
cmake --build <your-build-directory> -j2
```

Please describe which firmware path or hardware behavior a change implements,
and include reproducible logs or compact selected screenshots rather than raw
frame sequences.

## License and attribution

This fork retains the 86Box GNU GPL v2-or-later license and upstream notices;
see [`COPYING`](COPYING) and [`AUTHORS`](AUTHORS). New contributions to this
repository are distributed under the same license unless explicitly stated
otherwise in the relevant file.

Useful upstream references:

- [86Box project](https://github.com/86Box/86Box)
- [Official build guide](https://86box.readthedocs.io/en/stable/dev/buildguide.html)
- [ROM search paths](https://86box.readthedocs.io/en/latest/usage/roms.html)
