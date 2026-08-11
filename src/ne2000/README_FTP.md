# PicoMEM FTP server

Remote access to the PicoMEM microSD card (and USB drive) over WiFi, so files
can be copied to the card without pulling it out of the machine.

**PicoMEM 2 only** — see "Why PicoMEM 2 only" at the end.

## Setting it up

Add the network settings to `Wifi.txt` on the root of the microSD, after the two
existing lines. Files that only have the SSID and password keep working as they
are, the new lines are optional.

```
MyNetworkSSID
MyWiFiPassword
IP=192.168.1.50
MASK=255.255.255.0
GW=192.168.1.1
FTPUSER=picomem
FTPPASS=secret
```

| Key | Meaning |
| --- | --- |
| `IP` | Static address of the PicoMEM itself, or `DHCP`. No IP line means the stack (and FTP) stays off. |
| `MASK` | Subnet mask, defaults to `255.255.255.0` |
| `GW` | Default gateway, only needed to reach the card from another subnet |
| `FTP` | `OFF` disables the server while keeping the IP stack |
| `FTPUSER` | Login. If absent, any user/password is accepted |
| `FTPPASS` | Password |

Then connect with any client:

```
ftp 192.168.1.50
```

Windows Explorer (`ftp://192.168.1.50`), FileZilla, curl and the `ftp` command
all work. **Use passive mode** — active mode (`PORT`) is not supported.

## What you see

```
/sd     the microSD card   (FatFs volume 0:)
/usb    the USB drive      (FatFs volume 1:, hidden when nothing is plugged in)
```

So `/sd/HDD/` holds the hard disk images and `/sd/FLOPPY/` the floppy images.

Supported: `LIST` `NLST` `RETR` `STOR` `APPE` `DELE` `MKD` `RMD` `RNFR`/`RNTO`
`CWD` `CDUP` `PWD` `SIZE` `MDTM` `TYPE` `PASV` `ABOR`.

One client at a time.

## Things to know

**Do not overwrite or delete a disk image that is currently mounted.** The
firmware keeps a cluster map (`clmt`) of every mounted `.img`/`.vhd` for fast
seeking, and DOS caches FAT and directory sectors in PC RAM. Rewriting the image
underneath it corrupts the filesystem inside it. Uploading a *new* image, or
replacing one that is not selected in the BIOS menu, is fine. Select `None` for
the drive first if you want to replace an image in place, then reboot the PC.

**Transfers share the card with DOS.** The FTP file access runs in the same
Core 0 loop as the Int13h disk emulation, so the two take turns. A big FTP
transfer while DOS is hammering the disk will slow both down, but neither
corrupts the other.

**No encryption.** FTP sends the password and the files in clear text, and there
is no TLS here. Keep it on a home LAN, do not forward a port to it.

**Timestamps.** FatFs is built with `FF_FS_NORTC`, so uploaded files get a fixed
date rather than the real one.

## How it works

The PicoMEM normally acts as a transparent layer 2 bridge: frames from the
emulated NE2000 go straight out through the cyw43 chip and the PC's TCP/IP stack
(mTCP...) owns the only IP address. The card itself has no IP.

`pm_net.c` adds a second logical host behind the *same* MAC address, with its
own IP. That is just an IP alias as far as the access point is concerned. The
frame dispatcher in `ne2000.c` routes each incoming frame:

- ARP to both stacks, each answers only for its own address
- IPv4 addressed to the Pico to lwIP only, which also keeps the 8088 from having
  to discard every FTP data packet
- everything else to the emulated NE2000, unchanged

### Threading

This is the part to be careful with. FatFs is built with `FF_FS_REENTRANT 0`, so
it must never be entered from two contexts at once.

- `cyw43_cb_process_ethernet()` runs in the cyw43 background IRQ. It only
  *copies* Pico-bound frames into a ring buffer.
- `pm_net_poll()`, called from the Core 0 main loop in `pm_cmd.cpp`, is the only
  place lwIP is ever entered.

Every lwIP callback — and therefore every FatFs call the FTP server makes — runs
in the Core 0 thread context, the same one that services the PC Int13h commands.
They interleave between loop iterations instead of preempting each other.

For the same reason the build links plain `pico_lwip` rather than
`pico_cyw43_arch_lwip_*` (which would both clash with the NE2000 bridge over the
`cyw43_cb_process_ethernet` symbol and run lwIP from the IRQ), and calls
`lwip_init()` instead of `lwip_nosys_init()`.

Work is bounded per poll — 6 frames, one 4KB file chunk, 12 directory entries —
so a busy transfer cannot add latency to a PC disk access.

### Why PicoMEM 2 only

On the PicoMEM 1.x the microSD shares its SPI bus with the PSRAM. `PM_EnableSD()`
takes the bus away from the PSRAM PIO, which is only safe while the PC is parked
inside a BIOS call. An FTP transfer arrives whenever the network feels like it,
so it could take the bus while the PC is executing from PSRAM backed RAM and
crash the machine.

The PicoMEM 2 puts the microSD on its own SPI bus (`pm20.h`), so there is no
conflict. `USE_PM_NET` in `src/CMakeLists.txt` is forced to 0 on every other
board; enabling it there needs bus arbitration to be written first.
