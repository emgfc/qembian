// Hosted EKA2 nanokernel, Linux platform: globals.
// Ported from e32/nkern/win32/ncglob.cpp (Copyright (c) 1998-2009 Nokia Corporation and/or its
// subsidiary(-ies), Eclipse Public License v1.0).
#include "nk_priv.h"

TScheduler TheScheduler;
LinuxInterrupt TheInterrupt;
SBTraceData BTraceData = { {0}, 0, 0 };
