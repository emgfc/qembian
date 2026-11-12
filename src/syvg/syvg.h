/* syvg: the C interface between QEMU's syborg-gl device and the host EGL/OpenVG (the Khronos RI built for
   the host, src/syvg; docs/devices/nokia-n8-rm596/egl-vg-host-v2.md). SPDX-License-Identifier: MIT */
#ifndef SYVG_H
#define SYVG_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* provided by the device (syborg_gl.c): the calling guest thread's memory, during its kick */
int syvg_guest_read(uint32_t va, void *dst, size_t n);
int syvg_guest_write(uint32_t va, const void *src, size_t n);
/* provided by syvg to the device */
int syvg_vg_call(uint32_t owner, const uint32_t *a, size_t n, uint32_t *out, size_t outWords);
void syvg_vg_flush(void);       /* the RI's drawing of the OpenVG calls run so far (phase 3: once per run) */
int syvg_egl_call(uint32_t owner, const uint32_t *a, size_t n, uint32_t *out, size_t outWords);
void syvg_owner_free(uint32_t owner);
/* a machine reset: every guest process's EGL and VG objects (and their GPU objects) go, the tables are empty */
void syvg_reset(void);
/* the host EGL (syvg_egl.cpp): the device's EGL configs (the profile's file); the OpenGL ES context current
   in a guest thread (0: none), for the device's GL replay */
void syvg_egl_load_configs(const char *path);
uint32_t syvg_gl_current(uint32_t owner, uint32_t thread);
/* provided by the device (syborg_gl.c): the profile's GL identity strings, and whether a GL function exists */
const char *syborg_gl_identity(const char *key);
int syborg_gl_has_proc(const char *name);
/* the GPU memory (EGL_NOK_resource_profiling): the phone's total, and the bytes in use per process */
uint64_t syborg_gl_mem_total(void);
int syborg_gl_mem_usage(uint32_t *owners, uint64_t *bytes, int max, uint64_t *used);
/* EGL_NOK_image_endpoint: a producer's new frame of GCE surface sid (buffer -1: presented on the host) */
void syvg_endpoint_updated(const uint32_t *sid, int32_t buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* inside syvg */
uint32_t syvg_owner(void);
uint32_t syvg_thread(void);
void syvg_set_caller(uint32_t owner, uint32_t thread);
void *syvg_current_context(void);
int syvg_image_in_use(void *image);
#ifdef __cplusplus
}
#endif
/* a guest window as the host EGL sees it: the GCE surface the guest made for it and its size */
typedef struct SyvgHostWindow {
	unsigned int sid[4];
	int width, height;
	int presented;              /* the last swap reached the scan-out image */
} SyvgHostWindow;
#endif
