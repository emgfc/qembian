/*
 * Nokia N-Gage QD (RH-29, DCT4 "UPP_WD2", Symbian 6.1 / EKA1) bring-up board.
 *
 * Runs the phone's own ROM (bootstrap, ekern.exe, ecust.dll): see docs/eka1-ngage-roadmap.md.
 * This is the discovery stage (roadmap P1): only what the ROM is known to need is modelled,
 * every other physical access goes to a logging catch-all that remembers what was written
 * (reads return the last value, else 0), so polling loops and probes show up in the log:
 *   qemu-system-arm -M ngage -bios ROM.bin -d unimp,guest_errors -D ngage.log
 *
 * The memory map and each modelled device answer a question in docs/eka1-ngage-questions.md:
 *   0x00000000  flash chip 0 (Intel CFI, x16), ROM.bin offsets 0..0xEFFFFF     Q1 Q4 Q12
 *   0x02000000  flash chip 1 (Intel CFI, x16), ROM.bin from offset 0xF00000    Q2 Q12
 *   0x0A000000  SDRAM, 16 MB                                                   Q3
 *   0x0C120100  interrupt controller, 4 banks x 32                              Q16
 *   0x0C120A00  timers 0-5 (0x20 apart), interrupt sources 0-5                 Q13 Q31
 *   0x0C120300  DMA engine (7 channels), 0x0C120600 MMC host (ngage_mmc.c, -M ngage,mmc=CARD.img)
 *   0x0C1F0000  NOS stand-in device (board-only, H-nos-standin)                 Q20 Q21
 *   0x0DE00000  kernel debug output (trace port), to serial 0                 Q18
 *   0x0C150420  CBus master to the external chips (device 1: power ASIC)       Q11
 *   0x0C000000  UPP peripherals and internal SRAM (0x0C004000), virtual 0x58000000: catch-all
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/block/flash.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/datadir.h"
#include "system/address-spaces.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "ngage_mmc.h"
#include "accel/tcg/cpu-loop.h"
#include "tcg/insn-start-words.h"
#include "exec/target_page.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "exec/cpu-common.h"
#include "qemu/host-utils.h"

/* Answers behind each value: docs/eka1-ngage-questions.md */
#define NGAGE_FLASH_PARTITION 0x00200000  /* read-while-write partition, 16 Mbit (Q48) [I] */
#define NGAGE_FLASH0_BASE   0x00000000      /* boot flash, holds ROM.bin (Q1, Q4) */
#define NGAGE_FLASH1_BASE   0x02000000      /* second chip (Q2); chip n is at n << 25 (Q4) */
#define NGAGE_FLASH_SIZE    (32 * MiB)      /* [I] the chip stride (Q5) */
#define NGAGE_FLASH_SECTOR  (64 * KiB)      /* [V] medint.pdd steps blocks by 0x10000 (Q27);
                                                the top 8 KB parameter blocks are not modelled */
#define NGAGE_FLASH_MANUF   0x89            /* Intel; accepted by 0x599c and 0x97c (Q2, Q4) */
#define NGAGE_FLASH_DEVICE  0x8874          /* bootstrap type 8 (Q5) */
#define NGAGE_ROM_SPLIT     0x00f00000      /* ROM.bin offset held at the start of chip 1 (Q12) */
#define NGAGE_SDRAM_BASE    0x0a000000      /* Q3 */

/* Guest PC of the instruction doing the current MMIO access (0 if unknown), for the logs. */
static uint32_t ngage_pc(void)
{
    uint64_t data[INSN_START_WORDS] = { 0 };

    if (current_cpu && current_cpu->mem_io_pc &&
        cpu_unwind_state_data(current_cpu, current_cpu->mem_io_pc, data)) {
        /*
         * ARM TBs are PC-relative (CF_PCREL): the unwind data holds the page offset, and
         * regs[15] still holds the TB's page. Log-only: a TB crossing a page can be one off.
         */
        uint32_t off = qemu_target_page_size() - 1;    /* 1 KB pages on ARMv4/v5 */
        return (ARM_CPU(current_cpu)->env.regs[15] & ~off) | ((uint32_t)data[0] & off);
    }
    return 0;
}

static uint32_t ngage_lr(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[14] : 0;
}

/*
 * Hot-path logging (Q56): the interrupt controller, timers, CBUS and NOS calls are logged for
 * their first NGAGE_LOG_HOT accesses per register (or slot/method), then not at all, unless
 * NGAGE_VERBOSE is set. Logging every access (pc lookup + a line each) took most of the host
 * time: 28 MB of log a minute.
 */
#define NGAGE_LOG_HOT 32

static bool ngage_log_hot(uint16_t *count)
{
    static int verbose = -1;

    if (verbose < 0) {
        verbose = getenv("NGAGE_VERBOSE") != NULL;
    }
    if (verbose) {
        return true;
    }
    if (*count >= NGAGE_LOG_HOT) {
        return false;
    }
    (*count)++;
    return true;
}

/* ---- logging catch-all ------------------------------------------------- */

typedef struct {
    MemoryRegion mr;
    GHashTable *mem;        /* word address -> last written value */
    GHashTable *seen;       /* word address -> number of accesses logged */
    uint64_t total;
} NgageUnknown;

#define NGAGE_LOG_PER_ADDR 16

static void ngage_unknown_log(NgageUnknown *u, hwaddr addr, bool wr,
                              uint64_t val, unsigned size)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(u->seen, k));

    u->total++;
    if (n < NGAGE_LOG_PER_ADDR) {
        g_hash_table_insert(u->seen, k, GUINT_TO_POINTER(n + 1));
        qemu_log_mask(LOG_UNIMP, "ngage: %s %08" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x lr=%08x%s\n",
                      wr ? "W" : "R", addr, size, val, ngage_pc(), ngage_lr(),
                      n + 1 == NGAGE_LOG_PER_ADDR ? " (further accesses not logged)" : "");
    }
}

/*
 * Registers whose read behaviour is answered (docs/eka1-ngage-questions.md) but that have no
 * device model yet: a read returns the last written word with 'set' bits forced on.
 */
static const struct {
    uint32_t addr;
    uint32_t set;
    const char *why;
} ngage_known[] = {
    { 0x0c120020, 0x0c000000, "Q9: clock status; bit 26 after 0xC, bit 27 after 3 to +4: PLL locked [I]" },
    { 0x0c149000, 0x00000001, "Q10: written 0x2210, bootstrap 0x37fc waits for bit 0: ready/ack [I]" },
};

/*
 * H-ngage-startup (Q51; NGAGE_STARTCODE=N overrides, -1 = off): the start-up reason hand-over
 * the NOS (or the hardware) makes [I]. Default 1 = a normal power-on. ecust's start-up mode getter (0x503439ac) takes the "reset" path when 0x0C150000
 * bits 1-3 are set and maps a code from internal SRAM (0x0C00420E when 0x420C/0x4208 are 0)
 * through 0x50343880: 1 -> mode 0 = Starter's normal list (keys 0,100) [V]. Cold (bits 0) it
 * can only give 4 or 9 [V]. Applied once the NOS stand-in ran, so the bootstrap still sees a
 * cold start.
 */
static bool ngage_nos_entered;

static uint32_t ngage_startup_handover(hwaddr word, uint32_t w)
{
    static int code = -2;

    if (code == -2) {
        code = getenv("NGAGE_STARTCODE") ? (int)strtol(getenv("NGAGE_STARTCODE"), NULL, 0) : 1;
    }
    if (code < 0 || !ngage_nos_entered) {
        return w;
    }
    if (word == 0x0c150000) {
        w |= 0x0002;
    } else if (word == 0x0c00420c) {
        w = (w & ~0x00ff0000u) | (uint32_t)(code & 0xff) << 16;
    }
    return w;
}

static uint64_t ngage_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageUnknown *u = opaque;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem,
                                  GUINT_TO_POINTER((guint)(addr & ~3u))));

    w = ngage_startup_handover(addr & ~3u, w);
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_known); i++) {
        if (ngage_known[i].addr == (addr & ~3u)) {
            w |= ngage_known[i].set;
        }
    }
    uint64_t v = (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);

    ngage_unknown_log(u, addr, false, v, size);
    return v;
}

static void ngage_unknown_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageUnknown *u = opaque;
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, k));
    unsigned sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    g_hash_table_insert(u->mem, k, GUINT_TO_POINTER(w));
    ngage_unknown_log(u, addr, true, val, size);
}

static const MemoryRegionOps ngage_unknown_ops = {
    .read = ngage_unknown_read,
    .write = ngage_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- CBus master to the external chips (Q11) ----------------------------- */
/*
 * Register interface [V] (bootstrap 0x35ac/0x3634, cbus.dll 0x5037d2fc via ekern's 16-bit
 * accessor 0x5031e708):
 *   +0x20  transfer format (0x0F: 16-bit data, 0x07: 8-bit; other high bits: Q11)
 *   +0x22  command: bits 0-4 register, bits 5-7 device, 0x100 = read, 0x200 = flag (Q11);
 *          reads back with bit 10 set = done (the drivers poll it)
 *   +0x24  data, high half (transfers longer than 16 bits)
 *   +0x26  data, low half; a write command commits on this store (written after +0x22)
 * The device/register split is the Nokia CBus format (3-bit device, 5-bit register; Linux
 * drivers/cbus for Retu/Tahvo). Model: a register file per (device, register), status registers
 * from ngage_cbus_status, every transaction logged ("ngage-cbus:").
 */
#define NGAGE_CBUS_BASE 0x0c150420

typedef struct {
    MemoryRegion mr;
    uint16_t fmt, cmd, hi, lo;
    bool pending;
    uint32_t regs[256];
    bool written[256];
} NgageCbus;

/* Status registers: a read returns this value whatever was written. */
static const struct {
    uint8_t dev, reg;
    uint16_t val;
    const char *why;
} ngage_cbus_status[] = {
    { 1, 0x15, 0x0010, "Q14/Q39: power-on reason; bootstrap 0x50002e6c stays on for bit 3 or 4; "
                       "the variant's start-up mode decoder (0x50343b64) gives mode 4 for bit 4, "
                       "else 9, and Starter then starts only its minimal list: bit 4 = power key "
                       "[I]" },
    { 4, 0x00, 0x0400, "Q15: ASIC id; cbus.dll 0x5037e064 requires (val & 0xff00) == 0x0400 "
                       "or faults 'CBus 25'" },
    { 1, 0x1a, 480, "Q35: ADC result (conversion started by reg 0x19). cbus.dll 0x5037e270 "
                    "needs 380..590 (else start-up mode 9: the variant resets); the bootstrap "
                    "powers off above 0x3DD and skips its delay for 5..93. 480 [I]" },
};

static uint64_t ngage_cbus_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageCbus *c = opaque;

    switch (addr) {
    case 0x0: return c->fmt;
    case 0x2: return c->cmd | 0x400;
    case 0x4: return c->hi;
    case 0x6: return c->lo;
    }
    return 0;
}

static void ngage_cbus_commit_write(NgageCbus *c)
{
    unsigned r = c->cmd & 0xff;

    static uint16_t count[256];

    c->regs[r] = (uint32_t)c->hi << 16 | c->lo;
    c->written[r] = true;
    if (ngage_log_hot(&count[r]))
    qemu_log_mask(LOG_UNIMP, "ngage-cbus: W dev %u reg %02x = %04x%s (fmt %02x cmd %03x) pc=%08x\n",
                  r >> 5, r & 0x1f, c->lo, c->hi ? " hi" : "", c->fmt, c->cmd, ngage_pc());
}

static void ngage_cbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageCbus *c = opaque;
    unsigned r;

    switch (addr) {
    case 0x0:
        c->fmt = val;
        return;
    case 0x4:
        c->hi = val;
        return;
    case 0x6:
        c->lo = val;
        if (c->pending) {
            ngage_cbus_commit_write(c);
            c->pending = false;
        }
        return;
    case 0x2:
        c->cmd = val & 0x3ff;
        r = val & 0xff;
        if (val & 0x100) {
            const char *src = c->written[r] ? "" : " (never written)";
            uint32_t v = c->regs[r];

            for (unsigned i = 0; i < ARRAY_SIZE(ngage_cbus_status); i++) {
                if ((ngage_cbus_status[i].dev << 5 | ngage_cbus_status[i].reg) == r) {
                    v = ngage_cbus_status[i].val;
                    src = " (declared status)";
                }
            }
            if (r == (1 << 5 | 0x15) && getenv("NGAGE_POWERON")) {   /* experiments (Q51) */
                v = strtoul(getenv("NGAGE_POWERON"), NULL, 0);
                src = " (NGAGE_POWERON)";
            }
            c->lo = v;
            c->hi = v >> 16;
            static uint16_t rcount[256];
            if (ngage_log_hot(&rcount[r]))
            qemu_log_mask(LOG_UNIMP, "ngage-cbus: R dev %u reg %02x -> %04x%s (fmt %02x cmd %03x) pc=%08x\n",
                          r >> 5, r & 0x1f, c->lo, src, c->fmt, c->cmd, ngage_pc());
            c->pending = false;
        } else {
            c->pending = true;
        }
        return;
    }
}

static const MemoryRegionOps ngage_cbus_ops = {
    .read = ngage_cbus_read,
    .write = ngage_cbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

/* ---- interrupt controller (Q16) ---------------------------------------- */
/*
 * 0x0C120100, four banks of 32 sources, 0x40 apart [V: ekern 0x5031e874 / 0x5031e8cc /
 * 0x5031e798 / 0x5031e868, dispatcher 0x5031c87c]:
 *   +08  write 1<<n: enable source n (the bootstrap and kernel enable the timer as bit 0)
 *   +0C  write 1<<n: disable source n [I]. Not an acknowledge: the bootstrap's timer handler
 *        writes it and leaves the timer status set; the kernel clears that status and
 *        re-enables source 0. Any other reading kills the kernel on the leftover status.
 *   +10  pending bits (ekern's "is pending" helper reads it)
 *   +14  number of the source to service; 0 = none. [I] 1-based: source 0 (the timer) is used.
 * Banks 0-2 drive IRQ (the dispatcher reads +14 of each); bank 3 drives FIQ [I] (the other
 * dispatcher reads 0x0C1201D4). Sources are level-sensitive [V for source 0: the kernel's
 * timer handler 0x5031c5b0 clears the timer's own status and never writes +0C].
 * Unknown offsets store and log.
 */
#define NGAGE_INTC_BASE 0x0c120100

typedef struct {
    MemoryRegion mr;
    qemu_irq irq, fiq;
    uint32_t enable[4], pending[4], level[4];
    uint32_t other[4][16];
} NgageIntc;

static void ngage_intc_update(NgageIntc *c)
{
    bool irq = false;

    for (int b = 0; b < 3; b++) {
        irq |= (c->pending[b] & c->enable[b]) != 0;
    }
    qemu_set_irq(c->irq, irq);
    qemu_set_irq(c->fiq, (c->pending[3] & c->enable[3]) != 0);
}

static void ngage_intc_set(void *opaque, int n, int level)
{
    NgageIntc *c = opaque;
    uint32_t bit = 1u << (n & 31);
    int b = n >> 5;

    c->level[b] = level ? c->level[b] | bit : c->level[b] & ~bit;
    c->pending[b] = level ? c->pending[b] | bit : c->pending[b] & ~bit;
    if (b == 0) {
        /*
         * Bank 3 is the FIQ side of sources 0-31 (Q46) [I: medint.pdd runs timer 3 and
         * enables bank 3 bit 3; the kernel enables bank 3 bit 27]: the same source
         * pending bits, its own enable mask.
         */
        c->level[3] = c->level[0];
        c->pending[3] = c->pending[0];
    }
    ngage_intc_update(c);
}

static uint64_t ngage_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageIntc *c = opaque;
    int b = addr >> 6, r = addr & 0x3f;
    uint32_t v;

    switch (r) {
    case 0x08: v = c->enable[b]; break;
    case 0x10: v = c->pending[b]; break;
    case 0x14: {
        uint32_t act = c->pending[b] & c->enable[b];
        v = act ? ctz32(act) + 1 : 0;
        return v;                       /* polled by the dispatchers: not logged */
    }
    default: v = c->other[b][r >> 2]; break;
    }
    static uint16_t rcount[4][16];
    if (ngage_log_hot(&rcount[b & 3][(r >> 2) & 15]))
    qemu_log_mask(LOG_UNIMP, "ngage-intc: R bank %d +%02x -> %08x pc=%08x lr=%08x\n", b, r, v,
                  ngage_pc(), ngage_lr());
    return v;
}

static void ngage_intc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageIntc *c = opaque;
    int b = addr >> 6, r = addr & 0x3f;
    static uint16_t wcount[4][16];

    if (ngage_log_hot(&wcount[b & 3][(r >> 2) & 15]))
    qemu_log_mask(LOG_UNIMP, "ngage-intc: W bank %d +%02x = %08x pc=%08x lr=%08x\n", b, r,
                  (unsigned)val, ngage_pc(), ngage_lr());
    switch (r) {
    case 0x08: c->enable[b] |= val; break;
    case 0x0c: c->enable[b] &= ~(uint32_t)val; break;
    default: c->other[b][r >> 2] = val; break;
    }
    ngage_intc_update(c);
}

static const MemoryRegionOps ngage_intc_ops = {
    .read = ngage_intc_read, .write = ngage_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- timers at 0x0C120A00 + 0x20 * i, interrupt sources 0-5 (Q13, Q17, Q31) */
/*
 * Registers [V: kernel helpers 0x5031ea58 (+04 mode, written and read back), eac4 (+18),
 * eae8 (+10), eafc (+14), eb14 (+08), eadc (read +10); timer 0 = the tick (0x5031c718),
 * timer 1 = the one-shot timer queue (object 0x80000558, methods 0x5031ca64-0x5031cb80)]:
 *   +04 mode: 0 stopped (the remaining count is kept), 1 one-shot down-counter, 2 periodic (reload from
 *       +18); a running mode written while stopped resumes from +10
 *   +08 interrupt status: set when the count expires; handlers write it (tick: 0, timer 1
 *       and the bootstrap: 1), which acknowledges; the bootstrap then polls it for != 0
 *   +10 count: writing (re)starts the countdown from the value; reading returns the live
 *       remaining count (timer 1's AddDelta reads it and writes count + delta)
 *   +18 reload (periodic)
 * Interrupt source i (name table IrqTimer0-5, Q30) is raised on expiry, lowered by a write to
 * +08 or +10 or by mode 0.
 * Clocks [V for timer 0: its period 0x843E2 is the kernel's 15625 us tick (0x3D09, export
 * 466) -> 34.67 MHz; I for the others: timer 1's ms-to-ticks factor is 2311 = 34.67 MHz/15].
 * Timer 3 runs at 34.67 MHz too [I, Q54]: medint.pdd programs flash one halfword per timer-3 FIQ
 * period of 0x1FE ticks, i.e. 14.7 us (a typical Intel word-program time); at /15 every flash
 * write took 15 times longer and Phone's profile commit outlasted ViewSrv's timeout.
 */
#define NGAGE_TIMER_BASE   0x0c120a00
#define NGAGE_TIMERS       6
#define NGAGE_TIMER0_HZ    34666667        /* 13 MHz * 8 / 3 */
#define NGAGE_TIMERN_HZ    (NGAGE_TIMER0_HZ / 15)

typedef struct NgageTimers NgageTimers;
typedef struct {
    NgageTimers *owner;
    int index;
    QEMUTimer *t;
    qemu_irq irq;
    uint32_t regs[8];
    int64_t start_ns;           /* when the current countdown started */
    uint32_t start_count;
} NgageTimer;

struct NgageTimers {
    MemoryRegion mr;
    NgageTimer t[NGAGE_TIMERS];
};

static uint64_t ngage_timer_hz(NgageTimer *s)
{
    return (s->index == 0 || s->index == 3) ? NGAGE_TIMER0_HZ : NGAGE_TIMERN_HZ;
}

static void ngage_timer_arm(NgageTimer *s, uint32_t ticks)
{
    s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->start_count = ticks;
    timer_mod(s->t, s->start_ns + muldiv64(ticks, NANOSECONDS_PER_SECOND, ngage_timer_hz(s)));
}

static uint32_t ngage_timer_count(NgageTimer *s)
{
    uint64_t gone;

    if (!s->regs[0x04 >> 2] || !timer_pending(s->t)) {
        return s->regs[0x10 >> 2];
    }
    gone = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns, ngage_timer_hz(s),
                    NANOSECONDS_PER_SECOND);
    return gone >= s->start_count ? 0 : s->start_count - gone;
}

static void ngage_timer_expire(void *opaque)
{
    NgageTimer *s = opaque;

    s->regs[0x08 >> 2] |= 1;
    s->regs[0x10 >> 2] = 0;
    qemu_set_irq(s->irq, 1);
    if (s->regs[0x04 >> 2] == 2 && s->regs[0x18 >> 2]) {
        ngage_timer_arm(s, s->regs[0x18 >> 2]);
    }
}

static uint64_t ngage_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageTimers *ts = opaque;
    NgageTimer *s = &ts->t[(addr >> 5) % NGAGE_TIMERS];
    unsigned r = addr & 0x1f;

    if (r == 0x10) {
        return ngage_timer_count(s);
    }
    return s->regs[r >> 2];
}

static void ngage_timer_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageTimers *ts = opaque;
    NgageTimer *s = &ts->t[(addr >> 5) % NGAGE_TIMERS];
    unsigned r = addr & 0x1f;

    static uint16_t tcount[NGAGE_TIMERS][8];

    if (r != 0x08 && !(s->index == 1 && r == 0x10) &&   /* per-tick traffic: not logged */
        ngage_log_hot(&tcount[s->index][(r >> 2) & 7])) {
        qemu_log_mask(LOG_UNIMP, "ngage-timer%d: W +%02x = %08x pc=%08x lr=%08x\n", s->index,
                      r, (unsigned)val, ngage_pc(), ngage_lr());
    }
    uint32_t live = ngage_timer_count(s);     /* the remaining count before this write */
    s->regs[r >> 2] = val;
    switch (r) {
    case 0x04:
        /* mode 0 stops the countdown and keeps the remaining count; a running mode written while stopped
           resumes from the count register. The kernel's one-shot queue (timer 1) toggles 0/1 around its
           updates: when only +10 restarted the countdown, a stop-start without a new count left timer 1
           dead, and with it every one-shot kernel timer (the keypad's scan stalled mid-scan: no key worked
           a minute after boot, docs/eka1-ngage.md Next 1) */
        if (!val) {
            if (timer_pending(s->t)) {
                s->regs[0x10 >> 2] = live;
            }
            timer_del(s->t);
            qemu_set_irq(s->irq, 0);
        } else if (!timer_pending(s->t) && s->regs[0x10 >> 2]) {
            ngage_timer_arm(s, s->regs[0x10 >> 2]);
        }
        break;
    case 0x08:
        qemu_set_irq(s->irq, 0);
        break;
    case 0x10:                  /* (re)start the countdown; written after the mode [V] */
        qemu_set_irq(s->irq, 0);
        /* medint.pdd starts timer 3 by writing the count without a mode (Q46): one-shot */
        if (val) {
            ngage_timer_arm(s, val);
        } else {
            timer_del(s->t);
        }
        break;
    }
}

static const MemoryRegionOps ngage_timer_ops = {
    .read = ngage_timer_read, .write = ngage_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- LCD interface at 0x0C121200 (Q37) ----------------------------------- */
/*
 * [V] lcd.ldd accessors and the bootstrap (pc 0x6658/0x66e0): +0x00 control word,
 * +0x04 control (2, 1) / status (bit 2 = ready, polled before each data word), +0x08 data
 * (panel commands such as 0x11 sleep-out and 0x04 read-ID; reads return panel answers).
 * Discovery model: always ready; every control and data access logged ("ngage-lcd:") up to
 * a limit, so the panel protocol can be decoded (P4).
 */
#define NGAGE_LCDIF_BASE 0x0c121200

/*
 * Control word +0x00 [V: ecust/lcd.ldd SendCommand 0x5034580c, ReadID 0x50344f54]:
 *   0x09280000           command phase: the next data word is a DCS command byte
 *   0x09240000 | (n - 1) parameter/data phase, n bytes
 *   0x07840000 | n       read n bytes (the first is a dummy cycle)
 *   0x49200000, 0x4f300000: idle / configuration [I]
 * The panel is a DCS controller [I: commands 0x11, 0x04, 0x09, 0x36, 0x3A, 0x2x seen in the
 * drivers]. ecust identifies it by ID1: 0x29 -> type 16, 0x83 -> type 4, else unknown
 * (0x50345470) [V]; this board answers 0x83 [I].
 */
#define NGAGE_LCD_ID1 0x83

#define NGAGE_LCD_W 176             /* ROM header screen size, 12 bpp (Q37) */
#define NGAGE_LCD_H 208

typedef struct {
    MemoryRegion mr;
    QemuConsole *con;
    uint16_t fb[NGAGE_LCD_H][NGAGE_LCD_W];      /* RGB444 */
    bool dirty;
    unsigned x0, x1, y0, y1, x, y;              /* window (0x2A/0x2B) and cursor */
    uint8_t pend[3];                            /* byte-mode pixel data in progress */
    unsigned npend;
    uint32_t ctl, ctl2, data;
    uint64_t nlog, ndata;
    uint8_t cmd;                /* last DCS command */
    unsigned nparam;            /* parameter bytes received since the command */
    uint8_t params[8];
    uint8_t rd[8];              /* read response */
    unsigned rdlen, rdpos;
    uint64_t pixbytes;          /* bytes after RAMWR (0x2C) */
    uint64_t panel_frames;      /* whole frames of pixels from the OS side (not the bootstrap) */
    bool os_ramwr;              /* the current RAMWR was issued by lcd.ldd */
    bool fb_logged;
} NgageLcdIf;

#define NGAGE_LCD_LOG_MAX 4000

static void ngage_lcd_cmd_done(NgageLcdIf *l)
{
    if (l->cmd == 0x2c) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: RAMWR: %" PRIu64 " pixel bytes\n", l->pixbytes);
    } else if (l->nparam && l->nlog++ < NGAGE_LCD_LOG_MAX) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: cmd %02x params %02x %02x %02x %02x (%u)\n", l->cmd,
                      l->params[0], l->params[1], l->params[2], l->params[3], l->nparam);
    }
}

/* one 12-bit pixel at the cursor, then advance within the window */
static void ngage_lcd_pixel(NgageLcdIf *l, uint16_t rgb444)
{
    if (l->x < NGAGE_LCD_W && l->y < NGAGE_LCD_H) {
        l->fb[l->y][l->x] = rgb444;
        l->dirty = true;
    }
    if (++l->x > l->x1) {
        l->x = l->x0;
        if (++l->y > l->y1) {
            l->y = l->y0;
        }
    }
}

/* DCS 12 bpp (COLMOD 3): 3 bytes = 2 pixels, R1G1 B1R2 G2B2 */
static void ngage_lcd_pixels3(NgageLcdIf *l, uint8_t b0, uint8_t b1, uint8_t b2)
{
    ngage_lcd_pixel(l, (b0 << 4) | (b1 >> 4));
    ngage_lcd_pixel(l, ((b1 & 0xf) << 8) | b2);
}

static void ngage_lcd_params_done(NgageLcdIf *l)
{
    if (l->cmd == 0x2a && l->nparam >= 2) {
        l->x0 = MIN(l->params[0], NGAGE_LCD_W - 1);
        l->x1 = MIN(l->params[1], NGAGE_LCD_W - 1);
    } else if (l->cmd == 0x2b && l->nparam >= 2) {
        l->y0 = MIN(l->params[0], NGAGE_LCD_H - 1);
        l->y1 = MIN(l->params[1], NGAGE_LCD_H - 1);
    }
}

static void ngage_lcd_command(NgageLcdIf *l, uint8_t c)
{
    ngage_lcd_params_done(l);
    ngage_lcd_cmd_done(l);
    l->cmd = c;
    l->nparam = 0;
    l->pixbytes = 0;
    memset(l->params, 0, sizeof(l->params));
    l->rdlen = l->rdpos = 0;
    switch (c) {
    case 0x04:                  /* read display ID: dummy, ID1, ID2, ID3 */
        l->rd[0] = 0; l->rd[1] = NGAGE_LCD_ID1; l->rd[2] = 0; l->rd[3] = 0; l->rdlen = 4;
        break;
    case 0x09:                  /* read display status: dummy + 4 bytes */
        memset(l->rd, 0, 5); l->rdlen = 5;
        break;
    case 0x2c:                  /* memory write: from the window's start */
        l->os_ramwr = ngage_lr() >= 0x50000000;      /* lcd.ldd, not the bootstrap */
        l->x = l->x0;
        l->y = l->y0;
        l->npend = 0;
        break;
    }
    if (l->nlog++ < NGAGE_LCD_LOG_MAX) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: cmd %02x lr=%08x\n", c, ngage_lr());
    }
}

static uint64_t ngage_lcdif_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageLcdIf *l = opaque;

    switch (addr) {
    case 0: return l->ctl;
    case 4: return l->ctl2 | 4;                 /* bit 2: ready */
    case 8:
        return l->rdpos < l->rdlen ? l->rd[l->rdpos++] : 0;
    }
    return 0;
}

static void ngage_lcdif_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageLcdIf *l = opaque;

    switch (addr) {
    case 0: l->ctl = val; return;
    case 4: l->ctl2 = val; return;
    case 8:
        l->ndata++;
        if (l->ctl & 0x80000) {                 /* command phase */
            ngage_lcd_command(l, val & 0xff);
        } else if (l->cmd == 0x2c) {
            /* control bit 31: 24-bit words, 2 pixels each [V: 54912 bytes in 18304 words] */
            if (l->ctl & 0x80000000) {
                ngage_lcd_pixels3(l, val >> 16, val >> 8, val);
            } else {
                l->pend[l->npend++] = val;
                if (l->npend == 3) {
                    ngage_lcd_pixels3(l, l->pend[0], l->pend[1], l->pend[2]);
                    l->npend = 0;
                }
            }
            l->pixbytes++;
            if (l->os_ramwr && l->pixbytes == NGAGE_LCD_W * NGAGE_LCD_H / 2) {
                /* lcd.ldd delivered a whole frame itself: H-ngage-fb no longer needed */
                l->panel_frames++;
            }
        } else if (l->nparam < sizeof(l->params)) {
            l->params[l->nparam++] = val;
        }
        return;
    }
}

/*
 * H-ngage-fb (Q38): the window server draws into the screen chunk, whose address
 * Plat::ScreenInfo (ekern export 336) computes as [[0x800066EC] + 24]; lcd.ldd copies it to
 * the panel (by DMA, not modelled yet). Until lcd.ldd's own transfers reach the panel model,
 * the display shows that RAM directly, read through the MMU: a 32-byte header, then
 * EColor4K pixels, 16 bits each (0x0RGB) [V: the buffer reads 0x0FFF white after the header;
 * chunk object +20 physical base 0x0A000000, +24 virtual base, +28 size 0x12000]. Off while the pointer is not set yet (the bootstrap's panel picture shows).
 */
#define NGAGE_SCREEN_CHUNK_PTR 0x800066ec
#define NGAGE_SCREEN_HDR       32          /* [V] header (first halfword 0x2000), then pixels */

static bool ngage_lcd_from_screen_chunk(NgageLcdIf *l)
{
    uint32_t chunk = 0, base = 0;
    static uint16_t line[NGAGE_LCD_W];
    CPUState *cs = first_cpu;

    if (!cs || cpu_memory_rw_debug(cs, NGAGE_SCREEN_CHUNK_PTR, &chunk, 4, false) || !chunk ||
        cpu_memory_rw_debug(cs, chunk + 24, &base, 4, false) || !base) {
        return false;
    }
    for (int y = 0; y < NGAGE_LCD_H; y++) {
        if (cpu_memory_rw_debug(cs, base + NGAGE_SCREEN_HDR + y * NGAGE_LCD_W * 2, line,
                                sizeof(line), false)) {
            return false;
        }
        for (int x = 0; x < NGAGE_LCD_W; x++) {
            l->fb[y][x] = le16_to_cpu(line[x]) & 0xfff;
        }
    }
    if (!l->fb_logged) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: H-ngage-fb: showing the screen chunk at %08x "
                      "(chunk object %08x)\n", base, chunk);
        l->fb_logged = true;
    }
    return true;
}

static bool ngage_lcd_gfx_update(void *opaque)
{
    NgageLcdIf *l = opaque;
    DisplaySurface *surf = qemu_console_surface(l->con);
    uint8_t *dst;
    int stride;

    if (!l->panel_frames && ngage_lcd_from_screen_chunk(l)) {
        l->dirty = true;
    }
    if (!l->dirty) {
        return true;
    }
    if (surface_width(surf) != NGAGE_LCD_W || surface_height(surf) != NGAGE_LCD_H) {
        qemu_console_resize(l->con, NGAGE_LCD_W, NGAGE_LCD_H);
        surf = qemu_console_surface(l->con);
    }
    dst = surface_data(surf);
    stride = surface_stride(surf);
    for (int y = 0; y < NGAGE_LCD_H; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < NGAGE_LCD_W; x++) {
            uint16_t p = l->fb[y][x];
            uint32_t r = (p >> 8) & 0xf, g = (p >> 4) & 0xf, b = p & 0xf;
            row[x] = (r * 0x11) << 16 | (g * 0x11) << 8 | (b * 0x11);
        }
    }
    l->dirty = false;
    qemu_console_update_full(l->con);
    return true;
}

static void ngage_lcd_invalidate(void *opaque)
{
    ((NgageLcdIf *)opaque)->dirty = true;
}

static const GraphicHwOps ngage_lcd_gfx_ops = {
    .gfx_update = ngage_lcd_gfx_update,
    .invalidate = ngage_lcd_invalidate,
};

static const MemoryRegionOps ngage_lcdif_ops = {
    .read = ngage_lcdif_read, .write = ngage_lcdif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- kernel debug output (Q18) ------------------------------------------ */
/*
 * [V] ekern 0x5031e4a8 (enabled by debug-mask bit 30): the cursor (superpage 0x400003FC)
 * starts at virtual 0x59000000 = physical 0x0DE00000; a message starts with 0xC0 written at
 * the base, each character is stored at the next byte address, '\n' writes 0xE0 to virtual
 * 0x591D0000 (physical 0x0DFD0000) and resets the cursor; '\r' is dropped.
 * Model: collect the characters, emit a line on the 0xE0 store, to serial 0 and the log.
 */
#define NGAGE_DBG_BASE  0x0de00000
#define NGAGE_DBG_SIZE  0x00200000
#define NGAGE_DBG_END   0x001d0000          /* offset of the end-of-message store */

typedef struct {
    MemoryRegion mr;
    CharFrontend chr;
    GString *line;
} NgageDbg;

static void ngage_dbg_flush(NgageDbg *d)
{
    qemu_log_mask(LOG_UNIMP, "ngage-dbg: %s\n", d->line->str);
    g_string_append(d->line, "\r\n");
    qemu_chr_fe_write_all(&d->chr, (const uint8_t *)d->line->str, d->line->len);
    g_string_truncate(d->line, 0);
}

static uint64_t ngage_dbg_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void ngage_dbg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageDbg *d = opaque;

    if (addr == NGAGE_DBG_END) {
        ngage_dbg_flush(d);
    } else if (addr == 0 && val == 0xc0 && d->line->len == 0) {
        /* message header; the first character then lands on the same address */
    } else if (addr < NGAGE_DBG_END) {
        g_string_append_c(d->line, (char)val);
    }
}

static const MemoryRegionOps ngage_dbg_ops = {
    .read = ngage_dbg_read, .write = ngage_dbg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* ---- NOS stand-in (H-nos-standin, Q20, Q21) ------------------------------ */
/*
 * The cellular software (NOS) does not run: its code is scrambled in the flash file. The
 * guest stub src/qemu-ngage/nos-standin/nos_standin.S is copied over the NOS entry point
 * (virtual 0x5149E4C4 = flash 0x9E4C4), so nosstarter.ext calls it. It registers trap objects
 * in the variant's interface slots; each method call lands here:
 *   +00 W  call: slot << 8 | method (0xFF00 = stub entered, 0xFE00|slot = registered, r0 = ok)
 *   +04..+10 r0..r3, +14..+20 the caller's first four stack arguments
 *   +28 R  result (r0)
 *   +30 W  ISI delivery (Q50, ngage_isi.c): 1 = fetch the next one into +34 channel, +38 the
 *          client's receive descriptor, +3C request number, +40 length (+34 = 0: none left),
 *          the frame at +100; 2 = done, +44 = DThread::Write's result
 * Every call is logged ("ngage-nos:"); answers come from ngage_nos_answers, default 0.
 * The ISI stand-in (the phone protocol to our fake modem) joins this device later.
 */
#define NGAGE_NOS_BASE  0x0c1f0000          /* board-only address, virtual 0x581F0000 */

#include "ngage_nos_stub.h"
static bool ngage_flash_restored;    /* Q55: saved flash applied, not a factory-fresh phone */
#include "ngage_isi.h"
#include "ngage_keys.h"

typedef struct {
    MemoryRegion mr;
    uint32_t a[9];
    uint32_t result;
    uint32_t dfc;               /* +2C: TDfc the stub queues after the call (Q34) */
    bool idle_logged;
    bool rx_dfc_busy;           /* the stub's ISI delivery DFC is queued or running (Q50) */
    uint32_t rx[5];             /* +34..+44: the delivery being fetched */
    uint32_t rx_flag;           /* last value written to the stub's RX_FLAG */
} NgageNos;

/* The stub's ISI delivery TDfc and "delivery waits" flag, in the NOS bss (nos_standin.S) */
#define NGAGE_NOS_RX_DFC  0x8000c578
#define NGAGE_NOS_RX_FLAG 0x8000c570

/*
 * Hand the stub its delivery DFC when a reply waits for a posted receive (Q50). While one
 * waits but can't be handed out yet, RX_FLAG makes the stub's idle hook take the device path
 * (it answers the idle hook from RAM otherwise, Q56).
 */
static void ngage_nos_rx_kick(NgageNos *n)
{
    bool ready = ngage_isi_ready();
    uint32_t flag;

    if (ready && !n->rx_dfc_busy && !n->dfc) {
        n->dfc = NGAGE_NOS_RX_DFC;
        n->rx_dfc_busy = true;
    }
    flag = cpu_to_le32(ready && !n->dfc);
    if (flag != n->rx_flag && current_cpu &&
        !cpu_memory_rw_debug(current_cpu, NGAGE_NOS_RX_FLAG, &flag, 4, true)) {
        n->rx_flag = flag;
    }
}

static const struct {
    uint8_t slot, method;
    uint32_t r0;
    const char *why;
} ngage_nos_answers[] = {
    { 0, 0, 0, NULL },
};

/*
 * Slot 3 (product data, Q21) method 18 (vtbl +0x48) = Read(item r1, r2, out r3, sp[0],
 * length sp[1], sp[2]) [V: product_info.dll 0x50419e94: item 14 into a stack word, item 15
 * straight into its own field this+8, which must then be <= 3]. The NOS writes the item
 * there; the stand-in writes these values (little-endian, 'length' bytes), 0 for any other
 * item (logged).
 */
static const struct {
    uint32_t item, value;
    const char *why;
    bool pending;               /* asynchronous read left pending: a notification (Q34) */
} ngage_nos_items[] = {
    { 5, 0, "Q34: ecust's asynchronous read at start-up (DFC at variant+0xB4). Its completion "
            "resets the phone unless the start-up mode is 1 or 2 (alarm/charger-type starts), "
            "so it is an event the NOS signals later: left pending [I]", true },
    { 14, 0, "Q23: product_info accepts value/100 in {0, 1004, 4400} [V]; 0 [I]" },
    { 15, 0, "Q23: product_info's index (must be <= 3) [V]; 0 [I]" },
    { 12, 105, "Q34/Q44: read asynchronously by helper.ldd request 1 (EStart), 2 bytes, "
               "EStart's request result [V]. EStart 0x503e9bce: if its C: mount fails and the "
               "value is 105 it formats C: (internal flash) instead of falling back to a RAM C: "
               "that Starter rejects (Q42) [V]. 105 = 'format the user area' [I]" },
};

static void ngage_nos_write_out(uint32_t va, uint32_t value, uint32_t len)
{
    uint8_t buf[16] = { 0 };

    len = MIN(len, sizeof(buf));
    stl_le_p(buf, value);
    if (current_cpu && va) {
        cpu_memory_rw_debug(current_cpu, va, buf, len, true);
    }
}

static uint64_t ngage_nos_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageNos *n = opaque;

    if (addr == 0x2c) {
        uint32_t d = n->dfc;
        n->dfc = 0;
        return d;
    }
    if (addr >= 0x34 && addr <= 0x44) {
        return n->rx[(addr - 0x34) >> 2];
    }
    if (addr >= 0x100) {
        return ngage_isi_fetched_word(addr - 0x100);
    }
    return addr == 0x28 ? n->result : (addr < 0x24 ? n->a[addr >> 2] : 0);
}

static void ngage_nos_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageNos *n = opaque;
    unsigned slot, method;

    if (addr == 0x30) {
        if (val == 1) {
            memset(n->rx, 0, sizeof(n->rx));
            if (!ngage_isi_fetch(&n->rx[0], &n->rx[1], &n->rx[2], &n->rx[3])) {
                n->rx_dfc_busy = false;         /* the DFC ends; the next reply kicks it again */
            }
        } else if (val == 2) {
            ngage_isi_done(n->rx[4]);
        }
        return;
    }
    if (addr == 0x44) {
        n->rx[4] = val;
        return;
    }
    if (addr) {
        if (addr < 0x24) {
            n->a[addr >> 2] = val;
        }
        return;
    }
    if (val == 0xff00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: stand-in entered (instead of the NOS)\n");
        ngage_nos_entered = true;
        return;
    }
    if (val == 0xfc00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: NOSDRIVER channel object %08x constructed, unit %u\n",
                      n->a[1], n->a[2]);
        ngage_isi_ctor(n->a[1], n->a[2]);
        return;
    }
    if (val == 0xfb00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: RTC hand-over to rtc.dll (request 1) -> %u (Q49)\n",
                      n->a[1]);
        return;
    }
    if (val == 0xfd00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: variant NOS status set to %u\n", n->a[1]);
        return;
    }
    if ((val & 0xff00) == 0xfe00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: interface %u registered: %s\n",
                      (unsigned)(val & 0xff), n->a[1] ? "ok" : "REFUSED (slot taken)");
        return;
    }
    slot = (val >> 8) & 0xff;
    method = val & 0xff;
    n->result = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_nos_answers); i++) {
        if (ngage_nos_answers[i].why && ngage_nos_answers[i].slot == slot &&
            ngage_nos_answers[i].method == method) {
            n->result = ngage_nos_answers[i].r0;
        }
    }
    if (slot == 3 && method == 18 && n->a[2] == 13 && !n->a[4]) {
        /* the null thread's per-pass idle hook (Q25): log once */
        if (!n->idle_logged) {
            qemu_log_mask(LOG_UNIMP, "ngage-nos: idle hook (slot 3 item 13) called; further calls "
                          "not logged\n");
            n->idle_logged = true;
        }
        n->result = 0;
        ngage_nos_rx_kick(n);
        return;
    }
    if (slot == 3 && method == 18) {
        uint32_t item = n->a[2], value = 0;
        const char *src = " (unknown item: 0)";
        bool pending = false;

        for (unsigned i = 0; i < ARRAY_SIZE(ngage_nos_items); i++) {
            if (ngage_nos_items[i].item == item) {
                value = ngage_nos_items[i].value;
                pending = ngage_nos_items[i].pending;
                src = "";
            }
        }
        if (item == 12 && ngage_flash_restored) {
            /* Q55: 105 makes Starter run the deep factory restore, which formats the user area
             * again; a phone whose flash was kept reports a normal start [I] */
            value = 100;
            src = " (saved flash: normal start)";
        }
        if (item == 12 && getenv("NGAGE_ITEM12")) {    /* experiments (Q39) */
            value = strtoul(getenv("NGAGE_ITEM12"), NULL, 0);
            src = " (NGAGE_ITEM12)";
        }
        /* r2 != 0: an asynchronous read; r2 is the caller's TDfc, queued when done (Q34) */
        if (n->a[3] && pending) {
            qemu_log_mask(LOG_UNIMP, "ngage-nos: product data item %u: asynchronous, left pending "
                          "(notification)\n", item);
        } else {
            ngage_nos_write_out(n->a[4], value, n->a[6]);
            n->dfc = n->a[3];
            qemu_log_mask(LOG_UNIMP, "ngage-nos: product data item %u -> %u (%u bytes)%s%s\n",
                          item, value, n->a[6], src, n->dfc ? ", asynchronous: DFC queued" : "");
        }
    }
    static uint16_t ncount[128][64];
    if (slot == 65 || slot == 66 || ngage_log_hot(&ncount[slot & 127][method & 63]))
    qemu_log_mask(LOG_UNIMP, "ngage-nos: slot %u method %u (vtbl +0x%x) this=%08x "
                  "r1=%08x r2=%08x r3=%08x sp[0..3]=%08x %08x %08x %08x -> %08x lr=%08x\n",
                  slot, method, method * 4, n->a[1], n->a[2], n->a[3], n->a[4],
                  n->a[5], n->a[6], n->a[7], n->a[8], n->result, ngage_lr());
    if (slot == 65 && method >= 3 && method <= 5) {
        ngage_isi_call(n->a[1], method, n->a[2], n->a[3], n->a[4]);     /* Q50 */
    }
    if (slot == 66 && method == 0) {
        ngage_isi_open(n->a[1], n->a[2]);
    }
    ngage_nos_rx_kick(n);
}

static const MemoryRegionOps ngage_nos_ops = {
    .read = ngage_nos_read, .write = ngage_nos_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- machine ------------------------------------------------------------ */

static struct arm_boot_info ngage_binfo;

/*
 * One x16 Intel flash device (CFI01). Both chips answer an Intel ID query (Q2, Q4):
 * device-width must be set, otherwise CFI01's legacy ID mode packs both ID bytes into one
 * word. Without a drive the contents are erased (0xFF); 'image' fills them from the start.
 */
static DeviceState *ngage_flash(hwaddr base, const char *name, const void *image, size_t len)
{
    DeviceState *fl = qdev_new(TYPE_PFLASH_CFI01);
    uint8_t *p;

    qdev_prop_set_uint32(fl, "num-blocks", NGAGE_FLASH_SIZE / NGAGE_FLASH_SECTOR);
    qdev_prop_set_uint64(fl, "sector-length", NGAGE_FLASH_SECTOR);
    /*
     * Read-while-write (Q48): medint.pdd programs C: in chip 1 while the kernel runs code and
     * reads vtables from the ROM part of the same chip. Intel 0x8874 (StrataFlash Wireless
     * L18, 256 Mbit) has 16 Mbit partitions [I]; without them every read of chip 1 returned
     * the status word (0x0080) during a program and nosdriver.ldd's vtable became 0x00800080.
     */
    qdev_prop_set_uint32(fl, "partition-size", NGAGE_FLASH_PARTITION);
    qdev_prop_set_uint8(fl, "width", 2);
    qdev_prop_set_uint8(fl, "device-width", 2);
    qdev_prop_set_uint16(fl, "id0", NGAGE_FLASH_MANUF);
    qdev_prop_set_uint16(fl, "id1", NGAGE_FLASH_DEVICE);
    qdev_prop_set_string(fl, "name", name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(fl), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(fl), 0, base);
    p = memory_region_get_ram_ptr(pflash_cfi01_get_memory(PFLASH_CFI01(fl)));
    memset(p, 0xff, NGAGE_FLASH_SIZE);
    if (image) {
        memcpy(p, image, len);
    }
    return fl;
}

/*
 * Declared code patches (docs/eka1-ngage-questions.md names each), applied to the board's
 * flash copy only. Each checks the original word first and refuses to patch anything else.
 */
static const struct {
    uint32_t va;
    uint32_t orig, patched;
    const char *why;
} ngage_patches[] = {
    { 0, 0, 0, NULL },
};

static void ngage_apply_patches(uint8_t *img, size_t len)
{
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_patches) && ngage_patches[i].va; i++) {
        size_t o = ngage_patches[i].va - 0x50000000;
        uint32_t w;

        if (o + 4 > len) {
            continue;
        }
        w = ldl_le_p(img + o);
        if (w != ngage_patches[i].orig) {
            warn_report("ngage: patch at %08x not applied: found %08x, expected %08x (%s)",
                        ngage_patches[i].va, w, ngage_patches[i].orig, ngage_patches[i].why);
            continue;
        }
        stl_le_p(img + o, ngage_patches[i].patched);
        info_report("ngage: %s", ngage_patches[i].why);
    }
}

/*
 * Declared workaround H-ngage-md5 (Q12, docs/eka1-ngage-questions.md). The bootstrap
 * (0x50003238) MD5s ROM ranges listed at ROM offset 0x8f00/0x8f18 ({virtual start, end,
 * 16-byte digest}) and halts on a mismatch. The digest stored in the RH-29 V 04.10 flash file
 * is not the MD5 of the file's bytes (checked with the ROM's own MD5 code on this board), so
 * the board writes the digest of the image it actually serves into its flash copy. ROM.bin
 * on disk is not changed.
 */
static void ngage_fix_md5_table(uint8_t *img, size_t len)
{
    static const size_t entries[] = { 0x8f00, 0x8f18 };

    for (unsigned i = 0; i < ARRAY_SIZE(entries); i++) {
        size_t e = entries[i];
        uint32_t start, end;
        uint8_t dg[16];
        gsize dlen = sizeof(dg);
        GChecksum *ck;

        if (e + 24 > len) {
            continue;
        }
        start = ldl_le_p(img + e);
        end = ldl_le_p(img + e + 4);
        if (start == 0xffffffff || start < 0x50000000 || end < start ||
            end - 0x50000000 >= len) {
            continue;
        }
        ck = g_checksum_new(G_CHECKSUM_MD5);
        g_checksum_update(ck, img + (start - 0x50000000), end - start + 1);
        g_checksum_get_digest(ck, dg, &dlen);
        g_checksum_free(ck);
        if (memcmp(dg, img + e + 8, 16)) {
            warn_report("ngage: H-ngage-md5: ROM MD5 entry 0x%zx (%08x-%08x) does not match the "
                        "image; writing the image's digest into the board's flash copy (Q12)",
                        e, start, end);
            memcpy(img + e + 8, dg, 16);
        }
    }
}

/*
 * The PPM flash file (RH2941.026: second Z: tree, language data, and a rewritten boot block
 * with the PPM's MD5 entry at 0x8F00, Q28) as extracted segments, loaded over the ROM like a
 * second flashing: -M ngage,ppm=DIR, files segment_NNN_typeTT_<flash address>.bin.
 * Phase 1 (img != NULL): segments below the ROM split go into the virtual ROM image (boot
 * block: before the patches and the MD5 workaround). Phase 2: the others into the chips.
 */
static char *ngage_ppm_dir;
static char *ngage_isi_file;         /* -M ngage,isi=FILE: the ISI reply table (Q50) */
static char *ngage_flash_file;       /* -M ngage,flash=FILE: the phone's own flash (Q55) */
static char *ngage_mmc_file;         /* -M ngage,mmc=FILE: the memory card image (ngage_mmc.c) */

/*
 * Persistent flash (Q55): the phone's writes to its flash (C:, the internal user disk, and any
 * other block the OS programs) survive a restart. At exit every 64 KB block that differs from
 * the freshly composed image is saved to FILE ("NGFL1" + {chip, offset, 64 KB} records); at
 * start the image is composed as usual and the records are applied. So a changed ROM or stand-in
 * stays in effect for the blocks the phone never wrote, and without FILE nothing persists (each
 * boot formats C: and runs the first-boot factory settings again: ~75 s).
 */
static uint8_t *ngage_flash_chip[2], *ngage_flash_pristine[2];
static Notifier ngage_flash_exit;

static void ngage_flash_save(Notifier *n, void *data)
{
    g_autofree char *tmp = g_strdup_printf("%s.tmp", ngage_flash_file);
    FILE *f = fopen(tmp, "wb");
    unsigned blocks = 0;

    if (!f) {
        error_report("ngage: flash=%s: cannot write: %s", ngage_flash_file, strerror(errno));
        return;
    }
    fwrite("NGFL1", 1, 6, f);
    for (uint32_t c = 0; c < 2; c++) {
        for (uint32_t o = 0; o < NGAGE_FLASH_SIZE; o += NGAGE_FLASH_SECTOR) {
            if (memcmp(ngage_flash_chip[c] + o, ngage_flash_pristine[c] + o, NGAGE_FLASH_SECTOR)) {
                uint32_t hdr[2] = { cpu_to_le32(c), cpu_to_le32(o) };
                fwrite(hdr, 1, sizeof(hdr), f);
                fwrite(ngage_flash_chip[c] + o, 1, NGAGE_FLASH_SECTOR, f);
                blocks++;
            }
        }
    }
    if (fclose(f) == 0 && rename(tmp, ngage_flash_file) == 0) {
        info_report("ngage: flash: %u changed blocks saved to %s", blocks, ngage_flash_file);
    }
}

static void ngage_flash_persist(uint8_t *chip0, uint8_t *chip1)
{
    g_autofree char *buf = NULL;
    gsize n = 0, pos = 6;
    unsigned blocks = 0;

    ngage_flash_chip[0] = chip0;
    ngage_flash_chip[1] = chip1;
    for (int c = 0; c < 2; c++) {
        ngage_flash_pristine[c] = g_memdup2(ngage_flash_chip[c], NGAGE_FLASH_SIZE);
    }
    if (g_file_get_contents(ngage_flash_file, &buf, &n, NULL)) {
        if (n < 6 || memcmp(buf, "NGFL1", 6)) {
            error_report("ngage: flash=%s: not a saved flash file", ngage_flash_file);
            exit(1);
        }
        while (pos + 8 + NGAGE_FLASH_SECTOR <= n) {
            uint32_t c = ldl_le_p(buf + pos), o = ldl_le_p(buf + pos + 4);

            if (c < 2 && o < NGAGE_FLASH_SIZE && !(o % NGAGE_FLASH_SECTOR)) {
                memcpy(ngage_flash_chip[c] + o, buf + pos + 8, NGAGE_FLASH_SECTOR);
                blocks++;
            }
            pos += 8 + NGAGE_FLASH_SECTOR;
        }
        info_report("ngage: flash: %u saved blocks applied from %s", blocks, ngage_flash_file);
        ngage_flash_restored = blocks > 0;
    } else {
        info_report("ngage: flash: %s is new: factory-fresh phone", ngage_flash_file);
    }
    ngage_flash_exit.notify = ngage_flash_save;
    qemu_add_exit_notifier(&ngage_flash_exit);
}

static void ngage_load_segments(uint8_t *img, size_t len, uint8_t *chip0, uint8_t *chip1)
{
    GDir *dir;
    const char *name;

    if (!ngage_ppm_dir) {
        return;
    }
    dir = g_dir_open(ngage_ppm_dir, 0, NULL);
    if (!dir) {
        error_report("ngage: ppm=%s: not a directory", ngage_ppm_dir);
        exit(1);
    }
    while ((name = g_dir_read_name(dir))) {
        const char *u = strrchr(name, '_'), *end = NULL;
        uint64_t fa;
        gchar *path, *buf;
        gsize n;

        if (!g_str_has_prefix(name, "segment_") || !g_str_has_suffix(name, ".bin") || !u ||
            qemu_strtou64(u + 1, &end, 16, &fa) || strcmp(end, ".bin")) {
            continue;
        }
        path = g_build_filename(ngage_ppm_dir, name, NULL);
        if (!g_file_get_contents(path, &buf, &n, NULL)) {
            error_report("ngage: cannot read %s", path);
            exit(1);
        }
        if (img && fa + n <= NGAGE_ROM_SPLIT && fa + n <= len) {
            memcpy(img + fa, buf, n);
            info_report("ngage: ppm: %s -> flash %08" PRIx64 " (%zu bytes, boot block)", name, fa, n);
        } else if (!img && fa >= NGAGE_ROM_SPLIT && fa + n <= NGAGE_FLASH_SIZE) {
            memcpy(chip0 + fa, buf, n);
            info_report("ngage: ppm: %s -> flash %08" PRIx64 " (%zu bytes)", name, fa, n);
        } else if (!img && fa >= NGAGE_FLASH1_BASE && fa + n <= NGAGE_FLASH1_BASE + NGAGE_FLASH_SIZE) {
            memcpy(chip1 + (fa - NGAGE_FLASH1_BASE), buf, n);
            info_report("ngage: ppm: %s -> flash %08" PRIx64 " (%zu bytes)", name, fa, n);
        }
        g_free(buf);
        g_free(path);
    }
    g_dir_close(dir);
}

static uint8_t *ngage_flash_ptr(DeviceState *fl)
{
    return memory_region_get_ram_ptr(pflash_cfi01_get_memory(PFLASH_CFI01(fl)));
}

static void ngage_init(MachineState *machine)
{
    MemoryRegion *sys = get_system_memory();
    NgageUnknown *u = g_new0(NgageUnknown, 1);
    ARMCPU *cpu;
    char *fn;
    gsize len;
    gchar *data;
    GError *err = NULL;

    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    u->mem = g_hash_table_new(g_direct_hash, g_direct_equal);
    u->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&u->mr, NULL, &ngage_unknown_ops, u, "ngage.unknown", 4 * GiB);
    memory_region_add_subregion_overlap(sys, 0, &u->mr, -1000);

    if (!machine->firmware) {
        error_report("ngage: give the phone's core ROM image with -bios ROM.bin");
        exit(1);
    }
    fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!fn || !g_file_get_contents(fn, &data, &len, &err)) {
        error_report("ngage: cannot read %s: %s", machine->firmware, err ? err->message : "not found");
        exit(1);
    }
    if (len > NGAGE_FLASH_SIZE) {
        error_report("ngage: %s is %zu bytes, larger than the %u MiB boot flash", fn, (size_t)len,
                     (unsigned)(NGAGE_FLASH_SIZE / MiB));
        exit(1);
    }
    ngage_load_segments((uint8_t *)data, len, NULL, NULL);     /* PPM boot block (Q28) */
    ngage_apply_patches((uint8_t *)data, len);
    /* H-nos-standin: the stub over the NOS entry (virtual 0x514xxxxx = flash 0x00xxxxxx, Q19) */
    memcpy(data + (NGAGE_NOS_STUB_VA - 0x51400000), ngage_nos_stub, sizeof(ngage_nos_stub));
    info_report("ngage: H-nos-standin: NOS entry replaced by the stand-in stub (%zu bytes)",
                sizeof(ngage_nos_stub));
    {
        /* the two NOS functions nosdriver.ldd imports (Q33): jumps into the stub */
        static const uint32_t nos_exports[][2] = {
            { 0x514a260c, NGAGE_NOS_SYM_nos_channel_ctor },
            { 0x514a1c90, NGAGE_NOS_SYM_nos_channel_open },
        };
        for (unsigned i = 0; i < ARRAY_SIZE(nos_exports); i++) {
            uint8_t *p = (uint8_t *)data + (nos_exports[i][0] - 0x51400000);
            stl_le_p(p, 0xe51ff004);                /* ldr pc, [pc, #-4] */
            stl_le_p(p + 4, nos_exports[i][1]);
        }
    }
    ngage_fix_md5_table((uint8_t *)data, len);

    /*
     * ROM.bin is the image as the MMU sees it (virtual 0x50000000). On the phone it is split
     * over the two chips (Q12): offsets 0..0xEFFFFF in chip 0, the rest at the start of chip 1
     * (flash file token 0xc8 ranges; bootstrap bank table 0x744 ends at 0xF00000).
     */
    {
        DeviceState *f0, *f1;

        f0 = ngage_flash(NGAGE_FLASH0_BASE, "ngage.flash0", data, MIN(len, NGAGE_ROM_SPLIT));
        f1 = ngage_flash(NGAGE_FLASH1_BASE, "ngage.flash1", data + MIN(len, NGAGE_ROM_SPLIT),
                         len > NGAGE_ROM_SPLIT ? len - NGAGE_ROM_SPLIT : 0);
        ngage_load_segments(NULL, 0, ngage_flash_ptr(f0), ngage_flash_ptr(f1));   /* Q28 */
        if (ngage_flash_file) {
            ngage_flash_persist(ngage_flash_ptr(f0), ngage_flash_ptr(f1));       /* Q55 */
        }
    }
    g_free(data);
    g_free(fn);

    memory_region_add_subregion(sys, NGAGE_SDRAM_BASE, machine->ram);   /* Q3 */

    {
        NgageCbus *c = g_new0(NgageCbus, 1);
        memory_region_init_io(&c->mr, NULL, &ngage_cbus_ops, c, "ngage.cbus", 0x8);
        memory_region_add_subregion(sys, NGAGE_CBUS_BASE, &c->mr);       /* Q11 */
    }

    {
        NgageIntc *ic = g_new0(NgageIntc, 1);
        NgageTimers *t = g_new0(NgageTimers, 1);
        qemu_irq *in;

        ic->irq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
        ic->fiq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
        memory_region_init_io(&ic->mr, NULL, &ngage_intc_ops, ic, "ngage.intc", 0x100);
        memory_region_add_subregion(sys, NGAGE_INTC_BASE, &ic->mr);       /* Q16 */
        in = qemu_allocate_irqs(ngage_intc_set, ic, 128);

        for (int i = 0; i < NGAGE_TIMERS; i++) {
            t->t[i].owner = t;
            t->t[i].index = i;
            t->t[i].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, ngage_timer_expire, &t->t[i]);
            t->t[i].irq = in[i];                                         /* IrqTimer<i> */
        }
        ngage_keys_init(sys, in[25]);                       /* keypad, IrqCTSI_IRQ (Q53) */
        ngage_mmc_init(sys, in, ngage_mmc_file);            /* MMC 21-24, DMA channels 6.. [I] */
        memory_region_init_io(&t->mr, NULL, &ngage_timer_ops, t, "ngage.timers",
                              0x20 * NGAGE_TIMERS);
        memory_region_add_subregion(sys, NGAGE_TIMER_BASE, &t->mr);       /* Q13 Q17 Q31 */
    }

    {
        NgageDbg *d = g_new0(NgageDbg, 1);
        d->line = g_string_new(NULL);
        if (serial_hd(0)) {
            qemu_chr_fe_init(&d->chr, serial_hd(0), &error_abort);
        }
        memory_region_init_io(&d->mr, NULL, &ngage_dbg_ops, d, "ngage.dbg", NGAGE_DBG_SIZE);
        memory_region_add_subregion(sys, NGAGE_DBG_BASE, &d->mr);          /* Q18 */
    }

    {
        NgageNos *n = g_new0(NgageNos, 1);
        memory_region_init_io(&n->mr, NULL, &ngage_nos_ops, n, "ngage.nos", 0x400);
        ngage_isi_load(ngage_isi_file);                                    /* Q50 */
        memory_region_add_subregion(sys, NGAGE_NOS_BASE, &n->mr);          /* H-nos-standin */
    }

    {
        NgageLcdIf *l = g_new0(NgageLcdIf, 1);
        l->x1 = NGAGE_LCD_W - 1;
        l->y1 = NGAGE_LCD_H - 1;
        l->con = qemu_graphic_console_create(NULL, 0, &ngage_lcd_gfx_ops, l);
        qemu_console_resize(l->con, NGAGE_LCD_W, NGAGE_LCD_H);
        memory_region_init_io(&l->mr, NULL, &ngage_lcdif_ops, l, "ngage.lcdif", 0x10);
        memory_region_add_subregion(sys, NGAGE_LCDIF_BASE, &l->mr);         /* Q37 */
    }

    ngage_binfo.ram_size = machine->ram_size;
    ngage_binfo.loader_start = NGAGE_SDRAM_BASE;
    ngage_binfo.firmware_loaded = true;
    arm_load_kernel(cpu, machine, &ngage_binfo);
}

static char *ngage_get_ppm(Object *obj, Error **errp)
{
    return g_strdup(ngage_ppm_dir ? ngage_ppm_dir : "");
}

static void ngage_set_ppm(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_ppm_dir);
    ngage_ppm_dir = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_isi(Object *obj, Error **errp)
{
    return g_strdup(ngage_isi_file ? ngage_isi_file : "");
}

static void ngage_set_isi(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_isi_file);
    ngage_isi_file = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_flash(Object *obj, Error **errp)
{
    return g_strdup(ngage_flash_file ? ngage_flash_file : "");
}

static void ngage_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_flash_file);
    ngage_flash_file = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_mmc(Object *obj, Error **errp)
{
    return g_strdup(ngage_mmc_file ? ngage_mmc_file : "");
}

static void ngage_set_mmc(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_mmc_file);
    ngage_mmc_file = *value ? g_strdup(value) : NULL;
}

static void ngage_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "mmc", ngage_get_mmc, ngage_set_mmc);
    object_class_property_set_description(oc, "mmc",
        "memory card image (MBR + FAT16, eka1/tools/ngage_card.py), the phone's E:");

    object_class_property_add_str(oc, "flash", ngage_get_flash, ngage_set_flash);
    object_class_property_set_description(oc, "flash",
        "the phone's own flash: changed blocks saved here at exit and applied at start");

    object_class_property_add_str(oc, "isi", ngage_get_isi, ngage_set_isi);
    object_class_property_set_description(oc, "isi",
        "ISI reply table for the NOSDRIVER channels (reply RES REQ RESP BYTES...)");

    object_class_property_add_str(oc, "ppm", ngage_get_ppm, ngage_set_ppm);
    object_class_property_set_description(oc, "ppm",
        "directory of extracted PPM flash segments (segment_*_<flash address>.bin)");

    mc->desc = "Nokia N-Gage QD (RH-29, DCT4 UPP, EKA1) bring-up board";
    mc->init = ngage_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = 16 * MiB;           /* Q3 */
    mc->default_ram_id = "ngage.sdram";
}

static const TypeInfo ngage_machine_type = {
    .name = MACHINE_TYPE_NAME("ngage"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void ngage_machine_init(void)
{
    type_register_static(&ngage_machine_type);
}

type_init(ngage_machine_init)
