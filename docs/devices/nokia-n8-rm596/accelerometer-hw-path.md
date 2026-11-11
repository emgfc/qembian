# N8 accelerometer: running Nokia's own accelerometer.ldd/.pdd on an emulated sensor

Research note (2026-11-11), mock-level only, static analysis of the firmware (no boot). Firmware: N8
RM-596 011.012 (core ROM), 701 RM-774 113.010 and 808 RM-807 113.010 checked quickly, N95 8GB RM-320
35.0.001 checked quickly. Sources: `build/roms/<id>/<fw>/symindex.json` (import thunks resolved
to export ordinals), Ghidra output `build/roms/nokia-n8-rm596/RM-596_011.012_01.01+00.01/ghidra/
{accelerometer.pdd,i2c0.dll,egpio.dll,GAPE.DLL}.c`. Claims are marked [verified] (read in the code)
or [inferred]. The current approach (our own LDD/PDD stand-in) is in `accelerometer.md` and
docs/hacks.md H-accel-standin.

No names come from the SDK: i2c0.dll, egpio.dll, PowerResourceManager.dll (61 exports, not the
29 of the open-source `resman.def`) and GAPE.DLL are Nokia's own, and their import libraries are not
in any of our SDKs. The names below come from the code.

## 1. Who imports what (N8) [verified]

| module | imports |
|--------|---------|
| accelerometer.ldd (9.7 KB) | ekern.exe (38) only |
| accelerometer.pdd (38 KB) | ekern (54), egpio (10), PowerResourceManager (4), i2c0 (3), GAPE (2) |
| i2c0.dll (9 exports) | ekern (22), karap (4: ord 24, 34, 36, 38), PowerResourceManager (2: ord 18, 26) |
| egpio.dll (54 exports) | ekern (6), karap (5: ord 34, 35, 36, 37, 38) |
| PowerResourceManager.dll (61 exports) | ekern (52) only |
| GAPE.DLL (2 exports) | ekern (5) only |

PDD imports by ordinal:
- egpio: 1, 3, 5, 36, 37, 39, 40, 42, 44, 45
- PowerResourceManager: 2, 18, 20, 26
- i2c0: 3, 4, 5
- GAPE: 1, 2

The 701 and 808 accelerometer.pdd import exactly the same ordinals, and their i2c0/egpio import the
same karap ordinals [verified]. The PDD contains the same chip classes in all three
(`DAccPddSensorADXL340`, `DAccPddSensorLIS302DL`, `DAccChipPdd`, `DAccPddPowerHandlerPlatform`)
[verified]. The LDD only needs ekern, so it is not the hard part. The PDD's 54 ekern imports must
resolve against our ekern, though. No Nokia kernel-side driver runs on the N8 build yet:
kernel-drivers.txt lists 9 stand-ins and 34 missing. The accelerometer would be the first, so check
its ekern ordinals first ([inferred] risk).

## 2. The PDD's view of its dependencies

### 2.1 GPIO (egpio.dll): one pin, GPIO 12
- Pin id: the PDD exports a constant (ordinal 2, value **0x0c** = 12) and passes it to egpio ord1
  [verified]. Nothing in the PDD refers to GPIO 44 [verified for the constant block; the second
  schematic line is inferred to be unused, or INT2].
- Call sequences, from FUN_801e0694 / 0630 / 07d4 / 083c / 08b8 / 092c [verified order, names inferred]:
  ```
  init:     h = ord1(12)               // reserve: returns pin<<8 (1 for pin 0), -1 if taken -> PDD returns -8
            ord5(h)                    // set up the pin (writes 1<<bit to a bank register)
            TPin p; ord45(&p, h)       // 20-byte pin object on the stack: +8 handle, +0xc bit, +0x10 bank
            ord39(&p, 1, 2)            // direction/trigger set-up (input, trigger type 2) [inferred]
            ord40(&p, 0)               // interrupt disable
            ord37(&p, 0, 0)            // wake-up config (egpio's backend is EMPTY on the N8 [verified])
            ord45(&p, h); ord44(&p)    // unbind any old ISR
            ord42(&p, Isr, this)       // bind ISR void Isr(TAny*); KErrNone expected
  enable:   ord45; ord36(&p); ord40(&p, 1); ord37(&p, 1, 1)   // ack, enable, wake on
  disable:  ord45; ord40(&p, 0); ord37(&p, 0, 0)
  ack:      ord45; ord36(&p)           // clear the pending interrupt (write 1<<bit to bank reg +0x38) [inferred]
  release:  ord45; ord44(&p); ord3(h)  // unbind, free the reservation bit
  ```
- ISR (FUN_801e05f4): only queues a DFC (`TDfc::Add` on this+400) [verified]. So egpio calls the
  ISR in interrupt context with the bound argument.
- egpio internals (not needed for option a): reservation bitmap at kernel data 0xc8005c28,
  per-bank ISR table at 0xc8004028 (0x400 per bank, 0x20 per pin), bank descriptors of 0x68 bytes
  (register offsets at +0x24/+0x38/+0x40/+0x44/+0x4c/+0x50/+0x5c, MMIO base at +0x60). The bank
  MMIO bases were not resolved. The registers are set/clear style (writes of 1<<bit) [verified shape].

### 2.2 I2C (i2c0.dll): queued transfers with a completion callback
The PDD keeps 3 transfer slots of 0x34 bytes (DAT_801e09e0) and passes `slot+4` (the transfer T) to i2c0.
Layout of T [verified for the fields the PDD and i2c0 touch; meanings inferred where noted]:

| off | type | set by | value / meaning |
|-----|------|--------|-----------------|
| 0x00, 0x04 | SDblQueLink | i2c0 | queue link |
| 0x08 | TUint16 | PDD | 7-bit slave address (0x1d or 0x1c) |
| 0x0c | TInt | PDD | 0 [inferred: flags] |
| 0x10 | TInt | PDD | 3 [inferred: bus speed or config id] |
| 0x14 | TInt (*)(TAny*) | PDD | completion callback (i2c0 calls `cb(*(T+0x18))`, i2c0.c:1426) |
| 0x18 | TAny* | PDD | callback argument (the slot) |
| 0x1c | TInt | i2c0 | kind: 0 = write (ord4); 1 = read only, 2 = write then read (ord3; 1 if the write descriptor is NULL or empty) |
| 0x20 | TDes8* | PDD | bytes to write (register sub-address first) |
| 0x24 | TDes8* | PDD | read buffer; its Length() = bytes to read |
| 0x28 | TInt* | PDD | result word [inferred: i2c0 writes the transfer result here] |

- **ord4(T)** queue a write. **ord3(T)** queue a write+read, a repeated-start read of the register
  [inferred]. Both return the queueing error (KErrNone), append T to the controller queue and
  start its thread "I2CThread_0" [verified].
- **ord5(T)** cancel: -14 (KErrInUse) if T is the active transfer, otherwise unlink it and return 0
  [verified].
- i2c0 panics if its global controller (0xc800ac00 +4) is not set up, so it must have run as a
  kernel extension before the PDD loads [verified panic checks].
- PDD use is synchronous: set the busy flag, queue, `Kern::SemaphoreWait(sem)`. The callback
  signals the semaphore only if the flag is set [verified]. So a stand-in may also run the callback
  before ord3/ord4 return, as long as T+0x28 is written first [inferred].

### 2.3 Power (PowerResourceManager.dll): one resource, 0x31
[verified call shapes, names inferred from the open-source PRM API]
- ord2(&cbObj, Fn, arg, 7): construct a TPowerResourceCb (fn, arg, priority 7).
- ord18(&clientId, name): RegisterClient. Returns KErrNone.
- ord26(clientId, **0x31**, level 2 (on) / 0 (off), NULL, 0): ChangeResourceState, synchronous.
  Resource 0x31 = the accelerometer supply [inferred].
- ord20(clientId): DeRegisterClient.
- i2c0 itself uses ord18 + ord26(client, **0x11**, 1/0): the I2C0 clock or power [inferred].

### 2.4 GAPE.DLL: a named global pointer registry
- ord1(key) -> TAny* (0 if unset), ord2(key, TAny*) -> TInt [verified: GAPE_ord1 looks through 37
  slots, the keys are names like "Keypad", "Light", "Accelerometer", "TouchIC"]. The PDD looks up
  its chip singleton and registers it if it is missing.
- GAPE imports only ekern, so Nokia's own GAPE.DLL can probably run unmodified [inferred].

## 3. The sensor protocol on I2C [register traffic verified; chip meanings from the public LIS302DL map]

The PDD probes two chips. Sub-address bit 7 means auto-increment.

**ADXL340 class** (chip A; register names in the N95 PDD's debug strings: WHOAMI, CTL, DATAX/Y/Z,
INTCONTROL, INTCONTROL2, INTSOURCE, THRESHG, THRESHC, OFSX/Y/Z, DUR, LATENT, INTVAL, MOREINFO):
- address 0x1d. Read 0x0F (1 byte) == **0x4A** ('J') -> present [verified].
- uses registers 0x11, 0x12-0x14, 0x1c, 0x1d and 0x21 (3-byte write); data is 3 bytes from 0x15
  (0x95) [verified]. Not modelled: answer WHO_AM_I with something else and this class drops out.

**LIS302DL class** (the one to model):
- probe: address **0x1d**, read 0x0F == **0x3B** (';'). If not, retry at **0x1c**. If neither
  answers, the PDD marks the chip absent [verified].
- after a match it reads reg 0x32 (1 byte): ==1 -> mode value 0x20, otherwise 0x10 [verified; meaning
  inferred: a clone/variant check, reset value 0 is fine].
- writes [verified]: 0xa0 + 3 bytes (CTRL_REG1..3), 0x20 (CTRL_REG1), 0x22 (CTRL_REG3), 0x38
  CLICK_CFG = 0x6a / 0x55 / 0, 0xbb + 5 bytes (CLICK_THSY_X..CLICK_WINDOW 0x3B-0x3F), FF_WU
  registers by variable address (pairs written with 0x80 auto-increment).
- reads: 0x39 CLICK_SRC (1 byte), FF_WU_SRC by variable address (1 byte), **sample = 0xa9, 5
  bytes** = OUT_X, (0x2A), OUT_Y, (0x2C), OUT_Z, signed 8-bit [verified read; layout from the LIS302DL
  map].
- interrupt: the chip's INT line on GPIO 12. The ISR -> DFC -> I2C reads (the source regs, then
  the sample) [inferred]. Whether streaming is paced by data-ready on INT1 or by a timer was not
  decoded. A model that raises INT1 per CTRL_REG3 (data-ready, FF/WU, click) covers both.

LIS302DL model: about 25 registers. 0x0F RO 0x3B. 0x20-0x22 RW. 0x23 HP reset. 0x27 STATUS (ZYXDA).
0x29/0x2B/0x2D OUT. 0x30-0x37 FF_WU (SRC read-clears). 0x38-0x3F click (0x39 RO, read-clears).
Auto-increment on bit 7. Scale 18 mg/digit at ±2 g (FS=0). Fill the gravity vector from the
existing syborg-sensor `accel` property. The PDD's 248-byte control-0 block then comes from Nokia's
own code. FUN_801dd5cc fills it: word0 = address | const, ranges 2000/8000, and so on.

## 4. What the bus DLLs need below them

- **i2c0.dll** -> karap ord 34 (Bind), 36 (Enable), 38 (Disable) on interrupt id **0x96**, and ord24
  (register modify, args (addr, clear, set)) [verified calls; names inferred from the `Interrupt::`
  export order SetPriority=33, Bind=34, Clear=35, Enable=36, Unbind=37, Disable=38: the
  Disable-at-ISR-entry / Enable-at-exit pattern fits]. The I2C0 registers are at fixed kernel virtual
  addresses: STAT 0xc6032208, IE 0xc6032204 (16-bit), plus 0xc6033010. The STAT bits tested are
  1/2/4/8/0x10 = OMAP I2C's AL/NACK/ARDY/RRDY/XRDY [verified bits, OMAP layout inferred]. So the
  RAPUYAMA I2C is OMAP-like.
- **egpio.dll** -> karap ord 34-38 (Bind, Clear, Enable, Unbind, Disable) for its bank interrupts, and
  direct MMIO through the bank descriptors (bases not resolved).
- Both rely on karap's MMU map, set up by Nokia's bootstrap (0xc603xxxx). Our Syborg bootstrap does
  not create those mappings.

## 5. Options and sizes

**(a) Our own i2c0 / egpio / PowerResourceManager stand-ins, Nokia LDD+PDD unmodified (recommended).**
- i2c0.dll: 9 exports. Real: 3, 4, 5. Run the transfer through a new simple QEMU I2C master
  (MMIO: address, write FIFO, read count, go, status; done synchronously or from a DFC), write the
  result, call T+0x14(T+0x18). Keep a queue so the cancel in ord5 works. The other 6 exports stub
  to KErrNotSupported. About 250 lines.
- egpio.dll: 54 exports. Real: 1, 3, 5, 36, 37, 39, 40, 42, 44, 45. Reserve bitmap, pin object,
  one ISR slot per pin, enable/ack mapped to one Syborg IRQ line (or a tiny syborg-gpio block) that
  the QEMU LIS302DL INT1 drives. 37 and 39 can be no-ops. The rest stub. About 250 lines.
- PowerResourceManager.dll: 61 exports. Real: 2 (fill the cb object), 18 (client id counter), 20,
  26 (KErrNone; optionally tell QEMU "sensor powered"). The rest stub. About 100 lines.
- GAPE.DLL: try Nokia's own (ekern only); otherwise a 2-function, 37-slot registry, about 40 lines.
- QEMU: an I2C master MMIO device on QEMU's i2c bus core, about 150 lines, plus a LIS302DL
  `I2CSlave` with about 25 registers and an INT1 qemu_irq, about 250 lines. It reads the vector the
  window's Turn view already sets.
- All four must be in ROM as kernel extensions or XIP kernel DLLs, initialised before accssy loads the
  PDD (i2c0 panics if its controller isn't set up).
- Non-Nokia code left on the kernel side: those 3 or 4 DLLs, which only forward. The driver logic
  (chip probe, power, calibration block, ring buffer, request protocol, the 248-byte config) is all
  Nokia's.
- Exported-ordinal *count* must match (9/54/61/2), and the DEF NONAME ordinals must equal Nokia's.
  Only the PDD's subset needs real behaviour while no other Nokia kernel driver imports them.

**(b) Emulate the RAPUYAMA I2C/GPIO in QEMU and run Nokia's i2c0.dll / egpio.dll too.**
- karap.dll cannot run, but a shim "karap.dll" with ord 24 and 33-38 is small: it maps Nokia IRQ ids
  (0x96, the GPIO bank ids) to Syborg IRQs. The bootstrap must also map 0xc6032000/0xc6033000 and
  the GPIO bank bases to QEMU regions at the same virtual addresses.
- QEMU: an OMAP-style I2C controller. QEMU's omap_i2c might fit if the revision and FIFO usage match,
  which was not checked. The RAPUYAMA GPIO bank model has registers that are not decoded. Nokia's
  PowerResourceManager PSL (inside the DLL, ekern-only) probably talks to the PMIC in ways we have
  not decoded, so PRM still needs a stand-in.
- More unknowns, about twice the work of (a). The only gain is that i2c0/egpio become Nokia's.
  Worth it only once several Nokia I2C/GPIO drivers (touch, ALS, keypad expander) should run.

## 6. N95 8GB (RM-320 35.0.001; RM-159 is the same) [verified imports]
It has `accelerometerpdd.pdd` (28 KB, 1 export) and `accelerometerldd.ldd` (3 KB, ekern 39 only).
The PDD imports ekern (63), **kaomap.dll (12: ord 9, 11, 13, 59, 60, 62, 63, 65, 67, 68, 102,
110)**, **i2c2_master.dll (2: ord 2, 3)** and **power.dll (4: ord 13, 16, 18, 20)**. i2c_master /
i2c2_master import power.dll and kaomap (101, 103, 106, 126, 128, 130). The PDD has the same classes
(DAccPddSensorADXL340 / DAccPddSensorLIS302DL) and LIS302DL register-name debug strings (CTRL_REG1-3,
FF_WU_*, CLICK_*, OUT_X/Y/Z, WHOAMI). So the chip protocol is the same family on the OMAP2420's
second I2C bus. The difference is that this PDD calls the ASSP (kaomap: GPIO and interrupts
[inferred]) directly. Option (a) there is a stand-in kaomap.dll with those 12 ordinals (only if no
name clash with our N95 kernel side, not checked), an i2c2_master with 2 ordinals and a power.dll
with 4. The same QEMU LIS302DL model serves it.

## 7. Step plan (option a)
1. ABI check without booting: resolve accelerometer.ldd (38) and .pdd (54) ekern ordinals against our
   N8 ekern (`tools/personality/check.py --device nokia-n8-rm596` or kabi). Stop if they need patches.
2. QEMU: `syborg-i2c` master (MMIO, IRQ optional) + `lis302dl` I2C slave fed by the syborg-sensor
   vector, with INT1 to a Syborg IRQ line. Unit-test it by poking registers from the qtest/HMP side.
3. Stand-ins `src/standins/{i2c0,egpio,prm,gape}` with NONAME DEF files that reproduce Nokia's ordinal
   numbering and export counts. ROM them as extensions in the N8 profile (`[[standins]]`).
4. Profile variant: take Nokia's accelerometer.ldd/.pdd from the firmware (drop them from
   missing_drivers) instead of `src/standins/accelerometer`. Keep the current stand-in as fallback.
5. sydb session: check the I2C log (WHO_AM_I at 0x1d, CTRL writes), DoCreate and control 0 (Nokia's
   248-byte block), request 3 samples, then Turn view -> landscape.
6. 701/808: same DLL set and ordinals [verified]. Re-check their i2c0/egpio export counts, then
   reuse the stand-ins.
