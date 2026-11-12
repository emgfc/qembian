// Hosted EKA2 tests: load the phone's composed core ROM (shared read-only by htest --rom, never in the
// repo) at its link address. /t/nk_rom.args (written by htest) gives the file, base, size, EUSER's
// export directory and its exec stub block. Returns EFalse when no ROM was given (the test SKIPs).
#pragma once
#include <hrt.h>

struct SRomArgs
	{
	TUint32 iBase, iSize, iExpDir, iStubLo, iStubHi;
	TUint32 iPersonality;		// registry id of the exec personality (0 = native numbering)
	char iRofs[128];			// the composed ROFS flash image (board flash window), or empty
	char iBoardConfig[512];		// the device's syborg-platform.config string
	char iPath[128];
	char iNvmem[128];			// C: drive image (seed), or empty
	char iNvmem2[128];			// E: drive image (seed), or empty
	TUint32 iFbWidth, iFbHeight;	// the framebuffer's size (syborg-fb.width/height), 0 = none
	char iReplies[160];			// the fake modem's reply table, or empty
	};

static TUint32 RomHex(const char*& p)
	{
	TUint32 x = 0;
	for (; *p && *p != ' ' && *p != '\n'; ++p)
		x = x * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
	if (*p == ' ') ++p;
	return x;
	}

static TUint32 RomDec(const char*& p)
	{
	TUint32 x = 0;
	for (; *p >= '0' && *p <= '9'; ++p)
		x = x * 10 + (*p - '0');
	return x;
	}

static TBool RomReadArgs(SRomArgs& a)
	{
	char buf[2048];
	memset(&a, 0, sizeof(a));
	TInt fd = hsys(SYS_openat, -100, "/t/nk_rom.args", 0, 0);
	if (fd < 0)
		return EFalse;
	TInt n = hsys(SYS_read, fd, buf, sizeof(buf) - 1);
	hsys(SYS_close, fd);
	if (n <= 0)
		return EFalse;
	buf[n] = 0;
	for (const char* p = buf; *p; )
		{
		const char* k = p;
		while (*p && *p != ' ') ++p;
		TInt kl = p - k;
		if (*p) ++p;
		if (kl == 3 && !memcmp(k, "rom", 3))
			{
			TInt i = 0;
			while (*p && *p != '\n' && i < 120) a.iPath[i++] = *p++;
			a.iPath[i] = 0;
			}
		else if (kl == 4 && !memcmp(k, "base", 4)) a.iBase = RomHex(p);
		else if (kl == 4 && !memcmp(k, "size", 4)) a.iSize = RomHex(p);
		else if (kl == 12 && !memcmp(k, "euser_expdir", 12)) a.iExpDir = RomHex(p);
		else if (kl == 5 && !memcmp(k, "stubs", 5)) { a.iStubLo = RomHex(p); a.iStubHi = RomHex(p); }
		else if (kl == 11 && !memcmp(k, "personality", 11)) a.iPersonality = RomHex(p);
		else if (kl == 4 && !memcmp(k, "rofs", 4))
			{
			TInt i = 0;
			while (*p && *p != '\n' && i < 120) a.iRofs[i++] = *p++;
			a.iRofs[i] = 0;
			}
		else if (kl == 3 && !memcmp(k, "fbw", 3)) a.iFbWidth = RomDec(p);
		else if (kl == 3 && !memcmp(k, "fbh", 3)) a.iFbHeight = RomDec(p);
		else if (kl == 7 && !memcmp(k, "replies", 7))
			{
			TInt i = 0;
			while (*p && *p != '\n' && i < 150) a.iReplies[i++] = *p++;
			a.iReplies[i] = 0;
			}
		else if ((kl == 5 && !memcmp(k, "nvmem", 5)) || (kl == 6 && !memcmp(k, "nvmem2", 6)))
			{
			char* d = kl == 5 ? a.iNvmem : a.iNvmem2;
			TInt i = 0;
			while (*p && *p != '\n' && i < 120) d[i++] = *p++;
			d[i] = 0;
			}
		else if (kl == 11 && !memcmp(k, "boardconfig", 11))
			{
			TInt i = 0;
			while (*p && *p != '\n' && i < 510) a.iBoardConfig[i++] = *p++;
			a.iBoardConfig[i] = 0;
			}
		while (*p && *p != '\n') ++p;
		if (*p) ++p;
		}
	return a.iPath[0] && a.iBase && a.iSize && a.iExpDir && a.iStubLo < a.iStubHi;
	}

static TBool RomLoad(const SRomArgs& a)
	{
	TInt fd = hsys(SYS_openat, -100, a.iPath, 0, 0);
	if (fd < 0)
		return EFalse;
	TUint8* m = (TUint8*)hmmap((void*)a.iBase, a.iSize, PROT_READ | PROT_WRITE | PROT_EXEC,
	                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TUint32)m != a.iBase)
		return EFalse;
	TUint32 done = 0;
	while (done < a.iSize)
		{
		TInt r = hsys(SYS_read, fd, m + done, a.iSize - done > 0x100000 ? 0x100000 : a.iSize - done);
		if (r <= 0) break;
		done += r;
		}
	hsys(SYS_close, fd);
	hsys(SYS_mprotect, m, a.iSize, PROT_READ | PROT_EXEC);		// ROM: read-only, as on the phone
	return done == a.iSize;
	}
