# Building the PicoMEM firmware on Linux

The Pico SDK is intentionally **not** part of this repository — that is normal,
it is referenced through `PICO_SDK_PATH` (or `~/.pico-sdk`, which the Raspberry
Pi Pico VS Code extension populates and `src/CMakeLists.txt` picks up
automatically). `src/CMakeLists.txt` pins `sdkVersion 2.2.0`.

Three things do get in the way of a clean Linux build, all of them independent
of the SDK. They are described below with the workaround used by
`tools/build-linux.sh`.

## 1. Toolchain

```
sudo apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi \
                        libstdc++-arm-none-eabi-newlib cmake ninja-build
```

`gcc-arm-none-eabi` on its own is not enough — without `libnewlib-arm-none-eabi`
the build stops at `cannot read spec file 'nosys.specs'`.

`picotool` (needed to produce the `.uf2`) does not have to be installed, the SDK
downloads and builds it automatically during configuration.

## 2. Pico SDK

```
git clone --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init lib/lwip lib/cyw43-driver
```

`lib/lwip` is required — the FTP server links `pico_lwip`. `lib/cyw43-driver` is
required for the WiFi/NE2000 code. `lib/btstack` and `lib/mbedtls` are not used
and can be left uninitialised.

## 3. Submodules that are declared but not registered

`.gitmodules` lists four submodules, but only one of them has an actual gitlink
in the index:

```
$ git ls-files -s | awk '$1=="160000"'
160000 7786c93... 0	src/lib/rp2040-psram
```

So `git submodule update --init` fetches `rp2040-psram` and silently does
nothing for the other three. `src/lib/tinyusb` and `src/lib/tusb_xinput` have to
be cloned by hand:

```
git submodule update --init src/lib/rp2040-psram
git clone https://github.com/Ryzee119/tusb_xinput.git src/lib/tusb_xinput
git clone https://github.com/hathach/tinyusb.git src/lib/tinyusb
```

**Which TinyUSB revision?** The gitlink is missing so nothing is recorded, but
the range can be narrowed from the code:

`src/USB_Host_driver/tusb_config.h` sets `CFG_TUH_MIDI 1`. Under that guard
`usbh.c` uses `AUDIO_SUBCLASS_CONTROL`, and the only thing that brings that
definition in is `tusb.h` -> `class/midi/midi_host.h` -> `class/audio/audio.h`.
`midi_host.h` (native MIDI host support) **first exists in 0.19.0**, so anything
older fails with:

```
usbh.c: error: 'AUDIO_SUBCLASS_CONTROL' undeclared
```

That gives a hard lower bound. Verified by building the full firmware:

| TinyUSB | Result |
| ------- | ------ |
| <= 0.18.0 | does not compile, no `midi_host.h` |
| 0.19.0  | builds, but `xinput_host.c` warns: incompatible pointer type |
| 0.20.0  | builds, same warning |
| **0.21.0** | **builds clean** |

The warning on 0.19/0.20 is not cosmetic. TinyUSB changed
`usbh_class_driver_t::open()` from returning `bool` to returning a `uint16_t`
descriptor length; `tusb_xinput` at current master uses the new signature, so on
0.19/0.20 a `true` return is read as "driver length 1" and xinput controller
enumeration would misbehave at runtime.

So **0.21.0 is the recommendation**, and it is also the only tag consistent with
current `tusb_xinput`. Note that `tusb_xinput` has no recorded revision either,
so if upstream pinned an older one, 0.19.0/0.20.0 could be what they use.

Both should be registered properly:

```
git submodule add https://github.com/hathach/tinyusb src/lib/tinyusb
git submodule add https://github.com/Ryzee119/tusb_xinput src/lib/tusb_xinput
```

`WonderMCA` is only needed for `PICOMEM_BOARD "WONDERMCA_10"`.

### The xinput link step

`src/USB_Host_Driver2/CMakeLists.txt` opens with a note to self:

```
# !! Remove link of tinyusb in xinput CMakefile !!
```

That refers to `target_link_libraries(xinput_host INTERFACE tinyusb)` in
`src/lib/tusb_xinput/CMakeLists.txt`. It has to go, otherwise the link ends with
`cannot find -ltinyusb` — the TinyUSB sources are already compiled straight into
the `picomem` target. `tools/build-linux.sh` does this automatically in the
staged copy. A `git submodule add` of tusb_xinput would make this a permanent
local edit, so it may be worth carrying a patched copy or upstreaming the change.

## 4. Directory name case

There are two directories differing only in case:

```
src/USB_Host_Driver/     msc/  (capital D)  <- the name CMake references
src/USB_Host_driver/     usb.c, tusb_config.h, hid_app.c, ...  (lowercase d)
```

On Windows and macOS the filesystem is case-insensitive, so these are one
directory holding all the files and the build works. On Linux they stay separate
and the build fails at configure time:

```
CMake Error at CMakeLists.txt:622 (target_link_libraries):
  Cannot find source file: src/USB_Host_Driver/usb.c
```

`src/USB_Host_Driver2/CMakeLists.txt` refers to `../USB_Host_Driver/usb.c` and
`CFG_TUSB_CONFIG_FILE="../../../USB_Host_Driver/tusb_config.h"`, both of which
only exist in the lowercase directory. Consolidating the two directories under
one name would fix Linux builds for good.

## Building

```
export PICO_SDK_PATH=/path/to/pico-sdk
./tools/build-linux.sh
```

The script copies `src/` to a temporary directory, merges the two case-variant
USB directories there, drops the xinput tinyusb link, configures and builds. It
never modifies the working tree. Output: `build/picomem.uf2`.

`--no-usb` builds with `DO_USBHOST 0`. It additionally has to stub out
`pm_libs/inputs/pm_inputs.c` and `inputs/players/player.c` in the staged copy:
`pm_libs` compiles them unconditionally and both include `tusb.h` through
`inputs/manager.h`, so with the USB host off the TinyUSB include paths are not
set and they cannot build. Guarding those two sources with `if (DO_USBHOST)` in
`src/pm_libs/CMakeLists.txt` would make `DO_USBHOST 0` a supported
configuration and let the flag drop the stubbing.

## Measured footprint (PICOMEM_20, TinyUSB 0.21.0)

| Configuration                        | FLASH   | RAM (of 512 KB) |
| ------------------------------------ | ------- | --------------- |
| `DO_USBHOST 0`, `USE_PM_NET 0`       | 4650420 | 353248 (67.4%)  |
| `DO_USBHOST 0`, `USE_PM_NET 1`       | 4702764 | 420204 (80.2%)  |
| `DO_USBHOST 1`, `USE_PM_NET 0`       | 4725084 | 376400 (71.8%)  |
| **`DO_USBHOST 1`, `USE_PM_NET 1`**   | 4777428 | **443356 (84.6%)** |

The FTP server costs a flat **65 KB of RAM** (its buffers are static, so the
figure does not change with the USB host on or off) and about 51 KB of flash.
Everything enabled leaves roughly 79 KB of RAM free.

See `src/ne2000/README_FTP.md` for what makes up the 65 KB and which knobs to
turn should a future feature need that headroom back.

## PicoMEM 1.x (RP2040, `PICOMEM_BOARD "PICOMEM"`, `pico_w`)

The PicoMEM 2 source dropped two settings a 1.x needs, and a build without
them never lights the LED and hangs the PC after the option ROM banner:
`CYW43_PIO_CLOCK_DIV_INT=4` (it was in the `pico_w` block; it is back in the
"< 1.5" block) and 1.20V at 280MHz on an RP2040 (`BOARD_Init` set the
RP2350's 1.15V). Both are restored on `pm1x-sb-fixes` and `os8088-pm114`.

Build a 1.x the way the 2025 firmwares were built, not with the 0.21.0
TinyUSB recommended above: Pico SDK **2.1.1**, TinyUSB from
`https://github.com/wbcbz7/tinyusb` branch `rp2040-epx-only` at **a88dc2429**
(2025-11-13; the RP2040 host-mode fixes plus `midi_host.h`), and tusb_xinput at
**cfd83ba** (the `bool open()` signature that TinyUSB revision has).

A 1.x moving from the November 2025 firmware to this source should have its
`PICOMEM.CFG` deleted and the settings entered again: the old file is not read
correctly, and the NE2000 is the visible casualty (no card at its port).

### What `os8088-pm114` is: the PR branch, plus this build and a knob

`os8088-pm114` is `pm1x-sb-fixes` - the fixes as they would go upstream, one
commit on FreddyVRetro/ISA-PicoMEM `main` - with two commits on top: this
1.14 build setup (`CMakeLists.txt`'s board, this file, `tools/build-linux.sh`,
the soundfont stand-in) and the field diagnostics. When the fixes change, they
change on `pm1x-sb-fixes` and this branch is rebased onto it, so the two never
drift. os8088 `docs/FIELD-NOTES.md` 66 is the field record behind each fix:

- **A DMA cycle no longer decodes as an 8237 write** (`pm_isa_rp2040.h`).
- **The Sound Blaster is fed through a disk call**: the SB worker runs in the
  disk loop (`bios_disk.cpp`), in every PC-command wait for the length of an
  int 13h (`pm_pccmd_wait_hook`), and the mixer in the SD driver's waits.
- **512-byte DMA copies and a 1,024-sample FIFO start** for the SB, and a
  sound shorter than the start level plays once its DMA has stopped.
- **The block IRQ is sent once the block has been read**, not when its time
  is up, and **the rest of the last copy is dropped** when the PC programs
  the DMA again. Either one wrong is crackles for a whole play.

`OS88_DIAG` (CMake cache variable, default 0) builds in what measured them:
the DMA-port write log at BIOS+5C00h, the Sound Blaster's audio health
record at BIOS+5F80h ('AH08'), the silence-by-site counters, and the two
PC-side switches os8088's `tests/pmdma` flips - BIOS+5C09h (no SB worker in
the disk loop) and BIOS+5F85h (the stock 128-byte copies, 512-sample start
and no waits pumped). With it off none of that RAM is read or written and
the fixes cannot be switched off. `tools/build-linux.sh` passes
`CMAKE_EXTRA` through to the configure step:

    CMAKE_EXTRA=-DOS88_DIAG=1 tools/build-linux.sh
