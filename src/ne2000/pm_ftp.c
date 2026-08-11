// pm_ftp.c : Minimal FTP server for the PicoMEM. See pm_ftp.h.

#include "pm_ftp.h"

#if USE_PM_NET

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "lwip/tcp.h"
#include "lwip/netif.h"

#include "ff.h"
#include "f_util.h"

#include "pm_net.h"
#include "../pm_debug.h"

// Implemented in pm_boards/pmdev_pico.cpp : gives the SPI bus back to the
// microSD on the boards where it is shared with the PSRAM (PicoMEM 1.x).
// It is a no-op on the PicoMEM 2 where the microSD has its own SPI bus.
extern bool PM_EnableSD(void);

#define FTP_CTRL_PORT      21
#define FTP_PASV_PORT_MIN  2100
#define FTP_PASV_PORT_MAX  2115

#define FTP_CHUNK          4096   // Bytes moved per pm_ftp_poll() call
#define FTP_LIST_MAXLINES  12     // Directory entries emitted per poll
#define FTP_PATH_MAX       160
#define FTP_CMD_MAX        200

typedef enum {
    FTP_XFER_NONE = 0,
    FTP_XFER_LIST,
    FTP_XFER_RETR,
    FTP_XFER_STOR
} ftp_xfer_t;

typedef struct {
    struct tcp_pcb *ctrl;
    struct tcp_pcb *data;
    struct tcp_pcb *pasv;         // Listening pcb created by PASV

    bool  authed;
    bool  user_ok;

    char  cwd[FTP_PATH_MAX];      // Virtual path, always starts with '/'
    char  rnfr[FTP_PATH_MAX];     // Pending RNFR target (FatFs path)

    ftp_xfer_t xfer;
    bool  list_root;              // Listing the virtual root
    bool  list_nlst;              // NLST instead of LIST
    uint8_t list_stage;           // Virtual root entry being emitted

    FIL   fil;
    bool  fil_open;
    DIR   dir;
    bool  dir_open;

    char  cmd[FTP_CMD_MAX];
    uint16_t cmdlen;
} ftp_session_t;

static ftp_session_t s;
static struct tcp_pcb *ftp_listen_pcb = NULL;
static bool ftp_ready = false;
static uint16_t pasv_next_port = FTP_PASV_PORT_MIN;
static uint8_t xfer_buf[FTP_CHUNK];

static void ftp_data_close(void);

// A transfer command is accepted as soon as PASV has been issued : depending on
// the client, the data connection shows up either before or after the command.
static bool ftp_data_ready(void)
{
    return (s.data != NULL) || (s.pasv != NULL);
}

bool pm_ftp_busy(void)
{
    return (s.xfer == FTP_XFER_RETR) || (s.xfer == FTP_XFER_STOR);
}

// ---------------------------------------------------------------------------
// Control replies
// ---------------------------------------------------------------------------

static void ftp_reply(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int len;

    if (s.ctrl == NULL) return;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)(sizeof(buf) - 3)) len = (int)(sizeof(buf) - 3);
    buf[len++] = '\r';
    buf[len++] = '\n';

    if (tcp_sndbuf(s.ctrl) < len) return;   // Client is not reading, drop it
    tcp_write(s.ctrl, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    tcp_output(s.ctrl);
}

// ---------------------------------------------------------------------------
// Virtual path handling
//   "/"          -> virtual root, lists "sd" and "usb"
//   "/sd/..."    -> FatFs "0:/..."
//   "/usb/..."   -> FatFs "1:/..."
// ---------------------------------------------------------------------------

// Resolve "arg" against "base" (both virtual) into "out". Handles "." and "..".
static bool vpath_resolve(const char *base, const char *arg, char *out, size_t outsz)
{
    char tmp[FTP_PATH_MAX];
    const char *p;
    size_t len = 0;

    if (arg && (arg[0] == '/')) {
        tmp[0] = 0;                       // Absolute : start from the root
        p = arg + 1;
    } else {
        strncpy(tmp, base, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        if ((tmp[0] == '/') && (tmp[1] == 0)) tmp[0] = 0;   // Root as ""
        len = strlen(tmp);
        p = (arg != NULL) ? arg : "";
    }

    while (*p) {
        char comp[FTP_PATH_MAX];
        size_t clen = 0;

        while (*p == '/') p++;
        while (*p && (*p != '/')) {
            if (clen >= sizeof(comp) - 1) return false;
            comp[clen++] = *p++;
        }
        comp[clen] = 0;
        if (clen == 0) continue;
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            char *slash = strrchr(tmp, '/');
            if (slash) { *slash = 0; len = strlen(tmp); }
            else       { tmp[0] = 0; len = 0; }
            continue;
        }
        if (len + 1 + clen >= sizeof(tmp)) return false;
        tmp[len++] = '/';
        memcpy(tmp + len, comp, clen);
        len += clen;
        tmp[len] = 0;
    }

    if (tmp[0] == 0) { tmp[0] = '/'; tmp[1] = 0; }
    if (strlen(tmp) >= outsz) return false;
    strcpy(out, tmp);
    return true;
}

// Translate a virtual path into a FatFs path.
// Returns 1 on success, 0 when the path is the virtual root, -1 when invalid.
static int vpath_to_fat(const char *v, char *out, size_t outsz)
{
    const char *rest;
    const char *vol;

    if ((v == NULL) || (v[0] != '/')) return -1;
    if (v[1] == 0) return 0;                      // Virtual root

    if (strncmp(v + 1, "sd", 2) == 0)       { vol = "0:"; rest = v + 3; }
    else if (strncmp(v + 1, "usb", 3) == 0) { vol = "1:"; rest = v + 4; }
    else return -1;

    if ((*rest != 0) && (*rest != '/')) return -1;   // e.g. "/sdcard"

    if (snprintf(out, outsz, "%s%s", vol, (*rest == 0) ? "/" : rest) >= (int)outsz)
        return -1;
    return 1;
}

// Resolve a client supplied argument to a FatFs path, relative to the cwd.
// Returns the same codes as vpath_to_fat(). A missing argument is rejected, so
// that "DELE" with no operand can never resolve to the current directory.
static int arg_to_fat(const char *arg, char *fat, size_t fatsz)
{
    char vpath[FTP_PATH_MAX];
    if ((arg == NULL) || (*arg == 0)) return -1;
    if (!vpath_resolve(s.cwd, arg, vpath, sizeof(vpath))) return -1;
    return vpath_to_fat(vpath, fat, fatsz);
}

// ---------------------------------------------------------------------------
// Directory listing helpers
// ---------------------------------------------------------------------------

static const char *const month_name[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Format one entry the way "ls -l" does : every FTP client parses this.
static int format_list_line(char *out, size_t outsz, const char *name,
                            bool is_dir, uint32_t size, uint16_t fdate)
{
    unsigned year  = 1980 + ((fdate >> 9) & 0x7F);
    unsigned month = (fdate >> 5) & 0x0F;
    unsigned day   = fdate & 0x1F;

    if ((month < 1) || (month > 12)) month = 1;
    if (day < 1) day = 1;

    return snprintf(out, outsz, "%crw-rw-rw-   1 pico     pico     %10lu %s %2u  %4u %s\r\n",
                    is_dir ? 'd' : '-', (unsigned long)size,
                    month_name[month - 1], day, year, name);
}

// Is a FatFs volume present ? Used to hide "/usb" when no USB drive is mounted.
static bool volume_present(const char *vol)
{
    DIR d;
    if (f_opendir(&d, vol) != FR_OK) return false;
    f_closedir(&d);
    return true;
}

// ---------------------------------------------------------------------------
// Data connection
// ---------------------------------------------------------------------------

static void ftp_xfer_cleanup(void)
{
    if (s.fil_open) { f_close(&s.fil); s.fil_open = false; }
    if (s.dir_open) { f_closedir(&s.dir); s.dir_open = false; }
    s.xfer = FTP_XFER_NONE;
}

// Finish the current transfer : close the data connection and reply.
static void ftp_xfer_finish(const char *reply)
{
    ftp_xfer_cleanup();
    ftp_data_close();
    if (reply) ftp_reply("%s", reply);
}

static void ftp_data_close(void)
{
    if (s.data != NULL) {
        struct tcp_pcb *pcb = s.data;
        s.data = NULL;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_sent(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
}

static void ftp_pasv_close(void)
{
    if (s.pasv != NULL) {
        struct tcp_pcb *pcb = s.pasv;
        s.pasv = NULL;
        tcp_arg(pcb, NULL);
        tcp_accept(pcb, NULL);
        tcp_close(pcb);
    }
}

static void ftp_data_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    s.data = NULL;                    // Already freed by lwIP
    if (s.xfer != FTP_XFER_NONE) {
        ftp_xfer_cleanup();
        ftp_reply("426 Data connection closed");
    }
}

// STOR : the received data is written to the card from here. This callback is
// reached from pm_net_poll(), i.e. the Core 0 thread context, so calling FatFs
// is safe with respect to the Int13h disk code.
static err_t ftp_data_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q;
    (void)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (p == NULL) {                  // Client closed : transfer complete
        if (s.xfer == FTP_XFER_STOR) ftp_xfer_finish("226 Transfer complete");
        else ftp_data_close();
        return ERR_OK;
    }

    if (s.xfer != FTP_XFER_STOR) {    // Unexpected data, just discard it
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    PM_EnableSD();
    for (q = p; q != NULL; q = q->next) {
        UINT bw = 0;
        FRESULT fr = f_write(&s.fil, q->payload, q->len, &bw);
        if ((fr != FR_OK) || (bw != q->len)) {
            PM_ERROR("[FTP] write error %d\n", fr);
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            ftp_xfer_finish("451 Write error, transfer aborted");
            return ERR_OK;
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t ftp_data_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if ((err != ERR_OK) || (newpcb == NULL)) return ERR_VAL;

    if (s.data != NULL) {             // Only one data connection at a time
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    s.data = newpcb;
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, ftp_data_recv);
    tcp_err(newpcb, ftp_data_err);
    tcp_nagle_disable(newpcb);

    ftp_pasv_close();                 // The PASV listener is single use
    // Nothing else to do : pm_ftp_poll() starts pushing LIST/RETR data as soon
    // as s.data is set, and STOR is driven by ftp_data_recv().
    return ERR_OK;
}

// Open a PASV listening socket, reply 227.
static void ftp_cmd_pasv(void)
{
    struct tcp_pcb *pcb;
    const ip4_addr_t *ip;
    uint16_t port;
    int tries;

    ftp_pasv_close();
    ftp_data_close();

    pcb = tcp_new();
    if (pcb == NULL) { ftp_reply("425 Cannot open data connection"); return; }

    for (tries = 0; tries <= (FTP_PASV_PORT_MAX - FTP_PASV_PORT_MIN); tries++) {
        port = pasv_next_port;
        pasv_next_port++;
        if (pasv_next_port > FTP_PASV_PORT_MAX) pasv_next_port = FTP_PASV_PORT_MIN;
        if (tcp_bind(pcb, IP_ADDR_ANY, port) == ERR_OK) break;
        if (tries == (FTP_PASV_PORT_MAX - FTP_PASV_PORT_MIN)) {
            tcp_close(pcb);
            ftp_reply("425 Cannot open data connection");
            return;
        }
    }

    s.pasv = tcp_listen_with_backlog(pcb, 1);
    if (s.pasv == NULL) {
        tcp_close(pcb);
        ftp_reply("425 Cannot open data connection");
        return;
    }
    tcp_arg(s.pasv, NULL);
    tcp_accept(s.pasv, ftp_data_accept);

    // Advertise the address the client used to reach us
    ip = ip_2_ip4(&s.ctrl->local_ip);
    ftp_reply("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
              (unsigned)ip4_addr1(ip), (unsigned)ip4_addr2(ip),
              (unsigned)ip4_addr3(ip), (unsigned)ip4_addr4(ip),
              (unsigned)(port >> 8), (unsigned)(port & 0xFF));
}

// ---------------------------------------------------------------------------
// Transfer pumps, called from pm_ftp_poll()
// ---------------------------------------------------------------------------

static void pump_list(void)
{
    char line[FTP_PATH_MAX + 96];
    int lines = 0;

    while (lines < FTP_LIST_MAXLINES) {
        int len;

        if (tcp_sndbuf(s.data) < (int)sizeof(line)) break;

        if (s.list_root) {
            const char *name;
            if (s.list_stage == 0)      name = "sd";
            else if (s.list_stage == 1) name = "usb";
            else { ftp_xfer_finish("226 Transfer complete"); return; }
            s.list_stage++;

            // Hide a volume that is not mounted
            if (!volume_present((strcmp(name, "sd") == 0) ? "0:" : "1:")) continue;

            if (s.list_nlst) len = snprintf(line, sizeof(line), "%s\r\n", name);
            else             len = format_list_line(line, sizeof(line), name, true, 0, 0);
        } else {
            FILINFO fno;
            FRESULT fr;

            PM_EnableSD();
            fr = f_readdir(&s.dir, &fno);
            if ((fr != FR_OK) || (fno.fname[0] == 0)) {
                ftp_xfer_finish((fr == FR_OK) ? "226 Transfer complete"
                                              : "451 Directory read error");
                return;
            }
            if (s.list_nlst) len = snprintf(line, sizeof(line), "%s\r\n", fno.fname);
            else             len = format_list_line(line, sizeof(line), fno.fname,
                                                    (fno.fattrib & AM_DIR) != 0,
                                                    (uint32_t)fno.fsize, fno.fdate);
        }

        if ((len > 0) && (len < (int)sizeof(line))) {
            tcp_write(s.data, line, (u16_t)len, TCP_WRITE_FLAG_COPY);
            lines++;
        }
    }

    if (lines) tcp_output(s.data);
}

static void pump_retr(void)
{
    u16_t space = tcp_sndbuf(s.data);
    UINT want, br = 0;
    FRESULT fr;

    if (space < 512) { tcp_output(s.data); return; }

    want = (space < FTP_CHUNK) ? space : FTP_CHUNK;

    PM_EnableSD();
    fr = f_read(&s.fil, xfer_buf, want, &br);
    if (fr != FR_OK) {
        PM_ERROR("[FTP] read error %d\n", fr);
        ftp_xfer_finish("451 Read error, transfer aborted");
        return;
    }

    if (br > 0) {
        if (tcp_write(s.data, xfer_buf, (u16_t)br, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            // Should not happen, we checked tcp_sndbuf(). Rewind and retry later.
            f_lseek(&s.fil, f_tell(&s.fil) - br);
            return;
        }
        tcp_output(s.data);
    }

    if (br < want) ftp_xfer_finish("226 Transfer complete");   // End of file
}

void pm_ftp_poll(void)
{
    if (!ftp_ready) return;
    if (s.data == NULL) return;       // Data connection not established yet

    switch (s.xfer) {
        case FTP_XFER_LIST: pump_list(); break;
        case FTP_XFER_RETR: pump_retr(); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Open the data transfer for LIST/NLST.
static void ftp_cmd_list(const char *arg, bool nlst)
{
    char vpath[FTP_PATH_MAX];
    char fat[FTP_PATH_MAX];
    int kind;

    // Clients send "LIST -al" : skip the option arguments
    while ((arg != NULL) && (*arg == '-')) {
        while (*arg && (*arg != ' ')) arg++;
        while (*arg == ' ') arg++;
        if (*arg == 0) arg = NULL;
    }

    if (!ftp_data_ready()) { ftp_reply("425 Use PASV first"); return; }

    if (!vpath_resolve(s.cwd, arg, vpath, sizeof(vpath))) {
        ftp_reply("550 Invalid path");
        return;
    }
    kind = vpath_to_fat(vpath, fat, sizeof(fat));
    if (kind < 0) { ftp_reply("550 No such directory"); return; }

    s.list_nlst  = nlst;
    s.list_root  = (kind == 0);
    s.list_stage = 0;

    if (!s.list_root) {
        PM_EnableSD();
        if (f_opendir(&s.dir, fat) != FR_OK) {
            ftp_reply("550 No such directory");
            return;
        }
        s.dir_open = true;
    }

    s.xfer = FTP_XFER_LIST;
    ftp_reply("150 Opening ASCII mode data connection for file list");
}

static void ftp_cmd_retr(const char *arg)
{
    char fat[FTP_PATH_MAX];

    if ((arg == NULL) || (*arg == 0)) { ftp_reply("501 Missing file name"); return; }
    if (!ftp_data_ready()) { ftp_reply("425 Use PASV first"); return; }
    if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 No such file"); return; }

    PM_EnableSD();
    if (f_open(&s.fil, fat, FA_READ) != FR_OK) {
        ftp_reply("550 No such file");
        return;
    }
    s.fil_open = true;
    s.xfer     = FTP_XFER_RETR;
    ftp_reply("150 Opening BINARY mode data connection");
}

static void ftp_cmd_stor(const char *arg, bool append)
{
    char fat[FTP_PATH_MAX];

    if ((arg == NULL) || (*arg == 0)) { ftp_reply("501 Missing file name"); return; }
    if (!ftp_data_ready()) { ftp_reply("425 Use PASV first"); return; }
    if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 Invalid path"); return; }

    PM_EnableSD();
    if (f_open(&s.fil, fat,
               append ? (FA_WRITE | FA_OPEN_APPEND)
                      : (FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) {
        ftp_reply("550 Cannot create file");
        return;
    }
    s.fil_open = true;
    s.xfer     = FTP_XFER_STOR;
    ftp_reply("150 Opening BINARY mode data connection");
}

static void ftp_process_cmd(char *line)
{
    char *cmd = line;
    char *arg;

    while (*cmd == ' ') cmd++;
    arg = strchr(cmd, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    else       arg = NULL;

    // Upper case the verb only, arguments (file names) keep their case
    { char *p = cmd; while (*p) { if ((*p >= 'a') && (*p <= 'z')) *p -= 32; p++; } }

    // --- Commands allowed before authentication ---
    if (strcmp(cmd, "USER") == 0) {
        s.user_ok = (PM_NetCfg.ftp_user[0] == 0) ||
                    ((arg != NULL) && (strcmp(arg, PM_NetCfg.ftp_user) == 0));
        if (PM_NetCfg.ftp_user[0] == 0) {
            s.authed = true;
            ftp_reply("230 Login successful");
        } else {
            ftp_reply("331 Password required");
        }
        return;
    }
    if (strcmp(cmd, "PASS") == 0) {
        if (s.user_ok && ((PM_NetCfg.ftp_pass[0] == 0) ||
                          ((arg != NULL) && (strcmp(arg, PM_NetCfg.ftp_pass) == 0)))) {
            s.authed = true;
            ftp_reply("230 Login successful");
        } else {
            s.authed = false;
            ftp_reply("530 Login incorrect");
        }
        return;
    }
    if (strcmp(cmd, "QUIT") == 0) { ftp_reply("221 Goodbye"); return; }
    if (strcmp(cmd, "NOOP") == 0) { ftp_reply("200 OK"); return; }
    if (strcmp(cmd, "FEAT") == 0) {
        ftp_reply("211-Features:");
        ftp_reply(" SIZE");
        ftp_reply(" MDTM");
        ftp_reply("211 End");
        return;
    }

    if (!s.authed) { ftp_reply("530 Please login with USER and PASS"); return; }

    // --- Session / info ---
    if (strcmp(cmd, "SYST") == 0) { ftp_reply("215 UNIX Type: L8"); return; }
    if (strcmp(cmd, "TYPE") == 0) { ftp_reply("200 Type set"); return; }
    if (strcmp(cmd, "MODE") == 0) { ftp_reply("200 Mode set to S"); return; }
    if (strcmp(cmd, "STRU") == 0) { ftp_reply("200 Structure set to F"); return; }
    if (strcmp(cmd, "OPTS") == 0) { ftp_reply("200 OK"); return; }
    if ((strcmp(cmd, "PWD") == 0) || (strcmp(cmd, "XPWD") == 0)) {
        ftp_reply("257 \"%s\" is current directory", s.cwd);
        return;
    }
    if (strcmp(cmd, "PORT") == 0) {
        ftp_reply("502 Active mode not supported, use PASV");
        return;
    }
    if (strcmp(cmd, "PASV") == 0) { ftp_cmd_pasv(); return; }
    if (strcmp(cmd, "ABOR") == 0) {
        ftp_xfer_finish("226 Abort successful");
        return;
    }

    // --- Navigation ---
    if ((strcmp(cmd, "CWD") == 0) || (strcmp(cmd, "XCWD") == 0) ||
        (strcmp(cmd, "CDUP") == 0)) {
        char vpath[FTP_PATH_MAX];
        char fat[FTP_PATH_MAX];
        int kind;
        const char *target = (strcmp(cmd, "CDUP") == 0) ? ".." : arg;

        if ((target == NULL) || (*target == 0)) { ftp_reply("501 Missing path"); return; }
        if (!vpath_resolve(s.cwd, target, vpath, sizeof(vpath))) {
            ftp_reply("550 Invalid path"); return;
        }
        kind = vpath_to_fat(vpath, fat, sizeof(fat));
        if (kind < 0) { ftp_reply("550 No such directory"); return; }
        if (kind == 1) {
            DIR d;
            PM_EnableSD();
            if (f_opendir(&d, fat) != FR_OK) { ftp_reply("550 No such directory"); return; }
            f_closedir(&d);
        }
        strncpy(s.cwd, vpath, sizeof(s.cwd) - 1);
        s.cwd[sizeof(s.cwd) - 1] = 0;
        ftp_reply("250 Directory changed to %s", s.cwd);
        return;
    }

    // --- Transfers ---
    if (strcmp(cmd, "LIST") == 0) { ftp_cmd_list(arg, false); return; }
    if (strcmp(cmd, "NLST") == 0) { ftp_cmd_list(arg, true);  return; }
    if (strcmp(cmd, "RETR") == 0) { ftp_cmd_retr(arg);        return; }
    if (strcmp(cmd, "STOR") == 0) { ftp_cmd_stor(arg, false); return; }
    if (strcmp(cmd, "APPE") == 0) { ftp_cmd_stor(arg, true);  return; }

    // --- File management ---
    if (strcmp(cmd, "SIZE") == 0) {
        char fat[FTP_PATH_MAX];
        FILINFO fno;
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 No such file"); return; }
        PM_EnableSD();
        if (f_stat(fat, &fno) != FR_OK) { ftp_reply("550 No such file"); return; }
        ftp_reply("213 %lu", (unsigned long)fno.fsize);
        return;
    }
    if (strcmp(cmd, "MDTM") == 0) {
        char fat[FTP_PATH_MAX];
        FILINFO fno;
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 No such file"); return; }
        PM_EnableSD();
        if (f_stat(fat, &fno) != FR_OK) { ftp_reply("550 No such file"); return; }
        ftp_reply("213 %04u%02u%02u%02u%02u%02u",
                  1980 + ((fno.fdate >> 9) & 0x7F), (fno.fdate >> 5) & 0x0F, fno.fdate & 0x1F,
                  (fno.ftime >> 11) & 0x1F, (fno.ftime >> 5) & 0x3F, (fno.ftime & 0x1F) * 2);
        return;
    }
    if (strcmp(cmd, "DELE") == 0) {
        char fat[FTP_PATH_MAX];
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 No such file"); return; }
        PM_EnableSD();
        ftp_reply((f_unlink(fat) == FR_OK) ? "250 File deleted" : "550 Delete failed");
        return;
    }
    if ((strcmp(cmd, "MKD") == 0) || (strcmp(cmd, "XMKD") == 0)) {
        char fat[FTP_PATH_MAX];
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 Invalid path"); return; }
        PM_EnableSD();
        ftp_reply((f_mkdir(fat) == FR_OK) ? "257 Directory created" : "550 Create failed");
        return;
    }
    if ((strcmp(cmd, "RMD") == 0) || (strcmp(cmd, "XRMD") == 0)) {
        char fat[FTP_PATH_MAX];
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 Invalid path"); return; }
        PM_EnableSD();
        ftp_reply((f_unlink(fat) == FR_OK) ? "250 Directory removed" : "550 Remove failed");
        return;
    }
    if (strcmp(cmd, "RNFR") == 0) {
        FILINFO fno;
        if (arg_to_fat(arg, s.rnfr, sizeof(s.rnfr)) != 1) {
            s.rnfr[0] = 0;
            ftp_reply("550 No such file");
            return;
        }
        PM_EnableSD();
        if (f_stat(s.rnfr, &fno) != FR_OK) {
            s.rnfr[0] = 0;
            ftp_reply("550 No such file");
            return;
        }
        ftp_reply("350 Ready for RNTO");
        return;
    }
    if (strcmp(cmd, "RNTO") == 0) {
        char fat[FTP_PATH_MAX];
        if (s.rnfr[0] == 0) { ftp_reply("503 Use RNFR first"); return; }
        if (arg_to_fat(arg, fat, sizeof(fat)) != 1) { ftp_reply("550 Invalid path"); return; }
        PM_EnableSD();
        ftp_reply((f_rename(s.rnfr, fat) == FR_OK) ? "250 Rename successful"
                                                   : "550 Rename failed");
        s.rnfr[0] = 0;
        return;
    }

    ftp_reply("500 Unknown command");
}

// ---------------------------------------------------------------------------
// Control connection
// ---------------------------------------------------------------------------

static void ftp_session_reset(void)
{
    ftp_xfer_cleanup();
    ftp_data_close();
    ftp_pasv_close();
    s.ctrl    = NULL;
    s.authed  = false;
    s.user_ok = false;
    s.cmdlen  = 0;
    s.rnfr[0] = 0;
    strcpy(s.cwd, "/");
}

static void ftp_ctrl_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    s.ctrl = NULL;                    // Already freed by lwIP
    ftp_session_reset();
}

static err_t ftp_ctrl_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q;
    (void)arg;

    if ((err != ERR_OK) || (p == NULL)) {
        if (p) pbuf_free(p);
        ftp_session_reset();
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        return ERR_OK;
    }

    for (q = p; q != NULL; q = q->next) {
        uint16_t i;
        const char *d = (const char *)q->payload;
        for (i = 0; i < q->len; i++) {
            char c = d[i];
            if (c == '\n') {
                s.cmd[s.cmdlen] = 0;
                if ((s.cmdlen > 0) && (s.cmd[s.cmdlen - 1] == '\r'))
                    s.cmd[s.cmdlen - 1] = 0;
                s.cmdlen = 0;
                if (s.cmd[0]) ftp_process_cmd(s.cmd);
                if (s.ctrl == NULL) {          // Session dropped meanwhile
                    pbuf_free(p);
                    return ERR_OK;
                }
            } else if (s.cmdlen < (FTP_CMD_MAX - 1)) {
                s.cmd[s.cmdlen++] = c;
            }
            // Overlong lines are silently truncated
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t ftp_ctrl_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if ((err != ERR_OK) || (newpcb == NULL)) return ERR_VAL;

    if (s.ctrl != NULL) {             // Single session server
        tcp_arg(newpcb, NULL);
        tcp_recv(newpcb, NULL);
        tcp_write(newpcb, "421 Server busy\r\n", 17, TCP_WRITE_FLAG_COPY);
        tcp_output(newpcb);
        tcp_close(newpcb);
        return ERR_OK;
    }

    ftp_session_reset();
    s.ctrl = newpcb;
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, ftp_ctrl_recv);
    tcp_err(newpcb, ftp_ctrl_err);
    tcp_nagle_disable(newpcb);

    if (PM_NetCfg.ftp_user[0] == 0) {
        ftp_reply("220 PicoMEM FTP ready (anonymous)");
    } else {
        ftp_reply("220 PicoMEM FTP ready");
    }
    PM_INFO("[FTP] Client connected\n");
    return ERR_OK;
}

void pm_ftp_init(void)
{
    struct tcp_pcb *pcb;

    if (ftp_ready) return;

    memset(&s, 0, sizeof(s));
    strcpy(s.cwd, "/");

    pcb = tcp_new();
    if (pcb == NULL) { PM_ERROR("[FTP] tcp_new failed\n"); return; }

    if (tcp_bind(pcb, IP_ADDR_ANY, FTP_CTRL_PORT) != ERR_OK) {
        PM_ERROR("[FTP] bind failed\n");
        tcp_close(pcb);
        return;
    }

    ftp_listen_pcb = tcp_listen_with_backlog(pcb, 1);
    if (ftp_listen_pcb == NULL) {
        PM_ERROR("[FTP] listen failed\n");
        tcp_close(pcb);
        return;
    }
    tcp_accept(ftp_listen_pcb, ftp_ctrl_accept);

    ftp_ready = true;
    PM_INFO("[FTP] Server listening on port %d\n", FTP_CTRL_PORT);
}

#else  // !USE_PM_NET

void pm_ftp_init(void) { }
void pm_ftp_poll(void) { }
bool pm_ftp_busy(void) { return false; }

#endif // USE_PM_NET
