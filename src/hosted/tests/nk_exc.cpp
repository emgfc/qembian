// nk_exc: hardware exceptions on a nanothread reach its exception handler (SNThreadHandlers::
// iExceptionHandler, the kernel's Exc::Dispatch in a full boot) through the signal handler and the
// exception trampoline (nkern/linux/ncthrd.cpp, as the Win32 emulator's NThread::Exception).
// Regression for 2026-11-12: faults were dumped and the process exited; a kernel-side IPC read of a bad
// client descriptor (FbServ, address 0x140) killed the hosted phone where the ARM kernel's exception
// trap returns an error. Cases: an ARM and a Thumb load from an unmapped address; the handler sees the
// signal, the address and the registers, changes r0 and the pc (skips the load); execution resumes with
// every other register, the flags and the instruction set intact.
#include <nk_priv.h>

static NThread Initial;
static volatile TInt Calls;
static volatile TUint32 SeenSig, SeenAddr, SeenR4, SeenFlags;
extern volatile TUint32 HostedExceptions;

static void ExcHandler(TAny* aContext, NThread*)
	{
	TWin32ExcInfo& x = *(TWin32ExcInfo*)aContext;
	++Calls;
	SeenSig = x.iExcCode;
	SeenAddr = x.iExcDataAddress;
	SeenR4 = x.iR[4];
	SeenFlags = x.iFlags;
	x.iR[0] = 0x600d0000 + Calls;		// the "result" of the faulting load
	x.iPc += (x.iCpsr & 0x20) ? 2 : 4;	// skip it (a 16-bit Thumb or a 32-bit ARM ldr)
	}

static const SNThreadHandlers Handlers = { NULL, NULL, &ExcHandler, NULL };

// r0 = *(0x140) with r4..r7 and the flags set beforehand; returns r0, the r4..r7 sum and the Z flag
__attribute__((noinline, target("arm"))) static TUint32 ArmFault(TUint32& aSum, TUint32& aZ)
	{
	TUint32 r0, sum, z;
	__asm__ volatile(
		"	mov	r4, #0x11\n	mov	r5, #0x22\n	mov	r6, #0x44\n	mov	r7, #0x88\n"
		"	mov	r0, #0x140\n"
		"	cmp	r0, r0\n"				// Z = 1
		"	ldr	r0, [r0]\n"				// faults
		"	mov	%1, #0\n	moveq	%1, #1\n"	// Z kept?
		"	add	%2, r4, r5\n	add	%2, %2, r6\n	add	%2, %2, r7\n"
		"	mov	%0, r0\n"
		: "=r"(r0), "=&r"(z), "=&r"(sum) : : "r0", "r4", "r5", "r6", "r7", "cc", "memory");
	aSum = sum;
	aZ = z;
	return r0;
	}

__attribute__((noinline, target("thumb"))) static TUint32 ThumbFault(TUint32& aSum)
	{
	TUint32 r0, sum;
	__asm__ volatile(
		"	movs	r4, #0x11\n	movs	r5, #0x22\n"
		"	movs	r0, #0x14\n	lsls	r0, r0, #4\n"	// 0x140
		"	ldr	r0, [r0]\n"				// 16-bit Thumb load: faults
		"	adds	%1, r4, r5\n"
		"	mov	%0, r0\n"
		: "=r"(r0), "=&r"(sum) : : "r0", "r4", "r5", "cc", "memory");
	aSum = sum;
	return r0;
	}

int main(int, char**)
	{
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	info.iHandlers = &Handlers;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	TUint32 sum = 0, z = 0;
	TUint32 r = ArmFault(sum, z);
	HCHECK(Calls == 1 && HostedExceptions == 1, "an ARM load from 0x140 reached the exception handler (%d)", Calls);
	HCHECK(SeenSig == SIGSEGV && SeenAddr == 0x140, "signal %u, data address %08x", SeenSig, SeenAddr);
	HCHECK(SeenR4 == 0x11, "the record holds the faulting registers (r4 %x)", SeenR4);
	HCHECK(r == 0x600d0001, "resumed after the load with the handler's r0 (%08x)", r);
	HCHECK(sum == 0xff, "r4-r7 intact (%x)", sum);
	HCHECK(z == 1, "flags intact (Z)");
	HCHECK(SeenFlags == TWin32ExcInfo::EExcInKernel, "the initial thread runs 'in the kernel': EExcInKernel (%x)", SeenFlags);
	r = ThumbFault(sum);
	HCHECK(Calls == 2, "a Thumb load faulted too");
	HCHECK(r == 0x600d0002 && sum == 0x33, "resumed in Thumb state after the 16-bit load (r0 %08x, sum %x)", r, sum);
	HCHECK(!TheScheduler.iKernCSLocked, "the kernel is unlocked again");
	// ARM11's CLZ with zero should-be-one fields (0xe160201e, `clz r2, lr`, N95 AknIconServer): emulated
	extern volatile TUint32 HostedArm11Emulated;
	static const TUint32 code[] = { 0xe52de004u /* push {lr} */, 0xe3a0e801u /* mov lr, #0x10000 */,
		0xe160201eu /* clz r2, lr (SBO fields 0) */, 0xe1a00002u /* mov r0, r2 */, 0xe49de004u /* pop {lr} */,
		0xe12fff1eu /* bx lr */ };
	TUint32* buf = (TUint32*)hmmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memcpy(buf, code, sizeof code);
	hsys(SYS_ARM_cacheflush, (TUint32)buf, (TUint32)buf + sizeof code, 0);
	TUint32 lz = ((TUint32 (*)())buf)();
	HCHECK(lz == 15 && HostedArm11Emulated == 1, "ARM11 CLZ encoding with zero SBO fields emulated: clz(0x10000) = %u", lz);
	HCHECK(Calls == 2, "it did not reach the exception handler");
	return htest_done();
	}
