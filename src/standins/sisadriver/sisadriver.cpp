// sisadriver.ldd stand-in (see nokiastub.h): the 5730's IscApi loads it (RAPIDO's ISA transport, where
// the N95's IscApi loads xbus.ldd); the fake modem link itself is nokiaiscdriver. Load only.
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD("SisaDriver", KCtl, 0)
