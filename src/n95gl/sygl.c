/*
 * sygl: our OpenGL ES 1.1 + EGL implementation behind the N95's libgles_cm.dll entry points
 * (docs/gpu-roadmap.md, hacks H-gles-trampolines / H-gles-blob).
 *
 * The blob is linked at a fixed ROM address (SYGL_BASE, src/n95gl/build.sh) and runs in the calling
 * thread: no loader, no imports table, no writable static data (it is ROM). N95 functions are called
 * at their export addresses (n95imports.h, generated). Per-thread state will hang off
 * UserSvr::DllTls with a fixed key (phase 1).
 *
 * Entry table: build/n95gl/gen/entry.S (tools/gen-gles-glue.py). Defining sy_<apiName> here with
 * the API's signature replaces the default stub, which logs the call and returns 0.
 *
 * Phase 0: no renderer. eglGetDisplay hands out a display, eglInitialize fails with
 * EGL_NOT_INITIALIZED, and every other call is a logged stub.
 */
#include "n95imports.h"

typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef void* EGLDisplay;

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define SYGL_DISPLAY ((EGLDisplay)1)

typedef int (*TPrintf)(const char*, ...);
#define LOG(...) ((TPrintf)N95_RDebug_Printf)("SYGL: " __VA_ARGS__)

extern const char* const sygl_names[];

/* default for every entry point not implemented yet (r0 = ordinal, from the stub) */
unsigned sygl_unimpl(int aOrdinal)
	{
	LOG("unimplemented %s (ord %d)", sygl_names[aOrdinal], aOrdinal);
	return 0;
	}

EGLDisplay sy_eglGetDisplay(void* aNativeDisplay)
	{
	LOG("eglGetDisplay(%d) -> 1 [phase 0 build " __DATE__ " " __TIME__ "]", (int)aNativeDisplay);
	return SYGL_DISPLAY;
	}

EGLBoolean sy_eglInitialize(EGLDisplay aDpy, EGLint* aMajor, EGLint* aMinor)
	{
	(void)aMajor; (void)aMinor;
	LOG("eglInitialize(%d): no renderer yet (phase 0) -> EGL_NOT_INITIALIZED", (int)aDpy);
	return EGL_FALSE;
	}

EGLint sy_eglGetError(void)
	{
	return EGL_NOT_INITIALIZED;
	}

EGLBoolean sy_eglTerminate(EGLDisplay aDpy)
	{
	(void)aDpy;
	return EGL_TRUE;
	}
