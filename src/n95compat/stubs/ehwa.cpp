// N95 ehwa.ldd stand-in (see nokiastub.h). "Hwa" = TI DSP/BIOS bridge driver used by the audio
// stack (Bal.dll -> bridgeapi.dll) in MmfAudioServer's resource-manager thread. Hack H-dspbridge
// (docs/hacks.md): every control returns 0 and writes nothing; requests complete at once. Its
// only purpose is to keep MmfAudioServer alive (EikSrvs' key-sound server needs it). No audio.
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("Hwa", KCtl, 1, ETrue)
