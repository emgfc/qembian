/* src/khr: the guest's command transport to syborg-gl (hostgl.cpp) */
#pragma once
namespace OpenVGRI
{
class Color;
class Image;
class Surface;
bool khrHostOn();
unsigned int *khrHostEmit(int aOp, int aArgs);
int khrHostFlush(int aOutBytes);
unsigned int *khrHostLast();
unsigned int *khrHostGrow(int aWords);
bool khrHostReadRows(unsigned int aSurf, int aW, int aY0, int aRows, unsigned int *aDst);
bool khrHostPresentId(unsigned int aSurf, const unsigned int *sid);
const unsigned int *khrHostOutArea();
}
