// Hosted EKA2: Linux thread names for Symbian threads (called by DThread::EpocThreadFunction, kplat.cpp).
// Original work for qembian. Kept out of kplat.cpp: its inline-asm literal pools must stay in range.
#include <kernel/kern_priv.h>

// The host thread takes the Symbian thread's name (Linux comm, 15 characters: top/ps in the VM show the
// Symbian machine's threads): "Process" for a process's main thread, "Proc:Thread" for the others, the
// thread name for kernel threads. Names are cut at '.' and '[' ("EwSrv.exe[100039e3]0001" -> "EwSrv").
static TInt BaseName(const TDesC& aName, char* aOut, TInt aMax)
	{
	TInt n = 0;
	for (TInt i = 0; i < aName.Length() && n < aMax; ++i)
		{
		TUint c = aName[i];
		if (c == '.' || c == '[')
			break;
		aOut[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
		}
	return n;
	}

void HostedNameHostThread(DThread& aThread)
	{
	char comm[16];
	TInt n = 0;
	TKName name;
	if (aThread.iThreadType == EThreadUser && aThread.iOwningProcess)
		{
		aThread.iOwningProcess->Name(name);
		n = BaseName(name, comm, 15);
		aThread.Name(name);
		if (!(aThread.iFlags & KThreadFlagOriginal) && n < 14)
			{
			comm[n++] = ':';
			n += BaseName(name, comm + n, 15 - n);
			}
		}
	else
		{
		aThread.Name(name);
		n = BaseName(name, comm, 15);
		}
	comm[n] = 0;
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, comm);
	}

