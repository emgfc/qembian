# N-Gage QD MMC host controller: what a QEMU mock needs

Source: `epbus.dll` (ROM 0x503f46b0, the EKA1 PBus/MMC stack with the ASIC-specific socket
inlined) and ekern's DMA ASSP code (0x5031b17c-0x5031ee60). Accessor PCs were cross-checked with
the MMIO log of run109/run110 (`build/ngage/run110.log`). [V] = read in the code, [I] = inferred.

## Addressing
- All peripherals: CPU virtual `0x58000000 + off` = physical `0x0C000000 + off` (ekern helpers
  0x5031e6b0/6bc, epbus literal 0x58120000) [V]. MMC block = physical **0x0C120600** [V].
- The DMA engine addresses the same block as **0x1012xxxx** (kernel table: MMC data = 0x10120624,
  LCD data = 0x10121208) [V]. A model must alias 0x1012xxxx -> 0x0C12xxxx for DMA traffic.

## Registers (offsets from 0x0C120600)
| off | dir | meaning |
|---|---|---|
| 0x04 | W | control/go. bit0 = start (self-clearing: polled until it reads 0, <=120 reads) [V]. bit1 = abort (written as 2 when status bits 2/3 stay set, and after a data error) [V]. 0x40 alone after power-on = init (followed only by a timer wait) [V]; 0 = power down [V]. Command issue writes `mode|1`, mode from the response-type field: none->0, 1->0x04, 2->0x10, 3->0x0C, long R2->0x48, 5->0x08 [V values; bit meanings I]. Data phase writes `dir|phase<<2|1`: dir 0x10 or 0x20 (0x503f6c8c>0 -> 0x10), phase 1 = first block (the command in 0x1C is sent with it), 2 = middle, 3 = last (CMD12 written to 0x1C first) [V]. |
| 0x08 | W | block length for data commands, 0 otherwise [V written; I meaning] |
| 0x0C, 0x10 | W | 0x001DC130 each: timeouts [I]. 0x14 = 0x280: clock divider/config [I]. Ignore. |
| 0x18 | R | status. bit2 = data unit busy, bit3 = command unit busy, bit8 = response word ready [V]. Error bits, command phase: bit6 -> KMMCErrResponseTimeOut(0x1), bit1 -> 0x20, bit9 -> 0x8000(Hardware); data phase: bit7 -> KMMCErrDataTimeOut(0x2), bit5 -> 0x4, bit0 -> 0x40, bit10 -> 0x8000 [V]. Mock: 0, plus bit8 while a response word waits, bit6 when no card. |
| 0x1C | W | command index [V] |
| 0x20 | W/R | W: argument [V]. R: response FIFO, one 32-bit word per read, stored MSB-first into the EKA1 response buffer: short response = 1 word = response bits 39:8 (card status for R1, OCR for R3); R2 = 4 words = CID/CSD bits 127:0 (word 0 = CID bytes 0-3) [V]. |
| 0x24 | R | data FIFO (DMA device address 0x10120624); drained once by the CPU in the reset path [V]. |

## Interrupts (level, sources 21-24) [V]
CmdIdle (23) = !status.bit3, DataIdle (21) = !status.bit2, CmdRdy (24) = status.bit8; DataRdy (22)
is not used by epbus. epbus enables one line just before waiting (ekern export 112) and disables it
in the ISR (export 84), so the model asserts while the condition holds (the board's intc is level).
Correction to Q29: event bit 1 is a kernel timer (timeout), the IRQs are events 2/4/8, DMA done = 16.

## Command sequence (socket power-up, then per command) [V]
1. 0x604=0, delay, 0x604=0x40, timer; 0x614=0x280, 0x610=0x60C=0x1DC130. Status bits 2/3 set here
   -> 0x604=2 and KMMCErrBusTimeOut.
2. Non-data command: 0x61C=idx, 0x620=arg, 0x608=0, 0x604=mode|1, poll bit0==0, wait CmdIdle, map
   status errors; if a response is expected: status.bit8 required (else KMMCErrHardware 0x8000),
   read 0x620; R2: 3 more words, each after CmdIdle|CmdRdy, then one after CmdIdle.
   "No card" today = the CmdIdle wait times out; with a model, answer CMD1 with status.bit6.
3. Expected card side is the stock EKA1 MMC stack [I, medmmc not read]: CMD0; CMD1 polled until OCR
   bit31 (answer 0x80FF8000); CMD2 CID; CMD3 (MMC: host assigns RCA = arg>>16, R1); CMD9 CSD
   (v1.x, READ_BL_LEN 9, C_SIZE for the image); CMD7; CMD16 (512); CMD13 (R1 0x900 = tran+ready);
   CMD17/18 reads, CMD24/25 writes, CMD12 after multi-block.
4. Data command: not sent by step 2 (command class 5 skips it). epbus writes 0x61C/0x620 (first
   phase only; CMD12 for the last), 0x608=blocklen, 0x604=dir|phase<<2|1, polls bit0, then starts
   one kernel DMA request of blocklen bytes, waits CmdIdle|DataIdle|DMA, and if status.bit8 reads
   one R1 word from 0x620 (mandatory on the first phase, must be absent otherwise or it is taken
   as the next response; R1 bits 31:16 set -> KMMCErrStatus). Remaining DMA count != 0 -> 0x8000.

## DMA controller (ekern, 7 channels, first free one; physical 0x0C120304 + ch*0x40) [V]
**Correction (2026-11-12, from the run logs):** +0x314 is the **source** and +0x320 the **destination** (the LCD
channel moves RAM -> 0x10121208 and the MMC read channel 0x10120624 -> RAM with these), not the other way round
as written below. Data phases are 0 = single/first block (go 0x21), 1 = middle (0x25), 2 = last with CMD12 (0x29),
not 1/2/3 as in the command sequence above. `src/qemu-ngage/hw/arm/ngage_mmc.c` follows the corrected reading.
+0x304 enable (1/0); +0x308 control: 2 = start (device-paced), 1 = mem-to-mem start, 0 = stop;
reads nonzero while busy; +0x30C/+0x310 read once by the stop path, +0x310 written 0 at start;
+0x314 destination address, +0x318 dest code (4 = memory, 0 = device), +0x31C dest burst (32 or
0x20); +0x320 source address, +0x324 source code (0 device / 4 memory), +0x328 source burst (8 for
the MMC FIFO, 32 memory); +0x32C = count-1, in bytes for MMC (shift 0; other devices words).
Request select: 4-bit field per channel at 0x0C120068 (read) / 0x0C120064 (write), value 2 = MMC.
Completion: IRQ of the channel (source 6+ch [I], table at RAM 0x800004f4 filled by the bootstrap);
the ISR reads no status: it writes ctrl 0, waits +0x308==0, enable 0, then restarts for the next
physically contiguous chunk or completes. Lower the IRQ on the ctrl=0 write.

## Card detect, Nokia specifics
- epbus has no GPIO/door code and imports only ekern + EUser [V]; presence is decided by the CMD1
  answer. A door switch, if any, lives in hotswapdrv.ldd/the variant (GPIO sources 29-82) [I,
  not read]. GD1DRV.ldd (game cards, 2.6 KB) and CMD42 lock handling (epbus copies a password
  buffer for CMD42) were not analysed; a plain FAT card should not hit them [I].

## Proposed QEMU model (pseudo-C, src/qemu-ngage/hw/arm/ngage.c)
```c
typedef struct { uint32_t ctl, blklen, cmd, arg, status; uint32_t resp[4]; int nresp, rp;
  uint8_t *buf; uint32_t pos, len; int phase, dir; BlockBackend *blk; qemu_irq irq[4]; } NgageMmc;
static void mmc_irqs(NgageMmc *s){ bool rdy = s->nresp > s->rp;
  s->status = (s->status & 0x6c2) | (rdy ? 0x100 : 0);          /* bits 2/3 never set */
  qemu_set_irq(s->irq[2]/*CmdIdle 23*/, 1); qemu_set_irq(s->irq[0]/*DataIdle 21*/, 1);
  qemu_set_irq(s->irq[3]/*CmdRdy 24*/, rdy); }
static void mmc_exec(NgageMmc *s, bool data){ uint32_t st = 0x900; s->rp = s->nresp = 0;
  if (!s->blk) { s->status |= 0x40; return; }                    /* response timeout */
  switch (s->cmd) { case 0: return; case 1: s->resp[0]=0x80ff8000; s->nresp=1; return;
  case 2: case 9: case 10: fill 4 words (CID or CSD, bits 127:0, MSB first); s->nresp=4; return;
  case 17: case 18: case 24: case 25: s->pos = s->arg; s->len = s->blklen; s->dir = cmd>=24; break;
  case 12: default: break; } s->resp[0] = st; s->nresp = 1; }
write 0x604: s->ctl = v & ~1; if (v & 1) { int ph = (v>>2)&3;
  if (ph == 0) mmc_exec(s, false); else if (ph == 1) mmc_exec(s, true);
  else if (ph == 3) { s->cmd = 12; mmc_exec(s, false); }       /* phase 2: no response word */
  if (ph) blk_pread/alloc one block at s->pos for the FIFO; } mmc_irqs(s);
write 0x61C/0x620/0x608: store; read 0x604: ctl (bit0 = 0); read 0x618: status; read 0x620:
  resp[rp++] (0 if empty), mmc_irqs(); read 0x624 (also via DMA at 0x10120624): next 4 FIFO bytes
  (little-endian); write 0x624 (DMA, writes): append, flush the block to blk at the end.
DMA (7 channels): on +0x308 = 2 move count+1 bytes from +0x320 to +0x314 through
  address_space (aliasing 0x1012xxxx to 0x0C12xxxx), in 4-byte units for the FIFO side, then
  qemu_set_irq(dma_irq[ch], 1); +0x308 reads 0; write +0x308 = 0 lowers the IRQ.
```
First milestone: CMD0/1/2/3/9/7/16/13 answered -> the socket reports a card and E: mounts
(reads need the DMA model; writes are the same path with +0x314 = FIFO).
