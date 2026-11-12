/*
 * N-Gage QD board: the Bluetooth module (LPRF) behind btldd.ldd, as a minimal HCI controller.
 * QEMU-free core (registers, packet framing, HCI replies); the board glue is in ngage.c (CBus
 * device 5) and ngage_keys.c (its CTSI interrupt bit). Host unit test: tests/ngage_lprf_test.c.
 * Protocol: docs/research/ngage-bluetooth.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NGAGE_LPRF_H
#define NGAGE_LPRF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* CBus device 5 hardware registers (cbus.dll register table 0x5037e544, entries 37-44) [V] */
#define LPRF_REG_DATA     0     /* R: next 2 bytes of the RX packet; W: next 2 bytes of the TX packet */
#define LPRF_REG_RXLEN    1     /* R: length of the RX packet waiting */
#define LPRF_REG_IRQ      6     /* R: interrupt status (clears it); W: control */
#define LPRF_REG_WRITE    8     /* W: 0x8000|len start of a TX packet, 0xC000|len its end; R: acknowledge */
#define LPRF_REG_RESET    9     /* R: reset reason */
#define LPRF_REG_BUFREQ   10    /* W: 0x8000 = how much can the module take? */

/* interrupt status bits (btldd's interrupt DFC 0x503f0854) [V for the handling, I for names] */
#define LPRF_IRQ_AWAKE    0x0004
#define LPRF_IRQ_RX       0x0010
#define LPRF_IRQ_ACK      0x0040    /* LPRF_REG_WRITE holds an acknowledge */
#define LPRF_IRQ_RESET    0x0080    /* LPRF_REG_RESET holds the reason; bit 3 = powered up */

#define LPRF_MAX_PACKET   1024      /* btldd writes at most 520 bytes per request */
#define LPRF_RXQ          16

typedef struct {
    uint8_t data[LPRF_MAX_PACKET];
    uint16_t len;
} LprfPacket;

typedef struct NgageLprf {
    /* interrupt status the driver has not read yet, and bits raised but not yet delivered */
    uint16_t status, pending;
    uint16_t ack;               /* LPRF_REG_WRITE read value */
    uint16_t reset_reason;
    bool started;
    /* host -> module packet being written */
    uint8_t tx[LPRF_MAX_PACKET];
    unsigned tx_len, tx_expect;
    /* module -> host packets (head is being read) */
    LprfPacket rxq[LPRF_RXQ];
    unsigned rx_head, rx_count, rx_pos;
    bool rx_announced;          /* LPRF_IRQ_RX raised for the head packet */
    /* controller state */
    uint8_t bdaddr[6];
    char name[249];
    uint8_t scan_enable;
    uint8_t cod[3];
    unsigned commands, unknown;
    /* board callbacks */
    void (*kick)(void *opaque);                     /* deliver pending bits soon (a timer) */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} NgageLprf;

void ngage_lprf_init(NgageLprf *s);
void ngage_lprf_write(NgageLprf *s, unsigned reg, uint16_t val);
uint16_t ngage_lprf_read(NgageLprf *s, unsigned reg);
/* the board's timer: pending bits become status; true = raise the interrupt */
bool ngage_lprf_deliver(NgageLprf *s);

/* HCI controller: one complete host packet in (type, 0, HCI packet; see the .c), replies queued */
void ngage_lprf_hci(NgageLprf *s, const uint8_t *pkt, unsigned len);

#endif
