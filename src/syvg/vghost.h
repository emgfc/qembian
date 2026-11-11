/* syvg: drawing the RI's objects through syborg-gl in process (vghost.cpp, vgraster.cpp; the v1 guest code
   src/khr/hostgl.cpp + hostraster.cpp ported). SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
extern "C" uint32_t syborg_gl_exec_host(const uint32_t *cmd, size_t words, void *out, size_t outlen);
namespace OpenVGRI
{
class Color;
class Image;
class Surface;
bool khrHostOn();
unsigned int *khrHostEmit(int aOp, int aArgs);
int khrHostFlush(int aOutBytes);
unsigned int khrHostArgb(const Color &aColor);
unsigned int khrHostImage(Image *img);
void khrHostTargetSync(Surface *s);          /* an image target's pending CPU change, before drawing into it */
bool khrHostPresent(const Surface *src, const unsigned int *sid);
unsigned int *khrHostLast();
unsigned int *khrHostGrow(int aWords);
bool khrHostReadRows(unsigned int aSurf, int aW, int aY0, int aRows, unsigned int *aDst);
bool khrHostPresentId(unsigned int aSurf, const unsigned int *sid);
const unsigned int *khrHostOutArea();
void khrHostReset();                       /* syvg_reset: the batch, the ids, the owner */
void vgSetOwner(unsigned int aOwner);        /* the guest process of the calls that follow */
unsigned int vgKey(const void *aObj);        /* the device's 32-bit id of an RI object (even) */
void vgKeyDrop(const void *aObj);
}
