// Hosted EKA2: the phone's screen for a host window. The test VM has an ivshmem device whose memory is a file
// on the host (tools/hosted/htest.py --phone, tools/hosted/phone.py); a host thread here copies the board's
// framebuffer (the phone's video RAM, HostedFbPixels) into it about 30 times a second, only the rows that
// changed, and bumps a frame counter; the host viewer maps the same file and draws it. The same memory carries
// key events the other way (a ring, read by the keypad model). Original work for qembian.
//
// Layout (all little-endian u32):
//   0x000 magic 'HPFB'  0x004 version 1  0x008 width  0x00c height  0x010 pitch (bytes)  0x014 frame counter
//   0x100 key ring: 0x100 head (host writes)  0x104 tail (guest writes)  0x108 + 4 * i: 64 entries
//         (entry: bit 31 pressed, bits 0..15 the key code)
//   0x1000 pixels, XRGB8888, height rows of pitch bytes

#include <kernel/kern_priv.h>

extern const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch);

static const TUint32 KMagic = 0x42465048;		// "HPFB"
static const TUint32 KPixels = 0x1000;
static volatile TUint32* Shm;
static TUint32 ShmBytes;
static TUint32* Shadow;
static TInt ShadowBytes;
static volatile TInt DisplayTid;
volatile TUint32 HostedDisplayFrames;

static void Present()
	{
	TInt w = 0, h = 0, pitch = 0;
	const TUint32* src = HostedFbPixels(w, h, pitch);
	if (!src || w <= 0 || h <= 0 || KPixels + (TUint32)(w * h * 4) > ShmBytes)
		return;
	if (ShadowBytes < w * h * 4)
		{
		TAny* m = hmmap(0, w * h * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (HMAP_FAILED(m))
			return;
		Shadow = (TUint32*)m;
		ShadowBytes = w * h * 4;
		memset(Shadow, 0xff, ShadowBytes);
		}
	Shm[2] = w;
	Shm[3] = h;
	Shm[4] = w * 4;
	TBool any = EFalse;
	TUint32* dst = (TUint32*)((TUint8*)Shm + KPixels);
	for (TInt y = 0; y < h; ++y)
		{
		const TUint32* row = (const TUint32*)((const TUint8*)src + y * pitch);
		TUint32* sh = Shadow + y * w;
		if (!memcmp(sh, row, w * 4))
			continue;
		memcpy(sh, row, w * 4);
		memcpy(dst + y * w, row, w * 4);
		any = ETrue;
		}
	if (any)
		{
		__atomic_thread_fence(__ATOMIC_RELEASE);
		Shm[5] = Shm[5] + 1;			// a new frame: the host redraws
		++HostedDisplayFrames;
		}
	}

TBool HostedDisplayNextKey(TUint32& aEntry);
extern void HostedKeybPush(TUint32 aCode);

// keys every 10 ms (a host thread's poll of the shared ring, no system call), the screen every 30 ms
static int DisplayThread(void*)
	{
	for (TInt n = 0;; ++n)
		{
		for (TUint32 v; HostedDisplayNextKey(v); )
			{
			// ring entry: bit 31 pressed, bits 0..15 the Symbian scan code -> the keyboard's code
			// (bit 30: already a scan code; bit 31: released)
			HostedKeybPush(0x40000000u | (v & 0xff) | ((v & 0x80000000u) ? 0 : 0x80000000u));
			}
		if (n % 3 == 0)
			Present();
		hsleep_us(10000);
		}
	}

// the key ring in the same memory: the next entry, EFalse when empty
TBool HostedDisplayNextKey(TUint32& aEntry)
	{
	if (!Shm)
		return EFalse;
	TUint32 head = Shm[0x100 / 4], tail = Shm[0x104 / 4];
	if (head == tail)
		return EFalse;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	aEntry = Shm[0x108 / 4 + tail % 64];
	__atomic_thread_fence(__ATOMIC_RELEASE);
	Shm[0x104 / 4] = tail + 1;
	return ETrue;
	}

// show the phone's screen in the shared memory at aPath (the ivshmem BAR's sysfs resource file)
TInt HostedDisplayStart(const char* aPath)
	{
	TInt fd = hsys(SYS_openat, -100, aPath, 2 /* O_RDWR */, 0);
	if (fd < 0)
		return KErrNotFound;
	TInt size = hsys(19 /* lseek */, fd, 0, 2);
	if (size <= 0)
		size = 0x100000;
	TAny* m = hmmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	hsys(SYS_close, fd);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	Shm = (volatile TUint32*)m;
	ShmBytes = size;
	Shm[0] = KMagic;
	Shm[1] = 1;
	Shm[5] = 0;
	Shm[0x104 / 4] = Shm[0x100 / 4];		// keys pressed before boot are dropped
	hprintf("HOSTED: display in shared memory %s (%u bytes)\n", aPath, ShmBytes);
	TAny* stack = hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(stack) || hthread(&DisplayThread, NULL, stack, 0x10000, &DisplayTid) < 0)
		return KErrNoMemory;
	return KErrNone;
	}
