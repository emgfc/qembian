/* Force-included into the RI sources built for the host (scripts/build-syvg.sh): the RI's only use of C++
   exceptions is std::bad_alloc around allocations; built without exceptions, a failed allocation aborts
   the device with a message instead (host memory is not the limit the guest heap was). */
#pragma once
#ifdef __cplusplus
/* every system header the RI uses, first: the macros below must not reach their declarations */
#include <new>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <climits>
#include <cassert>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>
#include <assert.h>
#include <stdint.h>
/* MiniEGL keeps its global EGL in Dll::Tls (Symbian: per thread); here per guest process (syvg_eglos.cpp) */
namespace Dll { void *Tls(); int SetTls(void *aPtr); }
static inline void syvg_oom(void) { fprintf(stderr, "syvg: out of memory\n"); abort(); }
#define try if (1)
#define catch(x) else if (0)
#define throw syvg_oom();
#endif
