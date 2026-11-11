// mobilecrashdriver.ldd stand-in (see nokiastub.h): Nokia's MobileCrash driver (crash data from the
// previous run, kept in reserved RAM on the phone). mc_useragent.exe loads it by name and opens
// its channel right after MobileCrashStarter starts it (device name "MINKDA"); without the driver it used the unopened
// channel and died KERN-EXEC 0 (on the real phone it keeps running: docs/real-phone-tracing.md). Our
// board has no crash data: controls return KErrNone, requests stay pending (a crash that never comes).
// Every call is logged (SYBSTUB) so its protocol can be read off a boot log.
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD("MINKDA", KCtl, 1)	// the file is mobilecrashdriver.ldd, the device it installs is "MINKDA" [V: mc_useragent DoCreate literal 0x80a000e0]
