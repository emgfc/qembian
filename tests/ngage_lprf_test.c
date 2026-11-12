/*
 * Host unit test of the N-Gage Bluetooth module (src/qemu-ngage/hw/arm/ngage_lprf.c): btldd.ldd's
 * register protocol and the HCI replies, driven the way btldd and hci.dll drive them.
 *   cc -std=gnu11 -Wall -o build/ngage_lprf_test tests/ngage_lprf_test.c \
 *      src/qemu-ngage/hw/arm/ngage_lprf.c && build/ngage_lprf_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/qemu-ngage/hw/arm/ngage_lprf.h"

static int kicks;
static void kick(void *o) { (void)o; kicks++; }

static void setup(NgageLprf *s)
{
    memset(s, 0, sizeof(*s));
    s->kick = kick;
    ngage_lprf_init(s);
    kicks = 0;
}

/* what btldd's interrupt DFC sees: deliver, then read the status register */
static uint16_t irq(NgageLprf *s)
{
    ngage_lprf_deliver(s);
    return ngage_lprf_read(s, LPRF_REG_IRQ);
}

/* btldd states 5-8: start, data words (first byte high, odd byte alone), end */
static void host_write(NgageLprf *s, const uint8_t *p, unsigned n)
{
    ngage_lprf_write(s, LPRF_REG_WRITE, 0x8000 | n);
    for (unsigned i = 0; i < n; i += 2) {
        ngage_lprf_write(s, LPRF_REG_DATA, p[i] << 8 | (i + 1 < n ? p[i + 1] : 0));
    }
    ngage_lprf_write(s, LPRF_REG_WRITE, 0xc000 | n);
}

/* btldd's RX DFC: RXLEN, then words until RXLEN bytes */
static unsigned host_read(NgageLprf *s, uint8_t *out)
{
    unsigned n = ngage_lprf_read(s, LPRF_REG_RXLEN);

    for (unsigned i = 0; i < n; i += 2) {
        uint16_t w = ngage_lprf_read(s, LPRF_REG_DATA);
        out[i] = w >> 8;
        if (i + 1 < n) {
            out[i + 1] = w & 0xff;
        }
    }
    return n;
}

static void start(NgageLprf *s)
{
    ngage_lprf_write(s, LPRF_REG_IRQ, 0);
    ngage_lprf_write(s, LPRF_REG_IRQ, 1);
    assert(irq(s) & LPRF_IRQ_RESET);
    assert(ngage_lprf_read(s, LPRF_REG_RESET) & 8);        /* btldd 0x503f0b74: bit 3 or -36 */
    ngage_lprf_write(s, LPRF_REG_BUFREQ, 0x8000);
    assert(irq(s) == LPRF_IRQ_ACK);
    uint16_t a = ngage_lprf_read(s, LPRF_REG_WRITE);
    assert((a & 0xc000) == 0 && (a & 0x3fff) >= 520);     /* btldd 0x503f105c: no per-packet requests */
}

static unsigned command(NgageLprf *s, uint16_t op, const uint8_t *arg, unsigned alen, uint8_t *ev)
{
    uint8_t p[300] = { 0x01, 0x00, op & 0xff, op >> 8, alen };

    memcpy(p + 5, arg, alen);
    host_write(s, p, 5 + alen);
    uint16_t st = irq(s);
    assert(st & LPRF_IRQ_ACK);
    assert(ngage_lprf_read(s, LPRF_REG_WRITE) == (0x4000 | (5 + alen)));   /* btldd 0x503f10a4 */
    assert(st & LPRF_IRQ_RX);
    return host_read(s, ev);
}

/* Regression (btldd "CompleteWithError: 1 -33", Snakes' BTGetName -44): without a module the
 * driver waited for the reset indication forever. Start-up must answer it. */
static void test_btldd_timeout_without_module(void)
{
    NgageLprf s;

    setup(&s);
    ngage_lprf_write(&s, LPRF_REG_IRQ, 0);
    assert(kicks == 0 && irq(&s) == 0);                    /* nothing before control 1 */
    ngage_lprf_write(&s, LPRF_REG_IRQ, 1);
    assert(kicks == 1);
    assert(irq(&s) == LPRF_IRQ_RESET);
    assert(irq(&s) == 0);                                  /* read clears */
}

static void test_reset_command_complete(void)
{
    NgageLprf s;
    uint8_t ev[300];
    static const uint8_t want[] = { 0x04, 0x00, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };

    setup(&s);
    start(&s);
    unsigned n = command(&s, 0x0c03, NULL, 0, ev);      /* odd length (5): last byte alone */
    assert(n == sizeof(want) && !memcmp(ev, want, n));
    assert(ngage_lprf_read(&s, LPRF_REG_RXLEN) == 0);    /* queue empty */
}

static void test_read_local_name_and_bdaddr(void)
{
    NgageLprf s;
    uint8_t ev[300];

    setup(&s);
    start(&s);
    unsigned n = command(&s, 0x0c14, NULL, 0, ev);
    assert(n == 4 + 3 + 1 + 248);
    assert(ev[2] == 0x0e && ev[3] == 252 && ev[7] == 0);
    assert(!strcmp((char *)ev + 8, "N-Gage"));

    uint8_t name[] = "Gamer";
    command(&s, 0x0c13, name, sizeof(name), ev);          /* Write Local Name */
    command(&s, 0x0c14, NULL, 0, ev);
    assert(!strcmp((char *)ev + 8, "Gamer"));

    n = command(&s, 0x1009, NULL, 0, ev);
    assert(n == 4 + 3 + 7 && ev[7] == 0 && ev[13] == 0x00 && ev[12] == 0x02);
}

static void test_odd_length_event_is_exact(void)
{
    NgageLprf s;
    uint8_t ev[300];

    setup(&s);
    start(&s);
    /* Read Scan Enable: 4 + 3 + 2 = 9 bytes; hci.dll reads exact counts, no pad byte */
    unsigned n = command(&s, 0x0c19, NULL, 0, ev);
    assert(n == 9 && ev[3] == 5 && ev[8] == 0);
    uint8_t on = 3;
    command(&s, 0x0c1a, &on, 1, ev);
    command(&s, 0x0c19, NULL, 0, ev);
    assert(ev[8] == 3);
}

static void test_inquiry_status_then_complete(void)
{
    NgageLprf s;
    uint8_t ev[300];
    uint8_t inq[5] = { 0x33, 0x8b, 0x9e, 8, 0 };

    setup(&s);
    start(&s);
    unsigned n = command(&s, 0x0401, inq, 5, ev);
    assert(n == 8 && ev[2] == 0x0f && ev[4] == 0 && ev[6] == 0x01 && ev[7] == 0x04);
    /* the second event is announced when the first is read */
    assert(irq(&s) & LPRF_IRQ_RX);
    n = host_read(&s, ev);
    assert(n == 5 && ev[2] == 0x01 && ev[4] == 0);
}

static void test_unknown_command(void)
{
    NgageLprf s;
    uint8_t ev[300];

    setup(&s);
    start(&s);
    command(&s, 0x1400 | 0x55, NULL, 0, ev);             /* OGF 5, no parameters */
    assert(ev[2] == 0x0e && ev[7] == 0x01 && s.unknown == 1);
    command(&s, 0xfc00 | 0x12, NULL, 0, ev);             /* vendor: accepted */
    assert(ev[7] == 0x00);
}

static void test_rx_queue_full(void)
{
    NgageLprf s;
    uint8_t ev[300];

    setup(&s);
    start(&s);
    for (int i = 0; i < LPRF_RXQ + 4; i++) {             /* nobody reads: replies pile up */
        uint8_t p[5] = { 1, 0, 0x03, 0x0c, 0 };
        host_write(&s, p, 5);
    }
    assert(s.rx_count == LPRF_RXQ);
    for (int i = 0; i < LPRF_RXQ; i++) {
        assert(host_read(&s, ev) == 8);
    }
    assert(ngage_lprf_read(&s, LPRF_REG_RXLEN) == 0);
}

static void test_write_length_mismatch_and_short_packets(void)
{
    NgageLprf s;
    uint8_t ev[300];

    setup(&s);
    start(&s);
    /* end says 7, only 5 written: acknowledged as written by the driver's count, no crash */
    ngage_lprf_write(&s, LPRF_REG_WRITE, 0x8005);
    ngage_lprf_write(&s, LPRF_REG_DATA, 0x0100);
    ngage_lprf_write(&s, LPRF_REG_DATA, 0x030c);
    ngage_lprf_write(&s, LPRF_REG_DATA, 0x0000);
    ngage_lprf_write(&s, LPRF_REG_DATA, 0xffff);         /* beyond the start length: ignored */
    ngage_lprf_write(&s, LPRF_REG_WRITE, 0xc005);
    assert(irq(&s) & LPRF_IRQ_RX);
    assert(host_read(&s, ev) == 8 && ev[5] == 0x03);
    /* a 1-byte and an ACL packet: dropped, no reply */
    uint8_t one[1] = { 1 }, acl[6] = { 2, 0, 1, 0, 0, 0 };
    host_write(&s, one, 1);
    host_write(&s, acl, 6);
    assert(s.rx_count == 0);
    /* RXLEN / DATA with nothing queued */
    assert(ngage_lprf_read(&s, LPRF_REG_RXLEN) == 0 && ngage_lprf_read(&s, LPRF_REG_DATA) == 0);
}

/* BTTestServer writes a 0xFFFF connection accept timeout at boot: outside the spec range
 * (0x0001..0xB540), so refused with Invalid HCI Command Parameters as a real controller does;
 * in-range values and the page timeout's full range are accepted. */
static void test_bttestserver_accept_timeout_out_of_range(void)
{
    NgageLprf s;
    uint8_t ev[300];
    uint8_t ffff[2] = { 0xff, 0xff }, ok[2] = { 0xa0, 0x1f }, max[2] = { 0x40, 0xb5 }, zero[2] = { 0, 0 };

    setup(&s);
    start(&s);
    command(&s, 0x0c16, ffff, 2, ev);
    assert(ev[2] == 0x0e && ev[5] == 0x16 && ev[6] == 0x0c && ev[7] == 0x12);
    command(&s, 0x0c16, ok, 2, ev);
    assert(ev[7] == 0);
    command(&s, 0x0c16, max, 2, ev);
    assert(ev[7] == 0);
    command(&s, 0x0c16, zero, 2, ev);
    assert(ev[7] == 0x12);
    command(&s, 0x0c18, ffff, 2, ev);                     /* page timeout: 0xFFFF is valid */
    assert(ev[7] == 0);
    command(&s, 0x0c18, zero, 2, ev);
    assert(ev[7] == 0x12);
}

int main(void)
{
    test_bttestserver_accept_timeout_out_of_range();
    test_btldd_timeout_without_module();
    test_reset_command_complete();
    test_read_local_name_and_bdaddr();
    test_odd_length_event_is_exact();
    test_inquiry_status_then_complete();
    test_unknown_command();
    test_rx_queue_full();
    test_write_length_mismatch_and_short_packets();
    printf("ngage_lprf_test: all passed\n");
    return 0;
}
