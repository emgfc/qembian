/*
 * N-Gage (EKA1) board: the keypad (Q53).
 *
 * ekeyb.dll scans a 6 x 5 key matrix itself through two UPP ports [V, static RE of ekeyb]:
 *   0x0C150400 columns: +00 input (active low, bits 0-4), +02 drive latch (low byte sets, high
 *              byte clears), +04 interrupt enable [I], +08/+0A other latches
 *   0x0C15040C rows (bits 0-5): +00 input, +02 drive latch, +04 (0x0C150410) row interrupt
 *              enable (low byte enables, high byte disables [I])
 *   0x0C150100 CTSI status: bit 2 = keypad (bit 15 = CBUS/UEM); write 1 to clear [I]
 *   interrupt source 25 (IrqCTSI_IRQ).
 * Idle: all columns driven, row interrupts enabled; a press pulls its row. The ISR disables the
 * row interrupts, acks, and a DFC scans: drive one row, read the columns. Scan codes come from
 * ekeyb's layout 3 table (0x5041c4b0, index row * 5 + col) [V], so the board only needs to
 * know where each key sits in the matrix.
 *
 * Host keys (QEMU input layer: ngage_phone.py, QMP input-send-event) map through NGAGE_KEYS.
 * The power key is not in the matrix (UEM over CBUS, dev 1 reg 0x1F bit 1): not modelled yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "ngage_keys.h"

#define NGAGE_KPD_BASE   0x0c150400
#define NGAGE_CTSI_BASE  0x0c150100
#define NGAGE_CTSI_KPD   0x0004

/* host key -> matrix position (row, column) [V: ekeyb layout 3; key names inferred from the
 * standard scan codes] */
static const NgageKey ngage_keys[] = {
    { KEY_UP, "up", 0, 0 },          { KEY_LEFT, "left", 0, 1 },
    { KEY_F2, "right soft", 0, 2 },  { KEY_BACKSPACE, "clear", 0, 3 },
    { KEY_F5, "menu", 0, 4 },
    { KEY_DOWN, "down", 1, 0 },      { KEY_RIGHT, "right", 1, 1 },
    { KEY_F4, "end", 1, 2 },         { KEY_LEFTSHIFT, "edit", 1, 3 },
    { KEY_ENTER, "select", 1, 4 },
    { KEY_F1, "left soft", 2, 0 },   { KEY_1, "1", 2, 2 }, { KEY_6, "6", 2, 3 }, { KEY_8, "8", 2, 4 },
    { KEY_F3, "call", 3, 0 },        { KEY_5, "5", 3, 2 }, { KEY_7, "7", 3, 3 }, { KEY_F10, "#", 3, 4 },
    { KEY_9, "9", 4, 2 },            { KEY_0, "0", 4, 3 }, { KEY_3, "3", 4, 4 },
    { KEY_KPASTERISK, "*", 5, 2 },   { KEY_2, "2", 5, 3 }, { KEY_4, "4", 5, 4 },
    { KEY_F12, "power", -1, -1 },
};

typedef struct {
    MemoryRegion mr, ctsi_mr;
    qemu_irq irq;
    uint16_t ctsi;
    uint16_t regs[16];
    uint8_t xc, xr, ier;            /* driven columns, driven rows, row interrupt enable */
    uint8_t pressed[6];             /* pressed[row] = column bits */
} NgageKpd;

static NgageKpd kpd;

static uint8_t kpd_rows(NgageKpd *k)            /* rows pulled by a pressed key on a driven column */
{
    uint8_t v = 0;

    for (int r = 0; r < 6; r++) {
        if (k->pressed[r] & k->xc) {
            v |= 1 << r;
        }
    }
    return v;
}

static uint8_t kpd_cols(NgageKpd *k)            /* columns pulled through a driven row */
{
    uint8_t v = 0;

    for (int r = 0; r < 6; r++) {
        if (k->xr & (1 << r)) {
            v |= k->pressed[r];
        }
    }
    return v;
}

static void kpd_update(NgageKpd *k)
{
    if (kpd_rows(k) & k->ier) {
        k->ctsi |= NGAGE_CTSI_KPD;
    }
    qemu_set_irq(k->irq, (k->ctsi & NGAGE_CTSI_KPD) != 0);
}

static void kpd_latch(uint8_t *l, uint16_t v, uint8_t mask)
{
    *l = (*l | (v & mask)) & ~((v >> 8) & mask);
}

static uint64_t kpd_read(void *opaque, hwaddr a, unsigned size)
{
    NgageKpd *k = opaque;

    if (a == 0x00) {
        return ~kpd_cols(k) & 0x1f;             /* active low */
    }
    if (a == 0x0c) {
        return ~kpd_rows(k) & 0x3f;
    }
    return k->regs[(a >> 1) & 15];
}

static void kpd_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    NgageKpd *k = opaque;

    k->regs[(a >> 1) & 15] = v;
    if (a == 0x02) {
        kpd_latch(&k->xc, v, 0x1f);
    } else if (a == 0x0e) {
        kpd_latch(&k->xr, v, 0x3f);
    } else if (a == 0x10) {
        kpd_latch(&k->ier, v, 0x3f);
    }
    kpd_update(k);
}

static uint64_t ctsi_read(void *opaque, hwaddr a, unsigned size)
{
    return a ? 0 : ((NgageKpd *)opaque)->ctsi;
}

static void ctsi_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    NgageKpd *k = opaque;

    if (!a) {
        k->ctsi &= ~v;
        kpd_update(k);
    }
}

static const MemoryRegionOps kpd_ops = {
    .read = kpd_read, .write = kpd_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static const MemoryRegionOps ctsi_ops = {
    .read = ctsi_read, .write = ctsi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static void ngage_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_keys); i++) {
        const NgageKey *key = &ngage_keys[i];

        if (key->host != evt->key.key) {
            continue;
        }
        qemu_log_mask(LOG_UNIMP, "ngage-key: %s %s\n", key->name, evt->key.down ? "down" : "up");
        if (key->row < 0) {
            return;                             /* power: UEM, not modelled */
        }
        if (evt->key.down) {
            kpd.pressed[key->row] |= 1 << key->col;
        } else {
            kpd.pressed[key->row] &= ~(1 << key->col);
        }
        kpd_update(&kpd);
        return;
    }
}

static const QemuInputHandler ngage_key_handler = {
    .name  = "ngage keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ngage_key_event,
};

void ngage_keys_init(MemoryRegion *sys, qemu_irq irq)
{
    QemuInputHandlerState *s;

    kpd.irq = irq;
    memory_region_init_io(&kpd.mr, NULL, &kpd_ops, &kpd, "ngage.keypad", 0x20);
    memory_region_add_subregion(sys, NGAGE_KPD_BASE, &kpd.mr);
    memory_region_init_io(&kpd.ctsi_mr, NULL, &ctsi_ops, &kpd, "ngage.ctsi", 0x4);
    memory_region_add_subregion(sys, NGAGE_CTSI_BASE, &kpd.ctsi_mr);
    s = qemu_input_handler_register(NULL, &ngage_key_handler);
    qemu_input_handler_activate(s);
}
