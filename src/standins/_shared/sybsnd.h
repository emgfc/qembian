// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// syborg-snd (QEMU src/qemu-syborg/hw/arm/syborg_audio.c): register map for the stand-ins that forward
// sound to it (ehwa: TI DSP bridge phones; aamdaifdriver: RAPIDO phones). 16 slots, each a guest ring
// that the host plays, mixes and paces (docs/research/sound-device-architecture.md section 3).
#ifndef SYBSND_H
#define SYBSND_H
#include "syborg.h"

namespace SybSnd
	{
	const TUint32 KId = 0xc51d0012;			// v2 (v1 ...10 refused; ...11 is syborg-gl's)
	const TInt KSlots = 16;
	const TInt KIrq = 13;						// the board's pic[13] (no baseport enum)
	enum { RId = 0x00, RCaps = 0x04, RClockLo = 0x08, RMaster = 0x10, RRoute = 0x14, RIntStatus = 0x18,
		   RIntEnable = 0x1c };
	enum { SCtrl = 0x00, SFormat = 0x04, SRate = 0x08, SChannels = 0x0c, SRingAddr = 0x10, SRingSize = 0x14,
		   SWritePos = 0x18, SReadPos = 0x1c, SWatermark = 0x20, SGain = 0x24, SClass = 0x28, SStatus = 0x2c,
		   SFramesLo = 0x30, SPlanar = 0x38 };
	enum { CStart = 1, CStop = 2, CPause = 4, CResume = 8, CEos = 16, CFlush = 32 };
	enum { FS16 = 0, FS8 = 1, FALaw = 2, FMp3 = 3, FAac = 4, FS24in32 = 5, FS32 = 6, FSwap16 = 0x100 };
	enum { StRun = 1, StEos = 2, StUnder = 4, StDecErr = 8, StWm = 16 };

#ifdef __HOSTED__
	// the hosted EKA2 board: registers through its I/O bus (src/hosted/variant/hsnd.cpp). Bug 2026-11-12:
	// the direct accesses below bypassed it, the sound model never saw a stream and the start-up tone hung.
	struct TReg
		{
		TUint32 iAddr;
		inline operator TUint32() const { return HostedIoRead(iAddr); }
		inline TReg& operator=(TUint32 aValue) { HostedIoWrite(iAddr, aValue); return *this; }
		};
	inline TReg G(TUint aOff) { TReg r = { KHwSVPAudioDevice + aOff }; return r; }
	inline TReg S(TInt aSlot, TUint aOff) { TReg r = { KHwSVPAudioDevice + 0x100 + aSlot * 0x40 + aOff }; return r; }
#else
	inline volatile TUint32& G(TUint aOff) { return *(volatile TUint32*)(KHwSVPAudioDevice + aOff); }
	inline volatile TUint32& S(TInt aSlot, TUint aOff) { return *(volatile TUint32*)(KHwSVPAudioDevice + 0x100 + aSlot * 0x40 + aOff); }
#endif
	inline TBool Present() { return G(RId) == KId; }
	}
#endif
