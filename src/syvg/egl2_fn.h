/* EGL calls from the guest to the host EGL (SYGL_EGL; docs/devices/nokia-n8-rm596/egl-vg-host-v2.md):
   {fn, guest thread, args}. Pointers are guest virtual addresses; EGLSurface/EGLContext are 32-bit ids per
   guest process; EGLConfig is the config id (the device's configs: EGL_CONFIG_ID); EGLDisplay is 1. The guest
   only forwards: every EGL decision is the host's (src/syvg/syvg_egl.cpp). The result word goes to out[0] (strings:
   out[0] = length, bytes after). Shared by src/syvg/syvg_egl.cpp (host) and the guest forwarder.
   SPDX-License-Identifier: MIT */
#ifndef EGL2_FN_H
#define EGL2_FN_H
enum {
	EGL2_FN_GetError = 0,
	EGL2_FN_GetDisplay,             /* display_id */
	EGL2_FN_Initialize,             /* dpy, major*, minor* */
	EGL2_FN_Terminate,              /* dpy */
	EGL2_FN_QueryString,            /* dpy, name -> string */
	EGL2_FN_GetConfigs,             /* dpy, configs*, size, num* */
	EGL2_FN_ChooseConfig,           /* dpy, attribs*, configs*, size, num* */
	EGL2_FN_GetConfigAttrib,        /* dpy, config, attribute, value* */
	EGL2_FN_CreateWindowSurface,    /* dpy, config, sid0, sid1, sid2, sid3, width, height, attribs* */
	EGL2_FN_CreatePbufferSurface,   /* dpy, config, attribs* */
	EGL2_FN_CreatePbufferFromClientBuffer, /* dpy, buftype, buffer (a VG handle id), config, attribs* */
	EGL2_FN_DestroySurface,         /* dpy, surface */
	EGL2_FN_QuerySurface,           /* dpy, surface, attribute, value* */
	EGL2_FN_BindAPI,                /* api */
	EGL2_FN_QueryAPI,
	EGL2_FN_WaitClient,
	EGL2_FN_ReleaseThread,
	EGL2_FN_SurfaceAttrib,          /* dpy, surface, attribute, value */
	EGL2_FN_BindTexImage,           /* dpy, surface, buffer */
	EGL2_FN_ReleaseTexImage,        /* dpy, surface, buffer */
	EGL2_FN_SwapInterval,           /* dpy, interval */
	EGL2_FN_CreateContext,          /* dpy, config, share, attribs* */
	EGL2_FN_DestroyContext,         /* dpy, context */
	EGL2_FN_MakeCurrent,            /* dpy, draw, read, context */
	EGL2_FN_GetCurrentContext,
	EGL2_FN_GetCurrentSurface,      /* readdraw */
	EGL2_FN_GetCurrentDisplay,
	EGL2_FN_QueryContext,           /* dpy, context, attribute, value* */
	EGL2_FN_WaitGL,
	EGL2_FN_WaitNative,             /* engine */
	EGL2_FN_SwapBuffers,            /* dpy, surface -> result | presented << 1 */
	EGL2_FN_WindowUpdate,           /* surface, sid0..3, width, height: the guest remade its window surface */
	EGL2_FN_CreatePixmapSurface,    /* dpy, config, pixmap, attribs* */
	EGL2_FN_CopyBuffers,            /* dpy, surface, pixmap */
	EGL2_FN_GetProcAddress,         /* name* -> 1 if the host has the function */
	EGL2_FN_COUNT
};
#endif
