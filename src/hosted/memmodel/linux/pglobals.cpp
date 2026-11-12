// Hosted EKA2 port of memmodel/emul/pglobals.cpp (EPL v1.0, Nokia; header above): class names kept,
// Win32 virtual memory calls through lxwin32.h (mmap/mprotect). Other changes are marked "hosted:".
// Copyright (c) 1998-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the License "Eclipse Public License v1.0"
// which accompanies this distribution, and is available
// at the URL "http://www.eclipse.org/legal/epl-v10.html".
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
//
// Description:
// e32\memmodel\emul\pglobals.cpp
// 
//

#include "plat_priv.h"
#include "lxwin32.h"

TInt PP::RamDriveMaxSize;
TLinAddr PP::RamDriveStartAddress;
TInt PP::RamDriveFile = -1;		// hosted: memfd
TRamDriveInfo* PP::RamDriveInfo;
