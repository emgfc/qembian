// Hosted EKA2: memory model private header. Ported from memmodel/emul/plat_priv.h (Copyright (c)
// Nokia Corporation and/or its subsidiary(-ies), EPL v1.0): RAM drive file handles are Linux fds.
#ifndef __M32KERN_H__
#define __M32KERN_H__
#include <kernel/kern_priv.h>
#include <platform.h>

// hardware variant compatibility (as memmodel/epoc/plat_priv.h, EPL, Nokia): ROM root directories and
// images are per variant
class THardwareVariant
	{
public:
	inline THardwareVariant() { iVariant = 0x01000000; }
	inline THardwareVariant(TUint aVariant) { iVariant = aVariant; }
	inline operator TUint() { return iVariant; }
	inline TBool IsIndependent() { return Layer() <= 3; }
	inline TBool IsCpu() { return Parent() == 3; }
	inline TBool IsCompatibleWith(TUint aCpu, TUint aAsic, TUint aVMask)
		{ return (Layer()<=3 || (Parent()==3 && Layer()==aCpu) || (Layer()==aAsic && (VMask() & aVMask)!=0)); }
private:
	inline TUint Layer() { return iVariant >> 24; }
	inline TUint Parent() { return (iVariant >> 16) & 0xff; }
	inline TUint VMask() { return iVariant & 0xffff; }
private:
	TUint iVariant;
	};

struct TRamDriveInfo
	{
	TInt iSize;
	};

class PP
	{
public:
	static TInt RamDriveMaxSize;
	static TLinAddr RamDriveStartAddress;
	static TInt RamDriveFile;		// fd of the RAM drive's backing memfd (-1 if none)
	static TRamDriveInfo* RamDriveInfo;
	};

const TInt KKernelHeapSizeMin = 0x008000;
const TInt KKernelHeapSizeMax = 0xFFC000;

#endif
