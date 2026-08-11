// pm_net.c : PicoMEM on-board TCP/IP stack (lwIP), see pm_net.h for the design.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pm_net.h"
#include "../pm_debug.h"

pm_net_cfg_t PM_NetCfg = {
    .use_dhcp    = false,
    .configured  = false,
    .ip          = {0, 0, 0, 0},
    .mask        = {255, 255, 255, 0},
    .gw          = {0, 0, 0, 0},
    .ftp_enabled = true,
    .ftp_user    = {0},
    .ftp_pass    = {0},
};

// ---------------------------------------------------------------------------
// Configuration parsing : shared by both builds so Wifi.txt always parses.
// ---------------------------------------------------------------------------

// Parse "a.b.c.d" into 4 bytes. Returns false when the string is not an IPv4.
static bool parse_ipv4(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if ((a > 255) || (b > 255) || (c > 255) || (d > 255)) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

static void str_upper(char *s)
{
    while (*s) { if ((*s >= 'a') && (*s <= 'z')) *s -= ('a' - 'A'); s++; }
}

// Remove the leading and trailing spaces/tabs of a string, in place.
static char *str_trim(char *s)
{
    char *end;
    while ((*s == ' ') || (*s == '\t')) s++;
    end = s + strlen(s);
    while ((end > s) && ((end[-1] == ' ') || (end[-1] == '\t'))) end--;
    *end = 0;
    return s;
}

void pm_net_parse_cfgline(char *line)
{
    char *key, *val, *eq;

    if (line == NULL) return;
    // Skip the comment and empty lines
    key = str_trim(line);
    if ((*key == 0) || (*key == '#') || (*key == ';')) return;

    eq = strchr(key, '=');
    if (eq == NULL) return;    // Not a KEY=VALUE line, ignore it
    *eq = 0;
    val = str_trim(eq + 1);
    key = str_trim(key);
    str_upper(key);

    if (strcmp(key, "IP") == 0) {
        char upval[8];
        strncpy(upval, val, sizeof(upval) - 1);
        upval[sizeof(upval) - 1] = 0;
        str_upper(upval);
        if (strcmp(upval, "DHCP") == 0) {
            PM_NetCfg.use_dhcp   = true;
            PM_NetCfg.configured = true;
        } else if (parse_ipv4(val, PM_NetCfg.ip)) {
            PM_NetCfg.use_dhcp   = false;
            PM_NetCfg.configured = true;
        }
    } else if (strcmp(key, "MASK") == 0) {
        parse_ipv4(val, PM_NetCfg.mask);
    } else if (strcmp(key, "GW") == 0) {
        parse_ipv4(val, PM_NetCfg.gw);
    } else if (strcmp(key, "FTP") == 0) {
        char upval[8];
        strncpy(upval, val, sizeof(upval) - 1);
        upval[sizeof(upval) - 1] = 0;
        str_upper(upval);
        PM_NetCfg.ftp_enabled = ((strcmp(upval, "OFF") != 0) && (strcmp(upval, "0") != 0));
    } else if (strcmp(key, "FTPUSER") == 0) {
        strncpy(PM_NetCfg.ftp_user, val, sizeof(PM_NetCfg.ftp_user) - 1);
        PM_NetCfg.ftp_user[sizeof(PM_NetCfg.ftp_user) - 1] = 0;
    } else if (strcmp(key, "FTPPASS") == 0) {
        strncpy(PM_NetCfg.ftp_pass, val, sizeof(PM_NetCfg.ftp_pass) - 1);
        PM_NetCfg.ftp_pass[sizeof(PM_NetCfg.ftp_pass) - 1] = 0;
    }
}

#if USE_PM_NET

#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "hardware/sync.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "pm_ftp.h"

// Present in cyw43.h on recent SDKs, kept here so the build does not depend on it
#ifndef CYW43_LINK_JOIN
#define CYW43_LINK_JOIN  (1)
#endif

// ---------------------------------------------------------------------------
// Receive ring : written by the cyw43 IRQ, drained by the Core 0 main loop.
// Fixed size slots keep the producer side trivial (and IRQ safe).
// ---------------------------------------------------------------------------

#define PM_NET_RX_SLOTS   10
#define PM_NET_RX_SLOTSZ  1536
#define PM_NET_TX_BUFSZ   1600

// Maximum frames handed to lwIP per pm_net_poll() call, so a burst of packets
// can never delay the PC Int13h service for too long.
#define PM_NET_RX_BURST   6

static uint8_t  rx_slot[PM_NET_RX_SLOTS][PM_NET_RX_SLOTSZ];
static uint16_t rx_slot_len[PM_NET_RX_SLOTS];
static volatile uint8_t rx_head;   // Written by the IRQ
static volatile uint8_t rx_tail;   // Written by the main loop
static volatile uint32_t rx_dropped;

static struct netif pm_netif;
static bool  net_started = false;
static bool  link_is_up  = false;
static bool  dhcp_running = false;
static char  ip_str[16] = "0.0.0.0";

bool pm_net_started(void) { return net_started; }

const char *pm_net_ip_str(void) { return ip_str; }

static void update_ip_str(void)
{
    const ip4_addr_t *a = netif_ip4_addr(&pm_netif);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
             (unsigned)ip4_addr1(a), (unsigned)ip4_addr2(a),
             (unsigned)ip4_addr3(a), (unsigned)ip4_addr4(a));
}

// --- Receive side (IRQ context) --------------------------------------------

bool pm_net_rx_push(const uint8_t *buf, uint16_t len)
{
    uint8_t head, next;

    if (!net_started) return false;
    if ((len == 0) || (len > PM_NET_RX_SLOTSZ)) return false;

    head = rx_head;
    next = (uint8_t)((head + 1) % PM_NET_RX_SLOTS);
    if (next == rx_tail) {       // Ring full : drop the frame, TCP will resend
        rx_dropped++;
        return false;
    }
    memcpy(rx_slot[head], buf, len);
    rx_slot_len[head] = len;
    __dmb();                     // Publish the data before the index
    rx_head = next;
    return true;
}

// --- Transmit side (Core 0 thread context) ---------------------------------

static err_t pm_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
    static uint8_t txbuf[PM_NET_TX_BUFSZ];
    async_context_t *ctx;

    (void)netif;
    if (p->tot_len > sizeof(txbuf)) return ERR_MEM;
    pbuf_copy_partial(p, txbuf, p->tot_len, 0);

    // Short lock : the cyw43 driver is also used by the background IRQ that
    // receives the frames. Never hold this across a FatFs access.
    ctx = cyw43_arch_async_context();
    if (ctx) async_context_acquire_lock_blocking(ctx);
    cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, p->tot_len, txbuf, false);
    if (ctx) async_context_release_lock(ctx);

    return ERR_OK;
}

static err_t pm_netif_init_cb(struct netif *netif)
{
    netif->name[0]    = 'p';
    netif->name[1]    = 'm';
    netif->output     = etharp_output;
    netif->linkoutput = pm_netif_linkoutput;
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->hwaddr_len = ETH_HWADDR_LEN;
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(netif, PM_NET_HOSTNAME);
#endif
    return ERR_OK;
}

// ---------------------------------------------------------------------------

bool pm_net_init(const uint8_t *mac)
{
    ip4_addr_t ip, mask, gw;

    if (net_started) return true;

    if (!PM_NetCfg.configured) {
        PM_INFO("[NET] No IP in Wifi.txt, on board TCP/IP stack disabled\n");
        return false;
    }

    // lwip_init() directly, not lwip_nosys_init() : the SDK helper schedules
    // sys_check_timeouts() on the async_context, which would run lwIP from the
    // background IRQ and break the single context rule described in pm_net.h.
    lwip_init();

    if (PM_NetCfg.use_dhcp) {
        ip4_addr_set_zero(&ip);
        ip4_addr_set_zero(&mask);
        ip4_addr_set_zero(&gw);
    } else {
        IP4_ADDR(&ip,   PM_NetCfg.ip[0],   PM_NetCfg.ip[1],   PM_NetCfg.ip[2],   PM_NetCfg.ip[3]);
        IP4_ADDR(&mask, PM_NetCfg.mask[0], PM_NetCfg.mask[1], PM_NetCfg.mask[2], PM_NetCfg.mask[3]);
        IP4_ADDR(&gw,   PM_NetCfg.gw[0],   PM_NetCfg.gw[1],   PM_NetCfg.gw[2],   PM_NetCfg.gw[3]);
    }

    if (netif_add(&pm_netif, &ip, &mask, &gw, NULL,
                  pm_netif_init_cb, ethernet_input) == NULL) {
        PM_ERROR("[NET] netif_add failed\n");
        return false;
    }
    // The MAC is shared with the emulated NE2000 : the AP only ever sees one
    // station, the two IP addresses are just aliases behind it.
    memcpy(pm_netif.hwaddr, mac, ETH_HWADDR_LEN);

    netif_set_default(&pm_netif);
    netif_set_up(&pm_netif);

    rx_head = rx_tail = 0;
    rx_dropped = 0;
    net_started = true;
    update_ip_str();

    // PM_ConnectWifi() starts the association before calling us, so the cyw43
    // link up callback may already have fired. Pick the current state up here,
    // otherwise the netif would stay link down forever and never transmit.
    if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) >= CYW43_LINK_JOIN)
        pm_net_set_link(true);

    if (PM_NetCfg.use_dhcp) {
        PM_INFO("[NET] Started, waiting for DHCP\n");
    } else {
        PM_INFO("[NET] Started, IP %s\n", ip_str);
    }

    if (PM_NetCfg.ftp_enabled) pm_ftp_init();

    return true;
}

void pm_net_set_link(bool up)
{
    if (!net_started) return;
    if (up == link_is_up) return;
    link_is_up = up;

    if (up) {
        netif_set_link_up(&pm_netif);
        if (PM_NetCfg.use_dhcp && !dhcp_running) {
            // Note : the PC uses the same MAC address, so a DHCP server may
            // hand out the very same lease to both. A static IP is safer when
            // DOS networking is used at the same time.
            if (dhcp_start(&pm_netif) == ERR_OK) dhcp_running = true;
        }
    } else {
        netif_set_link_down(&pm_netif);
    }
}

static uint32_t ip_be_to_host(const uint8_t ip_be[4])
{
    return ((uint32_t)ip_be[0] << 24) | ((uint32_t)ip_be[1] << 16) |
           ((uint32_t)ip_be[2] << 8)  |  (uint32_t)ip_be[3];
}

bool pm_net_is_our_unicast(const uint8_t ip_be[4])
{
    uint32_t mine;

    if (!net_started) return false;
    mine = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&pm_netif)));
    if (mine == 0) return false;              // No address yet (DHCP pending)
    return (ip_be_to_host(ip_be) == mine);
}

bool pm_net_wants_ip(const uint8_t ip_be[4])
{
    uint32_t dst, mine, mask;

    if (!net_started) return false;

    dst = ip_be_to_host(ip_be);
    if (dst == 0xFFFFFFFFu) return true;      // Limited broadcast

    mine = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&pm_netif)));
    if (mine == 0) return false;
    if (dst == mine) return true;

    mask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(&pm_netif)));
    if (mask && ((dst & mask) == (mine & mask)) && ((dst | mask) == dst))
        return true;                          // Directed broadcast (x.x.x.255)

    return false;
}

void pm_net_poll(void)
{
    uint8_t n;

    if (!net_started) return;

    for (n = 0; n < PM_NET_RX_BURST; n++) {
        struct pbuf *p;
        uint8_t tail = rx_tail;
        uint16_t len;

        if (tail == rx_head) break;           // Ring empty
        __dmb();
        len = rx_slot_len[tail];

        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (p != NULL) {
            if (pbuf_take(p, rx_slot[tail], len) != ERR_OK) {
                pbuf_free(p);
            } else if (pm_netif.input(p, &pm_netif) != ERR_OK) {
                pbuf_free(p);
            }
        } else {
            rx_dropped++;                     // Out of pbufs, drop it
        }
        rx_tail = (uint8_t)((tail + 1) % PM_NET_RX_SLOTS);
    }

    sys_check_timeouts();

    if (PM_NetCfg.use_dhcp) {
        // Refresh the printed address once DHCP has bound
        const ip4_addr_t *a = netif_ip4_addr(&pm_netif);
        if (!ip4_addr_isany_val(*a)) {
            static bool announced = false;
            if (!announced) {
                announced = true;
                update_ip_str();
                PM_INFO("[NET] DHCP address %s\n", ip_str);
            }
        }
    }

    if (PM_NetCfg.ftp_enabled) pm_ftp_poll();
}

#else  // !USE_PM_NET : stubs so ne2000.c can call unconditionally

bool pm_net_init(const uint8_t *mac)                 { (void)mac; return false; }
void pm_net_poll(void)                               { }
void pm_net_set_link(bool up)                        { (void)up; }
bool pm_net_rx_push(const uint8_t *b, uint16_t l)    { (void)b; (void)l; return false; }
bool pm_net_wants_ip(const uint8_t ip_be[4])         { (void)ip_be; return false; }
bool pm_net_is_our_unicast(const uint8_t ip_be[4])   { (void)ip_be; return false; }
bool pm_net_started(void)                            { return false; }
const char *pm_net_ip_str(void)                      { return "0.0.0.0"; }

#endif // USE_PM_NET
