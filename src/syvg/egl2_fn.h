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
	EGL2_FN_GetProcAddress,         /* name* -> 1/2: an OpenGL ES function (its API), 3: an EGL extension
	                                   function (the guest's own stub), 0: none */
	/* EGL_NOK_resource_profiling(2) and EGL_KHR_reusable_sync (the guest's memory monitor, GOOM) */
	EGL2_FN_QueryProfilingDataNOK,  /* dpy, query_bits, data*, data_size, data_count* */
	EGL2_FN_CreateSyncKHR,          /* dpy, type, attribs* -> sync id (0: EGL_NO_SYNC_KHR) */
	EGL2_FN_DestroySyncKHR,         /* dpy, sync */
	EGL2_FN_ClientWaitSyncKHR,      /* dpy, sync, flags: never blocks; EGL_TIMEOUT_EXPIRED_KHR while unsignaled
	                                   (the guest waits out the timeout and asks again) */
	EGL2_FN_SignalSyncKHR,          /* dpy, sync, mode */
	EGL2_FN_GetSyncAttribKHR,       /* dpy, sync, attribute, value* */
	/* EGL_NOK_image_endpoint(2): endpoints on GCE surfaces, their frames as EGLImages, VGImages from those */
	EGL2_FN_CreateEndpointNOK,      /* dpy, type, source_type, source (TSurfaceId*), attribs*, surface info: width,
	                                   height, stride, pixel format, buffers, offset0, offset1, mapped base (the
	                                   guest maps the surface in the calling process; 0: not mapped) */
	EGL2_FN_DestroyEndpointNOK,     /* dpy, endpoint */
	EGL2_FN_GetEndpointAttribNOK,   /* dpy, endpoint, attrib -> value */
	EGL2_FN_SetEndpointAttribNOK,   /* dpy, endpoint, attrib, value */
	EGL2_FN_EndpointBeginStreamingNOK, /* dpy, endpoint */
	EGL2_FN_EndpointEndStreamingNOK,   /* dpy, endpoint */
	EGL2_FN_AcquireImageNOK,        /* dpy, endpoint -> EGLImage id */
	EGL2_FN_ReleaseImageNOK,        /* dpy, endpoint, image, api */
	EGL2_FN_GetEndpointDirtyAreaNOK, /* dpy, endpoint, rects*, start, max, collapse -> count */
	EGL2_FN_EndpointRequestNotificationNOK, /* dpy, endpoint: armed; the guest polls (EndpointPollNOK) */
	EGL2_FN_EndpointCancelNotificationNOK,  /* dpy, endpoint */
	EGL2_FN_EndpointPollNOK,        /* -> n; out[1..n] = endpoints whose armed notification is due (disarmed) */
	EGL2_FN_DestroyImageKHR,        /* dpy, image */
	EGL2_FN_CreateEGLImageTargetKHR, /* image -> VGImage (vgCreateEGLImageTargetKHR) */
	EGL2_FN_COUNT
};
#endif
