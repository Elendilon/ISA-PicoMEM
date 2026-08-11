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

**The TinyUSB revision is unknown.** Because the gitlink is missing there is no
recorded commit, and neither 0.17.0 nor 0.18.0 compiles against the in-tree USB
host glue (`usbh.c: 'AUDIO_SUBCLASS_CONTROL' undeclared`). Whoever knows the
revision that upstream builds against should register it properly:

```
git submodule add https://github.com/hathach/tinyusb src/lib/tinyusb
```

`WonderMCA` is only needed for `PICOMEM_BOARD "WONDERMCA_10"`.

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
./tools/build-linux.sh --no-usb
```

The script copies `src/` to a temporary directory, merges the two case-variant
USB directories there, configures and builds. It never modifies the working
tree. Output: `build/picomem.uf2`.

`--no-usb` is required for now because of item 3. It also has to stub out
`pm_libs/inputs/pm_inputs.c` and `inputs/players/player.c` in the staged copy:
`pm_libs` compiles them unconditionally and both include `tusb.h` through
`inputs/manager.h`, so with the USB host off the TinyUSB include paths are not
set and they cannot build. Guarding those two sources with `if (DO_USBHOST)` in
`src/pm_libs/CMakeLists.txt` would make `DO_USBHOST 0` a supported
configuration and let the flag drop the stubbing.

Once the TinyUSB revision is pinned, drop `--no-usb` for a full build.

## Measured footprint (PICOMEM_20, `DO_USBHOST 0`)

|                       | FLASH   | RAM (of 512 KB) |
| --------------------- | ------- | --------------- |
| `USE_PM_NET 0`        | 4650420 | 353248 (67.4%)  |
| `USE_PM_NET 1`        | 4702764 | 420204 (80.2%)  |
| **FTP server cost**   | +51 KB  | **+65 KB**      |

See `src/ne2000/README_FTP.md` for what makes up the 65 KB and which knobs to
turn if a build with USB host enabled runs short of RAM.
