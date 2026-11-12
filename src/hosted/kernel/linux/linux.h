// Hosted EKA2: platform kernel header (replaces include/kernel/win32/win32.h, EPL, Nokia).
#ifndef __LINUX_KERN_H__
#define __LINUX_KERN_H__
#include <e32const.h>
#include <plat_priv.h>
#include <assp.h>

class Hosted
	{
public:
	static Asic* TheAsic;
	};
#endif
