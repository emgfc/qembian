// als.ldd stand-in (N86 8MP: Nokia's ambient light sensor driver; kernel side, not in our ROM). The light
// sensor service of SAServer (light_sensor_service.discrete_als.DLL) loads "ALS.pdd" and "ALS" and leaves
// with -5 when either load fails (SAServer then panics "SystemAdaptation -5" and the phone resets), then
// opens channel "ALS" (TVersion 1.0.0x642, unit -1, no PDD named). Hack H-als (docs/hacks.md): controls
// return 0, requests (light readings) stay pending: no light sensor, the backlight stays as set.
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("ALS", KCtl, 1, EFalse)
