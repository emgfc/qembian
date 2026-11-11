/* OpenGL ES 1.1 and 2.0 in the Symbian^3 phones' libEGL.dll: forwarding only (docs/devices/nokia-n8-rm596/
   gles-host.md). The stubs (tools/gen-gles.py -> gles_stubs.c) turn each call into {thread, fn, words}; the
   host (QEMU syborg-gl, syborg_gles.h, and its EGL, src/syvg/syvg_egl.cpp) does everything else. Plain C. */
#ifndef GLES_INT_H
#define GLES_INT_H
#include "gl1_fn.h"
#include "gl2_fn.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef int GLfixed;
typedef int GLclampx;
typedef int GLintptr;
typedef int GLsizeiptr;
typedef char GLchar;
typedef void* GLeglImageOES;

/* room for a call's words (locks the command buffer; NULL and unlocked if it cannot be made) */
unsigned* khr_gl_call(int aOp, int aFn, int aWords);
/* send now; the host's results (NULL if it could not run the batch) */
const unsigned* khr_gl_sync(int aOutBytes);
void khr_gl_done(void);
/* glGetString: the host's [length, bytes] kept in the process (unlocks) */
const char* khr_gl_string(const unsigned* aOut);
/* glMapBufferOES / glUnmapBufferOES: memory for the mapping is the guest's to give (unlocks) */
unsigned khr_gl_map(int aOp, int aFn, unsigned* aArgs, int aMap);
static inline unsigned khr_gl_f2u(float f) { union { float f; unsigned u; } v; v.f = f; return v.u; }

/* the transport (hostgl.cpp) and guest services (eglos.cpp) */
void khr_lock(void);
void khr_unlock(void);
int khr_host_on(void);
unsigned* khr_emit(int aOp, int aArgs);
int khr_flush(int aOutBytes);
const unsigned* khr_out(void);
unsigned khr_thread_id(void);
void* khr_alloc(int aBytes);
void khr_free(void* aPtr);
void* khr_big_alloc(int aBytes);
void khr_big_free(void* aPtr);
#endif
