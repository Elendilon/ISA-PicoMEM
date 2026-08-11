// pm_ftp.h : Minimal FTP server running on the PicoMEM, on top of pm_net/lwIP.
//
// Gives remote (LAN) access to the microSD card and the USB drive of the board:
//   /sd   -> FatFs volume "0:" (microSD)
//   /usb  -> FatFs volume "1:" (USB mass storage, when enabled)
//
// Passive mode only (every modern client uses PASV), one client at a time.
// Everything runs in the Core 0 thread context, see the note in pm_net.h.
//
// WARNING : FTP is a clear text protocol and this server has no TLS. It is
// meant for a trusted home LAN, do not forward a port to it from the Internet.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start listening on port 21. Called by pm_net_init().
void pm_ftp_init(void);

// Called from pm_net_poll() : pushes the pending LIST/RETR data.
void pm_ftp_poll(void);

// True while a file transfer is in progress (can be shown by the BIOS/OLED).
bool pm_ftp_busy(void);

#ifdef __cplusplus
}
#endif
