/* sygl (src/sygl) built into the N8's libEGL.dll (src/khr/build.sh): what sygl_int.h reaches through the
   N95 ROM veneers (src/sygl's generated n95imports.h) is a plain import here. */
#ifdef __cplusplus
#include <e32debug.h>
#define N95_RDebug_Printf RDebug::Printf
#else
int sy_rdebug_printf(const char* aFmt, ...) __asm__("_ZN6RDebug6PrintfEPKcz");
#define N95_RDebug_Printf sy_rdebug_printf
#endif
