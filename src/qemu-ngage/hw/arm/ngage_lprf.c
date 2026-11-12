/*
 * N-Gage QD board: the Bluetooth module (LPRF) as a minimal HCI controller. QEMU-free core;
 * see ngage_lprf.h and docs/research/ngage-bluetooth.md.
 *
 * Register protocol, from btldd.ldd (0x503EFFD0) [V unless marked]:
 *   start-up   the driver writes IRQ = 0 then 1; the module answers LPRF_IRQ_RESET with reset reason
 *              bit 3 (btldd 0x503f0b70: else "Reset reason" and -36). Driver state 1 then writes
 *              BUFREQ = 0x8000; the module answers LPRF_IRQ_ACK with WRITE = 0x0000|size: size >= 520
 *              tells the driver it never has to ask before a packet (btldd 0x503f105c).
 *   host->mod  WRITE = 0x8000|len, len bytes through DATA (two per word, first byte in bits 15-8, an odd
 *              last byte alone in bits 15-8), WRITE = 0xC000|len; the module answers LPRF_IRQ_ACK with
 *              WRITE = 0x4000|len (btldd 0x503f10a4: anything else is "Unexpected data acknowledge").
 *   mod->host  LPRF_IRQ_RX; the driver reads RXLEN, then DATA words (first byte in bits 15-8) up to
 *              512 bytes per pass until RXLEN bytes are read. One packet per RXLEN.
 *   status     IRQ reads return the bits raised since the last read and clear them [I: btldd never
 *              writes them back]. IRQ = 2 is a wake-up request: answered with LPRF_IRQ_AWAKE [I].
 *   The module never sleeps (no LPRF_IRQ 0x20 / 0x100), so the driver's wake-up paths stay unused.
 *
 * HCI over the packets [V: hci.dll header writer 0x5046dee4, its receiver; build/ngage/bt-hci-research.md]:
 *   H4 with the packet-type byte padded to 16 bits: type, 0, then the HCI packet (0x01 command,
 *   0x02 ACL data, 0x04 event). HCI_Reset is written as 01 00 03 0C 00. hci.dll reads exact byte
 *   counts (2: type; event: 2 = code + length, then the parameters), so RXLEN is the exact length.
 * The controller is a Bluetooth 1.1 controller that is switched on but has nobody to talk to:
 * every command succeeds, inquiries find nothing, connections fail with "page timeout". Whether
 * Bluetooth is "on" for the user is the phone's own setting (BtUi); the module only has to exist.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "ngage_lprf.h"

#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_EVENT 0x04

#define EV_INQUIRY_COMPLETE    0x01
#define EV_CONN_COMPLETE       0x03
#define EV_REMOTE_NAME         0x07
#define EV_CMD_COMPLETE        0x0e
#define EV_CMD_STATUS          0x0f

#define ERR_UNKNOWN_HCI_CMD    0x01
#define ERR_NO_CONNECTION      0x02
#define ERR_PAGE_TIMEOUT       0x04
#define ERR_INVALID_PARAMS     0x12

#define LPRF_BUFFER            520      /* what BUFREQ reports: >= 520, see above */

static void lprf_log(NgageLprf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void lprf_log(NgageLprf *s, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!s->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s->log(s->opaque, buf);
}

static void lprf_raise(NgageLprf *s, uint16_t bits)
{
    s->pending |= bits;
    if (s->kick) {
        s->kick(s->opaque);
    }
}

void ngage_lprf_init(NgageLprf *s)
{
    void (*kick)(void *) = s->kick;
    void (*log)(void *, const char *) = s->log;
    void *opaque = s->opaque;

    memset(s, 0, sizeof(*s));
    s->kick = kick;
    s->log = log;
    s->opaque = opaque;
    /* 00:02:EE (a Nokia OUI) + a fixed tail [I]; the name is the phone's default [I] */
    static const uint8_t addr[6] = { 0x56, 0x34, 0x12, 0xee, 0x02, 0x00 };   /* little endian */
    memcpy(s->bdaddr, addr, 6);
    strcpy(s->name, "N-Gage");
    s->cod[0] = 0x04;           /* phone: smart phone (minor 3) [I] */
    s->cod[1] = 0x02;
    s->cod[2] = 0x5a;
    s->reset_reason = 0x0008;
}

/* ---- the RX queue --------------------------------------------------------------------------- */

static void lprf_announce(NgageLprf *s)
{
    if (s->rx_count && !s->rx_announced) {
        s->rx_announced = true;
        lprf_raise(s, LPRF_IRQ_RX);
    }
}

static void lprf_queue(NgageLprf *s, const uint8_t *p, unsigned len)
{
    LprfPacket *pk;

    if (s->rx_count == LPRF_RXQ || len > LPRF_MAX_PACKET) {
        lprf_log(s, "rx queue full, packet of %u bytes dropped", len);
        return;
    }
    pk = &s->rxq[(s->rx_head + s->rx_count) % LPRF_RXQ];
    memcpy(pk->data, p, len);
    pk->len = len;
    s->rx_count++;
    lprf_announce(s);
}

static void lprf_event(NgageLprf *s, uint8_t code, const uint8_t *params, unsigned plen)
{
    uint8_t p[4 + 255];

    p[0] = H4_EVENT;
    p[1] = 0;
    p[2] = code;
    p[3] = plen;
    memcpy(p + 4, params, plen);
    lprf_queue(s, p, 4 + plen);
}

static void cmd_complete(NgageLprf *s, uint16_t op, const uint8_t *ret, unsigned rlen)
{
    uint8_t p[255];

    p[0] = 1;                   /* the host may send one more command */
    p[1] = op & 0xff;
    p[2] = op >> 8;
    memcpy(p + 3, ret, rlen);
    lprf_event(s, EV_CMD_COMPLETE, p, 3 + rlen);
}

static void cmd_status(NgageLprf *s, uint16_t op, uint8_t status)
{
    uint8_t p[4] = { status, 1, op & 0xff, op >> 8 };

    lprf_event(s, EV_CMD_STATUS, p, 4);
}

/* ---- HCI commands ------------------------------------------------------------------------- */

static void hci_command(NgageLprf *s, const uint8_t *c, unsigned len)
{
    uint16_t op;
    const uint8_t *arg;
    unsigned alen;
    uint8_t r[255] = { 0 };     /* r[0] = status 0 */
    unsigned rl = 1;

    if (len < 3) {
        lprf_log(s, "short HCI command (%u bytes)", len);
        return;
    }
    op = c[0] | c[1] << 8;
    arg = c + 3;
    alen = len - 3 < c[2] ? len - 3 : c[2];
    s->commands++;
    lprf_log(s, "HCI command %04x, %u parameter bytes", op, alen);

    switch (op) {
    /* link control (OGF 1) */
    case 0x0401:                                            /* Inquiry: nobody around */
        cmd_status(s, op, 0);
        r[0] = 0;
        lprf_event(s, EV_INQUIRY_COMPLETE, r, 1);
        return;
    case 0x0405: {                                          /* Create Connection */
        uint8_t cc[11] = { ERR_PAGE_TIMEOUT };
        cmd_status(s, op, 0);
        if (alen >= 6) {
            memcpy(cc + 3, arg, 6);                         /* handle 0, bdaddr, link type ACL */
        }
        cc[9] = 1;
        lprf_event(s, EV_CONN_COMPLETE, cc, 11);
        return;
    }
    case 0x0419: {                                          /* Remote Name Request */
        uint8_t rn[255] = { ERR_PAGE_TIMEOUT };
        cmd_status(s, op, 0);
        if (alen >= 6) {
            memcpy(rn + 1, arg, 6);
        }
        lprf_event(s, EV_REMOTE_NAME, rn, 255);
        return;
    }
    case 0x0402:                                            /* Inquiry Cancel */
        break;
    /* host controller and baseband (OGF 3) */
    case 0x0c03:                                            /* Reset */
        s->scan_enable = 0;
        break;
    case 0x0c13:                                            /* Write Local Name */
        memset(s->name, 0, sizeof(s->name));
        memcpy(s->name, arg, alen < 248 ? alen : 248);
        break;
    case 0x0c14:                                            /* Read Local Name */
        memcpy(r + 1, s->name, 248);
        rl = 1 + 248;
        break;
    case 0x0c16: {                                          /* Write Connection Accept Timeout */
        /*
         * Spec range 0x0001..0xB540 (BT 1.1 part H:1 4.7.15); outside it a controller answers
         * Invalid HCI Command Parameters. BTTestServer (Phoenix's BT test server) writes 0xFFFF
         * at boot, gets this error and gives up its HCI, so the BT stack can open btldd's only
         * channel (docs/research/ngage-bluetooth.md) [I: the real module's answer].
         */
        unsigned t = alen >= 2 ? arg[0] | arg[1] << 8 : 0;
        if (t < 0x0001 || t > 0xb540) {
            lprf_log(s, "connection accept timeout %#x out of range: invalid parameters", t);
            r[0] = ERR_INVALID_PARAMS;
        }
        break;
    }
    case 0x0c18: {                                          /* Write Page Timeout: 0x0001..0xFFFF */
        unsigned t = alen >= 2 ? arg[0] | arg[1] << 8 : 0;
        if (t == 0) {
            r[0] = ERR_INVALID_PARAMS;
        }
        break;
    }
    case 0x0c15: case 0x0c17:                               /* Read Conn Accept / Page Timeout */
        r[1] = 0x00; r[2] = 0x20;                           /* 0x2000 slots */
        rl = 3;
        break;
    case 0x0c19:                                            /* Read Scan Enable */
        r[1] = s->scan_enable;
        rl = 2;
        break;
    case 0x0c1a:                                            /* Write Scan Enable */
        s->scan_enable = alen ? arg[0] : 0;
        lprf_log(s, "scan enable %u", s->scan_enable);
        break;
    case 0x0c1b: case 0x0c1d:                               /* Read Page / Inquiry Scan Activity */
        r[1] = 0x00; r[2] = 0x08; r[3] = 0x12; r[4] = 0x00;  /* interval 0x800, window 0x12 */
        rl = 5;
        break;
    case 0x0c1f: case 0x0c20: case 0x0c21: case 0x0c22:     /* Read Authentication/Encryption... */
        rl = 2;
        break;
    case 0x0c23:                                            /* Read Class of Device */
        memcpy(r + 1, s->cod, 3);
        rl = 4;
        break;
    case 0x0c24:                                            /* Write Class of Device */
        if (alen >= 3) {
            memcpy(s->cod, arg, 3);
        }
        break;
    case 0x0c25:                                            /* Read Voice Setting */
        r[1] = 0x60;
        rl = 3;
        break;
    case 0x0c38:                                            /* Read Number Of Supported IAC */
        r[1] = 1;
        rl = 2;
        break;
    case 0x0c39:                                            /* Read Current IAC LAP: GIAC */
        r[1] = 1; r[2] = 0x33; r[3] = 0x8b; r[4] = 0x9e;
        rl = 5;
        break;
    /* informational (OGF 4) */
    case 0x1001:                                            /* Read Local Version Information */
        r[1] = 1;                                           /* HCI 1.1 */
        r[2] = 0x00; r[3] = 0x00;                           /* HCI revision */
        r[4] = 1;                                           /* LMP 1.1 */
        r[5] = 0x01; r[6] = 0x00;                           /* manufacturer 1 = Nokia Mobile Phones [I] */
        r[7] = 0x00; r[8] = 0x00;                           /* LMP subversion */
        rl = 9;
        break;
    case 0x1003:                                            /* Read Local Supported Features */
        r[1] = 0xff; r[2] = 0xff; r[3] = 0x0f; r[4] = 0x00; /* 1.1 slots, encryption, modes, SCO */
        rl = 9;
        break;
    case 0x1005:                                            /* Read Buffer Size */
        r[1] = 0x53; r[2] = 0x01;                           /* ACL 339 bytes (DH5) */
        r[3] = 0x40;                                        /* SCO 64 bytes */
        r[4] = 0x04; r[5] = 0x00;                           /* 4 ACL buffers */
        r[6] = 0x00; r[7] = 0x00;                           /* no SCO buffers */
        rl = 8;
        break;
    case 0x1007:                                            /* Read Country Code */
        rl = 2;
        break;
    case 0x1009:                                            /* Read BD_ADDR */
        memcpy(r + 1, s->bdaddr, 6);
        rl = 7;
        break;
    default:
        if ((op >> 10) == 0x3f) {                           /* vendor: accept */
            lprf_log(s, "vendor command %04x (%u bytes): accepted", op, alen);
            break;
        }
        if ((op >> 10) == 3 && (op & 0x3ff) >= 0x20) {
            /*
             * The other OGF 3 commands are writes (status only) or reads we don't know; a write
             * gets success, a read would need its fields: logged so they can be added.
             */
            lprf_log(s, "HCI command %04x (%u bytes): success, no return fields", op, alen);
            break;
        }
        if ((op >> 10) <= 6 && alen) {
            lprf_log(s, "HCI command %04x (%u bytes): success, no return fields", op, alen);
            break;
        }
        s->unknown++;
        lprf_log(s, "HCI command %04x (%u bytes): unknown command", op, alen);
        r[0] = ERR_UNKNOWN_HCI_CMD;
        break;
    }
    cmd_complete(s, op, r, rl);
}

void ngage_lprf_hci(NgageLprf *s, const uint8_t *pkt, unsigned len)
{
    if (len < 2) {
        lprf_log(s, "host packet of %u bytes: dropped", len);
        return;
    }
    switch (pkt[0]) {
    case H4_CMD:
        hci_command(s, pkt + 2, len - 2);
        return;
    case H4_ACL:
        lprf_log(s, "ACL data (%u bytes) without a connection: dropped", len - 2);
        return;
    default:
        lprf_log(s, "host packet of type %02x (%u bytes): dropped", pkt[0], len);
        return;
    }
}

/* ---- registers -------------------------------------------------------------------------- */

void ngage_lprf_write(NgageLprf *s, unsigned reg, uint16_t val)
{
    switch (reg) {
    case LPRF_REG_IRQ:
        if (val == 1) {                                     /* start: the module powers up */
            lprf_log(s, "start (control 1): reset indication");
            s->started = true;
            s->tx_len = s->tx_expect = 0;
            lprf_raise(s, LPRF_IRQ_RESET);
        } else if (val == 2) {
            lprf_raise(s, LPRF_IRQ_AWAKE);
        }
        return;
    case LPRF_REG_BUFREQ:
        s->ack = LPRF_BUFFER;
        lprf_raise(s, LPRF_IRQ_ACK);
        return;
    case LPRF_REG_WRITE:
        if ((val & 0xc000) == 0x8000) {
            s->tx_expect = val & 0x3fff;
            s->tx_len = 0;
            if (s->tx_expect > LPRF_MAX_PACKET) {
                lprf_log(s, "TX packet of %u bytes: too long", s->tx_expect);
                s->tx_expect = LPRF_MAX_PACKET;
            }
        } else if ((val & 0xc000) == 0xc000) {
            unsigned len = val & 0x3fff;

            if (len != s->tx_len) {
                lprf_log(s, "TX end says %u bytes, %u written", len, s->tx_len);
            }
            s->ack = 0x4000 | len;
            lprf_raise(s, LPRF_IRQ_ACK);
            ngage_lprf_hci(s, s->tx, s->tx_len);
            s->tx_len = s->tx_expect = 0;
        } else {
            lprf_log(s, "write register %04x", val);
        }
        return;
    case LPRF_REG_DATA:
        if (s->tx_len < s->tx_expect) {
            s->tx[s->tx_len++] = val >> 8;
        }
        if (s->tx_len < s->tx_expect) {
            s->tx[s->tx_len++] = val & 0xff;
        }
        return;
    default:
        lprf_log(s, "write reg %u = %04x", reg, val);
        return;
    }
}

uint16_t ngage_lprf_read(NgageLprf *s, unsigned reg)
{
    uint16_t v;
    LprfPacket *pk;

    switch (reg) {
    case LPRF_REG_IRQ:
        v = s->status;
        s->status = 0;
        return v;
    case LPRF_REG_WRITE:
        return s->ack;
    case LPRF_REG_RESET:
        return s->reset_reason;
    case LPRF_REG_RXLEN:
        s->rx_pos = 0;
        return s->rx_count ? s->rxq[s->rx_head].len : 0;
    case LPRF_REG_DATA:
        if (!s->rx_count) {
            return 0;
        }
        pk = &s->rxq[s->rx_head];
        v = s->rx_pos < pk->len ? pk->data[s->rx_pos] << 8 : 0;
        s->rx_pos++;
        if (s->rx_pos < pk->len) {
            v |= pk->data[s->rx_pos];
        }
        s->rx_pos++;
        if (s->rx_pos >= pk->len) {                         /* packet read: next one */
            s->rx_head = (s->rx_head + 1) % LPRF_RXQ;
            s->rx_count--;
            s->rx_pos = 0;
            s->rx_announced = false;
            lprf_announce(s);
        }
        return v;
    default:
        return 0;
    }
}

bool ngage_lprf_deliver(NgageLprf *s)
{
    uint16_t bits = s->pending;

    s->pending = 0;
    s->status |= bits;
    return bits != 0;
}
