/* host build of the RI (qembian): EGL scalar and handle types without Symbian's e32def.h */
#ifndef __EGL_EGLTYPES_H_
#define __EGL_EGLTYPES_H_
#include <khronos_types.h>
#include <stdint.h>
typedef int NativeDisplayType;
typedef void *NativeWindowType;
typedef void *NativePixmapType;
typedef NativeDisplayType EGLNativeDisplayType;
typedef NativePixmapType EGLNativePixmapType;
typedef NativeWindowType EGLNativeWindowType;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLClientBuffer;
#define EGL_DEFAULT_DISPLAY 0
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
typedef unsigned int TUint;
#define EGLAPI
#define EGLAPIENTRY
#define EGLAPIENTRYP EGLAPIENTRY *
typedef uint64_t khronos_utime_nanoseconds_t;
typedef uint64_t TUint64;
typedef int64_t TInt64;
#endif
