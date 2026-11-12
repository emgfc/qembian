/*
 * N-Gage QD board: the UPP's MMC host controller (0x0C120600) and the DMA engine the MMC stack moves data with
 * (0x0C120300), so the phone's own MMC stack (epbus.dll, medmmc.pdd) sees a card backed by a host image file
 * (-M ngage,mmc=CARD.img; eka1/tools/ngage_card.py builds one). Protocol: docs/research/ngage-mmc-controller.md.
 *
 * MMC registers (offsets from 0x0C120600) [V unless marked]:
 *   +04 control/go: bit0 start (reads back 0: done at once), 2 = abort, 0x40 = init. Command: mode|1. Data phase:
 *       dir|phase<<2|1 [V log]: phase 0 = single or first block (the command in +1C goes with it: go 0x21),
 *       1 = middle, 2 = last (CMD12 in +1C goes with it, R1 after the data: go 0x29)
 *   +08 block length   +0C/+10/+14 timeouts and clock: stored
 *   +18 status: bit8 a response word waits, bit6 response timeout (no card / no answer); busy bits never set
 *   +1C command index  +20 W argument / R response words (MSB first; R2 = 4 words of bits 127:0)
 *   +24 data FIFO (the DMA engine's device address 0x10120624)
 * Interrupt sources: 21 DataIdle, 23 CmdIdle (both always true: nothing is ever busy), 24 CmdRdy (= status bit8).
 *
 * DMA (7 channels at 0x0C120304 + ch * 0x40): +04 enable, +08 control (2 device-paced, 1 memory to memory, 0 stop;
 * reads 0 = idle), +14 SOURCE, +20 DESTINATION [V: the LCD channel moves RAM -> 0x10121208 with these], +2C count - 1
 * (bytes for the MMC FIFO). Only transfers between RAM and the MMC FIFO are modelled: whole 32-bit words, at most the
 * rest of the current block; the whole count moves at once and raises the channel's interrupt (source 6 + ch [I]);
 * writing control 0 lowers it. Anything else (the LCD's channel) keeps the old catch-all behaviour (the control word
 * reads back what was written). Peripheral addresses come as 0x1012xxxx (the engine's alias of 0x0C12xxxx).
 *
 * The card: a raw image (MBR + FAT, eka1/tools/ngage_card.py), a whole number of 512-byte blocks, 1 MB .. 1 GB (the
 * largest CSD v1.x capacity with 512-byte blocks). Reads and writes go to the file (writes change it: keep a copy of
 * a card you care about). A machine snapshot is refused while a card is attached (its file is outside it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/core/irq.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "migration/blocker.h"
#include "system/reset.h"
#include "ngage_mmc.h"

#define MMC_BLK 512

typedef struct {
    MemoryRegion mr, dma_mr;
    qemu_irq data_idle, cmd_idle, cmd_rdy;
    qemu_irq dma_irq[7];
    int fd;
    uint64_t size;
    uint32_t ctl, blklen, cmd, arg, status, other[4];
    uint32_t resp[4];
    int nresp, rp;
    uint32_t rca;
    /* data: the block being moved through the FIFO */
    uint8_t buf[MMC_BLK * 4];
    uint32_t fpos, flen;
    uint64_t addr;          /* card byte address of the next block */
    bool reading, writing, multi;
    bool stop_after;        /* the last block of a multi-block transfer (CMD12 issued with it) */
    /* DMA */
    uint32_t dma[7][16];
    bool dma_busy[7];               /* a channel we don't model: reads return what was written */
    uint64_t nlog, ngo;
    Error *blocker;
} NgageMmc;

static void mmc_irqs(NgageMmc *s)
{
    bool rdy = s->rp < s->nresp;
    s->status = (s->status & ~0x100u) | (rdy ? 0x100 : 0);
    qemu_set_irq(s->cmd_idle, 1);
    qemu_set_irq(s->data_idle, 1);
    qemu_set_irq(s->cmd_rdy, rdy);
}

static void set_bits(uint32_t w[4], int hi, int lo, uint32_t v)
{
    for (int b = lo; b <= hi; b++, v >>= 1) {
        int word = 3 - b / 32, bit = b % 32;      /* w[0] = bits 127:96 */
        w[word] = (w[word] & ~(1u << bit)) | ((v & 1) << bit);
    }
}

/* CSD v1.x: READ_BL_LEN 9, capacity (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks (the image size is checked at start:
   1 MB .. 1 GB; sizes between representable capacities round down) */
static void mmc_csd(NgageMmc *s, uint32_t w[4])
{
    uint64_t blocks = s->size / MMC_BLK;
    int mult = 7;
    while (mult > 0 && (blocks >> (mult + 2)) < 256) {
        mult--;
    }
    uint32_t csize = MIN((blocks >> (mult + 2)) - 1, 4095u);
    memset(w, 0, 16);
    set_bits(w, 127, 126, 1);       /* CSD_STRUCTURE 1.1 */
    set_bits(w, 125, 122, 2);       /* SPEC_VERS 2 */
    set_bits(w, 119, 112, 0x26);    /* TAAC */
    set_bits(w, 103, 96, 0x2a);     /* TRAN_SPEED 20 MHz */
    set_bits(w, 95, 84, 0x0f5);     /* CCC */
    set_bits(w, 83, 80, 9);         /* READ_BL_LEN */
    set_bits(w, 79, 79, 1);         /* READ_BL_PARTIAL */
    set_bits(w, 73, 62, csize);
    set_bits(w, 61, 50, 0xfff);     /* VDD currents */
    set_bits(w, 49, 47, mult);
    set_bits(w, 46, 42, 31);        /* ERASE_GRP_SIZE */
    set_bits(w, 28, 26, 2);         /* R2W_FACTOR */
    set_bits(w, 25, 22, 9);         /* WRITE_BL_LEN */
    set_bits(w, 0, 0, 1);
}

static void mmc_cid(uint32_t w[4])
{
    static const uint8_t cid[16] = { 0x15, 0x00, 0x00, 'Q', 'E', 'M', 'U', 'C', 'R', 0x10, 0x12, 0x34, 0x56,
                                     0x78, 0xa1, 0x01 };
    for (int i = 0; i < 4; i++) {
        w[i] = ldl_be_p(cid + 4 * i);
    }
}

/* exact file I/O (short transfers and EINTR retried); false on an error */
static bool mmc_io(int fd, uint8_t *buf, uint32_t n, uint64_t at, bool wr)
{
    while (n) {
        ssize_t r = wr ? pwrite(fd, buf, n, at) : pread(fd, buf, n, at);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        buf += r;
        at += r;
        n -= r;
    }
    return true;
}

/* the block at s->addr into the FIFO (reads); a block outside the card or a file error: data error, no data */
static void mmc_load(NgageMmc *s)
{
    s->fpos = 0;
    s->flen = 0;
    if (!s->blklen || s->blklen > sizeof(s->buf) || s->blklen & 3) {
        s->status |= 0x20;          /* unsupported block length */
        s->reading = s->writing = false;
        return;
    }
    if (s->addr + s->blklen > s->size) {
        s->status |= 0x80;          /* out of range: data timeout */
        s->reading = s->writing = false;
        return;
    }
    s->flen = s->blklen;
    if (s->reading && !mmc_io(s->fd, s->buf, s->flen, s->addr, false)) {
        s->status |= 0x80;
        s->reading = false;
        s->flen = 0;
    }
}

/* the FIFO's whole block onto the card (writes) */
static void mmc_flush(NgageMmc *s)
{
    if (s->writing && !mmc_io(s->fd, s->buf, s->flen, s->addr, true)) {
        s->status |= 0x400;         /* write error */
    }
    s->addr += s->flen;
    s->fpos = s->flen = 0;
    if (s->stop_after) {
        s->writing = s->multi = s->stop_after = false;
    }
}

/* the FIFO's block was read out */
static void mmc_block_read(NgageMmc *s)
{
    s->addr += s->flen;
    s->fpos = s->flen = 0;
    if (s->stop_after) {
        s->reading = s->multi = s->stop_after = false;
    }
}

/* run the command in s->cmd: fill the response words */
static void mmc_command(NgageMmc *s)
{
    const uint32_t r1 = 0x900 | (s->rca ? 0 : 0);       /* tran state, ready for data */

    s->rp = s->nresp = 0;
    s->status &= ~0x6c3u;
    if (s->fd < 0) {
        s->status |= 0x40;          /* no card: response timeout */
        return;
    }
    switch (s->cmd) {
    case 0:                         /* GO_IDLE: no response; any transfer ends */
        s->rca = 0;
        s->reading = s->writing = s->multi = s->stop_after = false;
        s->fpos = s->flen = 0;
        return;
    case 1:                         /* SEND_OP_COND: OCR, powered up */
        s->resp[0] = 0x80ff8000;
        s->nresp = 1;
        return;
    case 2: case 10:                /* ALL_SEND_CID / SEND_CID */
        mmc_cid(s->resp);
        s->nresp = 4;
        return;
    case 9:                         /* SEND_CSD */
        mmc_csd(s, s->resp);
        s->nresp = 4;
        return;
    case 3:                         /* SET_RELATIVE_ADDR (MMC: the host gives it) */
        s->rca = s->arg >> 16;
        s->resp[0] = 0x500;
        s->nresp = 1;
        return;
    case 17: case 18: case 24: case 25:
        s->addr = s->arg;           /* byte address (cards up to 2 GB) */
        s->reading = s->cmd == 17 || s->cmd == 18;
        s->writing = !s->reading;
        s->multi = s->cmd == 18 || s->cmd == 25;
        s->stop_after = false;
        break;
    case 12:                        /* STOP_TRANSMISSION */
        s->reading = s->writing = s->multi = s->stop_after = false;
        s->fpos = s->flen = 0;
        break;
    default:                        /* 7 select, 13 status, 16 block length, 42 ...: R1 */
        break;
    }
    s->resp[0] = r1;
    s->nresp = 1;
}

static void mmc_go(NgageMmc *s, uint32_t v)
{
    int phase = (v >> 2) & 3;
    bool data = (v & 0x30) != 0;    /* dir 0x10/0x20; phase 0 = a single block (CMD17/24: go 0x21) [V log] */

    if (s->ngo++ < 2000) {
        qemu_log_mask(LOG_UNIMP, "ngage-mmc: go %02x cmd %u arg %08x blk %u\n", v, s->cmd, s->arg, s->blklen);
    }
    if (!data) {
        mmc_command(s);
        return;
    }
    if (phase == 0) {               /* single or first block: the command goes with it (go 0x21) */
        mmc_command(s);
    } else if (phase == 1) {        /* middle block: no response word */
        s->rp = s->nresp = 0;
    } else {                        /* last block (go 0x29): CMD12 in +1C goes with it, its R1 after the data;
                                       the transfer stops once this block has moved */
        s->rp = s->nresp = 0;
        if (s->cmd == 12) {
            s->resp[0] = 0x900;
            s->nresp = 1;
            s->stop_after = true;
        }
    }
    if (s->reading || s->writing) {
        mmc_load(s);
    }
}

static uint64_t mmc_read_reg(NgageMmc *s, hwaddr off);

static uint64_t mmc_read(void *opaque, hwaddr off, unsigned size)
{
    NgageMmc *s = opaque;
    uint32_t v = mmc_read_reg(s, off);
    if (off != 0x24 && s->nlog++ < 200) {
        qemu_log_mask(LOG_UNIMP, "ngage-mmc: R +%02x -> %08x\n", (unsigned)off, v);
    }
    return v;
}

static uint64_t mmc_read_reg(NgageMmc *s, hwaddr off)
{
    uint32_t v = 0;

    switch (off) {
    case 0x04: v = s->ctl & ~1u; break;
    case 0x08: v = s->blklen; break;
    case 0x18: v = s->status; break;
    case 0x1c: v = s->cmd; break;
    case 0x20:
        v = s->rp < s->nresp ? s->resp[s->rp++] : 0;
        mmc_irqs(s);
        break;
    case 0x24:
        if (s->reading && s->fpos + 4 <= s->flen) {
            v = ldl_le_p(s->buf + s->fpos);
            s->fpos += 4;
            if (s->fpos >= s->flen) {
                mmc_block_read(s);
            }
        }
        break;
    case 0x0c: case 0x10: case 0x14: case 0x00:
        v = s->other[off == 0 ? 0 : (off - 0x0c) / 4 + 1];
        break;
    }
    return v;
}

static void mmc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NgageMmc *s = opaque;
    uint32_t v = val;

    if (off != 0x24 && s->nlog++ < 200) {
        qemu_log_mask(LOG_UNIMP, "ngage-mmc: W +%02x = %08x\n", (unsigned)off, v);
    }
    switch (off) {
    case 0x04:
        s->ctl = v & ~1u;
        if (v == 2) {               /* abort: a partly written block is dropped */
            s->rp = s->nresp = 0;
            s->reading = s->writing = s->multi = s->stop_after = false;
            s->fpos = s->flen = 0;
        } else if (v & 1) {
            mmc_go(s, v);
        }
        break;
    case 0x08: s->blklen = v; break;
    case 0x1c: s->cmd = v & 0x3f; break;
    case 0x20: s->arg = v; break;
    case 0x24:
        if (s->writing && s->fpos + 4 <= s->flen) {
            stl_le_p(s->buf + s->fpos, v);
            s->fpos += 4;
            if (s->fpos >= s->flen) {
                mmc_flush(s);
            }
        }
        break;
    case 0x0c: case 0x10: case 0x14: case 0x00:
        s->other[off == 0 ? 0 : (off - 0x0c) / 4 + 1] = v;
        break;
    }
    mmc_irqs(s);
}

static const MemoryRegionOps mmc_ops = {
    .read = mmc_read, .write = mmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- DMA ------------------------------------------------------------------------------------- */
static bool dma_is_fifo(uint32_t a)
{
    return (a & 0x00ffffff) == 0x120624 && ((a >> 24) == 0x10 || (a >> 24) == 0x0c);
}

static void dma_run(NgageMmc *s, int ch)
{
    uint32_t *r = s->dma[ch];
    uint32_t src = r[0x14 / 4], dst = r[0x20 / 4];     /* +0x314 source, +0x320 destination [V: the LCD channel
                                                          moves RAM -> 0x10121208] */
    uint64_t n = (uint64_t)r[0x2c / 4] + 1;
    uint8_t tmp[4];
    bool from_fifo = dma_is_fifo(src), to_fifo = dma_is_fifo(dst);

    if (from_fifo == to_fifo) {
        s->dma_busy[ch] = true;     /* not ours (the LCD's transfers): as before, the control word reads back */
        return;
    }
    s->dma_busy[ch] = false;
    if (s->ngo++ < 2000) {
        qemu_log_mask(LOG_UNIMP, "ngage-dma: ch %d %08x -> %08x, %" PRIu64 " bytes\n", ch, src, dst, n);
    }
    /* whole words, no more than the FIFO holds now, a RAM side that is RAM: else a data error and no transfer */
    uint32_t mem = from_fifo ? dst : src;
    uint32_t left = s->flen - s->fpos;
    if ((n & 3) || n > left || (uint64_t)mem + n > 0x100000000ull ||
        !address_space_access_valid(&address_space_memory, mem, n, !from_fifo, MEMTXATTRS_UNSPECIFIED) ||
        (from_fifo ? !s->reading : !s->writing)) {
        s->status |= 0x20;
        if (s->nlog++ < 200) {
            qemu_log_mask(LOG_GUEST_ERROR, "ngage-dma: ch %d refused: %" PRIu64 " bytes, %u in the FIFO\n", ch,
                          n, left);
        }
        mmc_irqs(s);
        qemu_set_irq(s->dma_irq[ch], 1);
        return;
    }
    for (uint32_t i = 0; i < n; i += 4) {
        if (from_fifo) {
            stl_le_p(tmp, mmc_read_reg(s, 0x24));
            address_space_write(&address_space_memory, dst + i, MEMTXATTRS_UNSPECIFIED, tmp, 4);
        } else {
            address_space_read(&address_space_memory, src + i, MEMTXATTRS_UNSPECIFIED, tmp, 4);
            mmc_write(s, 0x24, ldl_le_p(tmp), 4);
        }
    }
    qemu_set_irq(s->dma_irq[ch], 1);
}

/* region at 0x0C120300: channel ch's register 0x3RR at ch * 0x40 + RR (+04 enable, +08 control, +14 source,
   +20 destination, +2c count - 1) */
static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    NgageMmc *s = opaque;
    int ch = off / 0x40, r = off % 0x40;
    if (ch >= 7) {
        return 0;
    }
    if (r == 0x08 && !s->dma_busy[ch]) {   /* control: idle (MMC transfers finish at once) */
        return 0;
    }
    return s->dma[ch][r / 4];
}

static void dma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NgageMmc *s = opaque;
    int ch = off / 0x40, r = off % 0x40;
    if (ch >= 7) {
        return;
    }
    s->dma[ch][r / 4] = val;
    if (s->nlog++ < 200) {
        qemu_log_mask(LOG_UNIMP, "ngage-dma: W ch %d +%02x = %08x\n", ch, r, (unsigned)val);
    }
    if (r == 0x08) {                /* control (+0x308) */
        if (val == 1 || val == 2) {
            dma_run(s, ch);
        } else if (val == 0) {
            s->dma_busy[ch] = false;
            qemu_set_irq(s->dma_irq[ch], 0);
        }
    }
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read, .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mmc_reset(void *opaque)
{
    NgageMmc *s = opaque;
    s->ctl = s->blklen = s->cmd = s->arg = s->status = 0;
    s->nresp = s->rp = 0;
    s->rca = 0;
    s->fpos = s->flen = 0;
    s->addr = 0;
    s->reading = s->writing = s->multi = s->stop_after = false;
    memset(s->dma, 0, sizeof(s->dma));
    for (int i = 0; i < 7; i++) {
        s->dma_busy[i] = false;
        qemu_set_irq(s->dma_irq[i], 0);
    }
    mmc_irqs(s);
}

void ngage_mmc_init(MemoryRegion *sys, qemu_irq *intc, const char *card)
{
    NgageMmc *s = g_new0(NgageMmc, 1);

    s->fd = -1;
    if (card) {
        int fd = open(card, O_RDWR);
        off_t size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END);
        if (fd < 0 || size < 0) {
            error_report("ngage: mmc card %s: %s", card, strerror(errno));
        } else if (size % MMC_BLK || size < 1 * MiB || size > 1 * GiB) {
            error_report("ngage: mmc card %s: %lld bytes; a card image is a whole number of 512-byte blocks, "
                         "1 MB .. 1 GB (CSD v1.x)", card, (long long)size);
        } else {
            s->fd = fd;
            fd = -1;
            s->size = size;
            info_report("ngage: MMC card %s, %" PRIu64 " MB", card, s->size >> 20);
            error_setg(&s->blocker, "ngage: the MMC card image is outside machine snapshots");
            migrate_add_blocker(&s->blocker, &error_fatal);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    s->data_idle = intc[21];
    s->cmd_idle = intc[23];
    s->cmd_rdy = intc[24];
    for (int i = 0; i < 7; i++) {
        s->dma_irq[i] = intc[6 + i];
    }
    memory_region_init_io(&s->mr, NULL, &mmc_ops, s, "ngage.mmc", 0x40);
    memory_region_add_subregion(sys, 0x0c120600, &s->mr);
    memory_region_init_io(&s->dma_mr, NULL, &dma_ops, s, "ngage.dma", 7 * 0x40);
    memory_region_add_subregion(sys, 0x0c120300, &s->dma_mr);
    qemu_register_reset(mmc_reset, s);
    mmc_irqs(s);
}
