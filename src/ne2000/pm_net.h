// pm_net.h : PicoMEM on-board TCP/IP stack (lwIP)
//
// The PicoMEM normally acts as a transparent layer 2 bridge : the emulated
// NE2000 frames are sent/received "as is" through the cyw43 WiFi chip, and the
// PC (mTCP...) owns the only IP address of the board.
//
// pm_net adds a *second* logical host behind the same MAC address, so the Pico
// itself can answer IP traffic (used by the built in FTP server).
// The PC and the Pico share the cyw43 MAC, but use two different IP addresses,
// which is perfectly valid on the LAN side (an IP alias).
//
// Threading model, this is important :
//   - cyw43_cb_process_ethernet() runs in the cyw43 background IRQ context.
//     It only *copies* the frames addressed to the Pico into a small ring.
//   - pm_net_poll() is called from the Core 0 main loop and is the only place
//     lwIP is entered. Every lwIP callback (and so every FatFs access done by
//     the FTP server) therefore runs in the Core 0 thread context, like the
//     Int13h disk code, which keeps FatFs single threaded (FF_FS_REENTRANT=0).

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_NET_HOSTNAME "picomem"

// Initialise the lwIP netif bound to the cyw43 WiFi interface.
// Must be called after cyw43_arch_init() / the MAC address is known.
// Returns true if the stack has been started.
bool pm_net_init(const uint8_t *mac);

// Called from the Core 0 main loop : push the received frames into lwIP and
// run the lwIP timers. Bounded work per call so the Int13h latency is kept low.
void pm_net_poll(void);

// Called from the cyw43 link up/down callbacks.
void pm_net_set_link(bool up);

// Called from cyw43_cb_process_ethernet() (IRQ context) : copy one frame in the
// ring buffer that pm_net_poll() drains. Returns false if the ring is full.
bool pm_net_rx_push(const uint8_t *buf, uint16_t len);

// True when lwIP should see a frame sent to this IPv4 address (network byte
// order, as found in the IP header) : our own address, or a broadcast.
bool pm_net_wants_ip(const uint8_t ip_be[4]);

// True only when the address is exactly ours (never for a broadcast). Used to
// decide whether the frame can be kept away from the emulated NE2000.
bool pm_net_is_our_unicast(const uint8_t ip_be[4]);

// True once pm_net_init() succeeded.
bool pm_net_started(void);

// Current IPv4 address as a string ("0.0.0.0" when not configured yet).
const char *pm_net_ip_str(void);

// Parse one "KEY=VALUE" line of Wifi.txt (lines 3 and following).
// Unknown keys are ignored so the file stays forward compatible.
void pm_net_parse_cfgline(char *line);

// Configuration filled by pm_net_parse_cfgline(), read by pm_net/pm_ftp.
typedef struct pm_net_cfg_t {
    bool use_dhcp;        // IP=DHCP (default : off, static address)
    bool configured;      // true as soon as an IP or DHCP has been requested
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    bool ftp_enabled;     // FTP=OFF disables the server (default : on)
    char ftp_user[24];    // empty : anonymous access
    char ftp_pass[24];
} pm_net_cfg_t;

extern pm_net_cfg_t PM_NetCfg;

#ifdef __cplusplus
}
#endif
