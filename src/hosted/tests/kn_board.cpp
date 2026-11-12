// kn_board: the hosted board's device models without a phone ROM: the I/O bus, the NVMemory device
// (syborg-nvmem register semantics: transfers, status, interrupt, errors; the drive image mapped
// private, never written back), driver physical RAM and the physical windows. Each case is a regression
// or edge case of src/hosted/variant/hboard.cpp and src/hosted/memmodel/linux/mphys.cpp.
#include <kernel/kern_priv.h>

extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
extern TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage);
extern TUint32 HostedIoRead(TUint32 aAddr);
extern void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
extern TBool HostedIrqRaised(TInt aIrq);
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);

static const TUint32 KIo = 0xC6000000u;
static const TUint32 KNv = KIo + 0x10 * 0x1000;		// device 0x10, as the board's C:
static const TInt KIrq = 12;
enum { ROff = 0x04, RSize = 0x08, RDir = 0x0c, RExec = 0x10, RShm = 0x14, RNvSize = 0x18, RShmSize = 0x1c, RStatus = 0x20 };

// as the driver reaches them: syborg.h ReadReg/WriteReg, base + (register << 2)
static TUint32 Rd(TInt aReg) { return HostedIoRead(KNv + (aReg << 2)); }
static void Wr(TInt aReg, TUint32 aV) { HostedIoWrite(KNv + (aReg << 2), aV); }

static TUint32 Transfer(TUint32 aSector, TUint32 aCount, TUint32 aDir, TPhysAddr aShm, TUint32 aShmSize)
	{
	Wr(ROff, aSector); Wr(RSize, aCount); Wr(RDir, aDir); Wr(RShm, aShm); Wr(RShmSize, aShmSize);
	Wr(RExec, aDir);
	return Rd(RStatus);
	}

int main(int, char**)
	{
	// a 64-sector drive image, sector n filled with byte n
	const char* img = "/tmp/kn_board.img";
	TInt fd = hsys(SYS_openat, -100, img, 0x41 | 0x200 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644);
	HCHECK(fd >= 0, "drive image created (%d)", fd);
	TUint8 sec[512];
	for (TInt n = 0; n < 64; ++n)
		{
		memset(sec, n, sizeof sec);
		hsys(SYS_write, fd, sec, sizeof sec);
		}
	hsys(SYS_close, fd);

	HCHECK(HostedBoardInit(NULL, NULL) == KErrNone, "board: I/O area at %08x", KIo);
	// I/O bus: an address without a device model is plain memory
	HostedIoWrite(KIo + 0x3000, 0x12345678);
	HCHECK(HostedIoRead(KIo + 0x3000) == 0x12345678, "unmodelled device page reads back what was written");
	HCHECK(Epoc::LinearToPhysical(KIo + 0x3004) == 0xC0003004u, "I/O area is the peripheral window (phys %08x)",
	       Epoc::LinearToPhysical(KIo + 0x3004));
	HCHECK(Epoc::LinearToPhysical(0x00001000) == KPhysAddrInvalid, "no window: KPhysAddrInvalid");

	// driver physical RAM
	TPhysAddr a = 0, b = 0;
	HCHECK(Epoc::AllocPhysicalRam(0x40000, a) == KErrNone, "AllocPhysicalRam 256 KB -> %08x", a);
	HCHECK(Epoc::AllocPhysicalRam(0x1000, b, 16) == KErrNone && (b & 0xffff) == 0, "aligned allocation (64 KB) -> %08x", b);
	TPhysAddr c = 0;
	HCHECK(Epoc::AllocPhysicalRam(0x7fffffff, c) == KErrNoMemory, "an allocation larger than the RAM fails");
	HCHECK(Epoc::AllocPhysicalRam(0, c) == KErrNoMemory, "a zero-size allocation fails");
	TUint8* shm = (TUint8*)HostedPhysicalToLinear(a, 0x40000);
	HCHECK(shm != NULL, "the buffer is reachable (lin %08x)", shm);
	HCHECK(Epoc::LinearToPhysical((TLinAddr)shm + 0x200) == a + 0x200, "linear -> physical round trip");

	// NVMemory
	HCHECK(HostedNvmemAttach(0, KNv, KIrq, img) == KErrNone, "NVMemory attached with %s", img);
	HCHECK(Rd(RNvSize) == 64, "NV_MEMORY_SIZE in sectors (%u)", Rd(RNvSize));
	HCHECK(!HostedIrqRaised(KIrq), "no interrupt before a transfer");
	TUint32 st = Transfer(3, 2, 1, a, 0x40000);
	HCHECK(st == 2, "read of 2 sectors: status = sector count (%d)", st);
	HCHECK(shm[0] == 3 && shm[511] == 3 && shm[512] == 4 && shm[1023] == 4, "read data: sectors 3 and 4");
	Wr(ROff, 0);
	Wr(RExec, 1);
	HCHECK(HostedIrqRaised(KIrq), "a transfer raises the interrupt");
	(void)Rd(RStatus);
	HCHECK(!HostedIrqRaised(KIrq), "reading STATUS lowers it");
	memset(shm, 0xab, 512);
	st = Transfer(10, 1, 2, a, 0x40000);
	HCHECK(st == 1, "write of 1 sector (%d)", st);
	memset(shm, 0, 512);
	st = Transfer(10, 1, 1, a, 0x40000);
	HCHECK(st == 1 && shm[0] == 0xab && shm[511] == 0xab, "the write reads back");
	fd = hsys(SYS_openat, -100, img, 0, 0);
	hsys(19 /* lseek */, fd, 10 * 512, 0);
	TUint8 x = 0;
	hsys(SYS_read, fd, &x, 1);
	hsys(SYS_close, fd);
	HCHECK(x == 10, "the image file is unchanged (private mapping): %02x", x);
	HCHECK(Transfer(63, 1, 1, a, 0x40000) == 1, "the last sector reads");
	HCHECK(Transfer(63, 2, 1, a, 0x40000) == 0xffffffffu, "a transfer past the end fails (-1)");
	HCHECK(Transfer(0, 2, 1, a, 512) == 0xffffffffu, "a transfer larger than the shared buffer fails");
	HCHECK(Transfer(0, 1, 3, a, 0x40000) == 0xffffffffu, "an unknown direction fails");
	HCHECK(Transfer(0, 0, 1, a, 0x40000) == 0xffffffffu, "a zero-sector transfer fails");
	HCHECK(Transfer(0, 1, 1, 0x00001000, 0x40000) == 0xffffffffu, "a buffer outside every window fails");
	HCHECK(Rd(ROff) == 0 && Rd(RSize) == 1 && Rd(RDir) == 1, "registers read back");

	// syborg-snd sink: a ring drains at its playback rate, watermark and end-of-stream interrupts
	extern TInt HostedSndAttach();
	HCHECK(HostedSndAttach() == KErrNone, "sound device attached");
	const TUint32 snd = KIo + 0x0e * 0x1000, sl = snd + 0x100;		// slot 0
	HCHECK(HostedIoRead(snd) == 0xc51d0012u, "sound device ID");
	HostedIoWrite(snd + 7 * 4, 1);				// interrupt enable, slot 0
	HostedIoWrite(sl + 1 * 4, 0);				// S16
	HostedIoWrite(sl + 2 * 4, 48000);
	HostedIoWrite(sl + 3 * 4, 2);				// 192000 bytes a second
	HostedIoWrite(sl + 4 * 4, a);
	HostedIoWrite(sl + 5 * 4, 19200);			// 100 ms of ring
	HostedIoWrite(sl + 8 * 4, 9600);			// watermark: half free
	HostedIoWrite(sl + 0 * 4, 1);				// start
	HCHECK(HostedIoRead(sl + 11 * 4) & 1, "slot running after START");
	HostedIoWrite(sl + 6 * 4, 19200);			// a full ring written
	TUint64 t0 = hnow_ns();
	hsleep_us(60000);
	TUint32 rp = HostedIoRead(sl + 7 * 4);
	TUint32 ms = (TUint32)((hnow_ns() - t0) / 1000000);
	HCHECK(rp >= 9600 && rp <= 19200, "READ_POS advanced at the playback rate: %u bytes in %u ms", rp, ms);
	HCHECK(HostedIrqRaised(13) && (HostedIoRead(snd + 6 * 4) & 1), "watermark interrupt raised");
	HostedIoWrite(snd + 6 * 4, 1);				// acknowledge
	HostedIoWrite(sl + 0 * 4, 16);				// EOS
	hsleep_us(150000);
	TUint32 sst = HostedIoRead(sl + 11 * 4);
	HCHECK((sst & 2) && !(sst & 1) && HostedIoRead(sl + 7 * 4) == 19200, "drained after EOS: stopped, EOS flagged (%x)", sst);
	HostedIoWrite(sl + 5 * 4, 4);				// ring too small
	HostedIoWrite(sl + 0 * 4, 1);
	HCHECK(HostedIoRead(sl + 11 * 4) & 8, "an invalid ring is refused (error flag)");

	// syborg-keyboard (hkeyb.cpp, QEMU syborg_input.c semantics) and the phone window's key ring (hdisplay.cpp)
	extern TInt HostedKeybAttach();
	extern void HostedKeybPush(TUint32 aCode);
	extern TInt HostedKeybDropped;
	extern TInt HostedDisplayStart(const char* aPath);
	const TUint32 kb = KIo + 0x03 * 0x1000;
	HCHECK(HostedKeybAttach() == KErrNone, "keyboard attached");
	HCHECK(HostedIoRead(kb) == 0xc51d0002u && HostedIoRead(kb + 4 * 4) == 64, "keyboard ID, FIFO size 64");
	HCHECK(HostedIoRead(kb + 2 * 4) == 0 && HostedIoRead(kb + 1 * 4) == 0xffffffffu, "empty FIFO: count 0, DATA reads -1");
	HostedKeybPush(0x40000010);
	HCHECK(!HostedIrqRaised(2), "no interrupt while INT_ENABLE is 0");
	HostedIoWrite(kb + 3 * 4, 1);
	HCHECK(HostedIrqRaised(2) && HostedIoRead(kb + 3 * 4) == 1, "enabling with a queued key raises the interrupt");
	HostedKeybPush(0xc0000010);
	HCHECK(HostedIoRead(kb + 2 * 4) == 2, "two codes queued");
	HCHECK(HostedIoRead(kb + 4) == 0x40000010 && HostedIoRead(kb + 4) == 0xc0000010, "DATA pops in order (press, release)");
	HCHECK(!HostedIrqRaised(2), "the interrupt drops with the FIFO empty");
	for (TInt i = 0; i < 70; ++i)
		HostedKeybPush(0x40000000 | i);
	HCHECK(HostedIoRead(kb + 2 * 4) == 64 && HostedKeybDropped == 6, "a full FIFO drops (64 kept, %d dropped)", HostedKeybDropped);
	for (TInt i = 0; i < 64; ++i)
		(void)HostedIoRead(kb + 4);
	// the key ring: a pressed entry (bit 31) becomes a press of that scan code, a released one a release.
	// Bug 2026-11-12 (caught before commit): the first ring API returned the entry as TInt with -1 for
	// "empty", and a pressed entry has bit 31 set
	const char* shmPath = "/tmp/kn_board.shm";
	fd = hsys(SYS_openat, -100, shmPath, 0x42 | 0x200, 0644);
	hftruncate(fd, 0x100000);
	hsys(SYS_close, fd);
	HCHECK(HostedDisplayStart(shmPath) == KErrNone, "display shared memory on a plain file");
	fd = hsys(SYS_openat, -100, shmPath, 2, 0);
	volatile TUint32* ring = (volatile TUint32*)hmmap(0, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	hsys(SYS_close, fd);
	HCHECK(ring[0] == 0x42465048u, "header magic written");
	TUint32 head = ring[0x100 / 4];
	ring[0x108 / 4 + head % 64] = 0x80000000u | 0xa4;		// EStdKeyDevice0 pressed
	ring[0x108 / 4 + (head + 1) % 64] = 0xa4;				// released
	ring[0x100 / 4] = head + 2;
	for (TInt i = 0; i < 50 && HostedIoRead(kb + 2 * 4) < 2; ++i)
		hsleep_us(5000);
	HCHECK(ring[0x104 / 4] == head + 2, "the VM consumed the ring (tail %u)", ring[0x104 / 4]);
	TUint32 k1 = HostedIoRead(kb + 4), k2 = HostedIoRead(kb + 4);
	HCHECK(k1 == 0x400000a4u && k2 == 0xc00000a4u, "ring -> keyboard: press %08x, release %08x", k1, k2);
	return htest_done();
	}
