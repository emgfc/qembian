// usbaudiodriver.ldd stand-in (see nokiastub.h): channel "USBAudioDrv", opened by
// usbaudiodriverproxy.dll when the N8's Logical Audio Control (Lac.dll) builds its USB audio route
// while the audio server starts; failing to open it left -1 and took the system-permanent
// !AudioServer thread down. No USB audio device ever attaches here: every control returns KErrNone
// and every request (the proxy's link listener, data transfers) stays pending. Calls are logged.
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD("USBAudioDrv", KCtl, 1)
