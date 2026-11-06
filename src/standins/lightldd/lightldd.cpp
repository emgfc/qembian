// N95 LightLdd.ldd stand-in (see nokiastub.h). "LightDrv" is the backlight driver used by
// SALightsService (SAServer) for the display/keypad lights behind HWRM. Hack H-lightdrv
// (docs/hacks.md): every request completes at once with KErrNone, controls return 0. Without the
// driver pair SALightsService leaves -5, HWRM's light plugin is never created and SysAp shows
// "Feature not supported".
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("LightDrv", KCtl, 1, ETrue)
