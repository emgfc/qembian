// Hosted EKA2: our board's syborg.h with its register access routed to the hosted I/O bus
// (src/hosted/variant/hboard.cpp): device models answer register reads/writes with the same semantics
// as the QEMU syborg devices, so the board's drivers and stand-ins compile unchanged. The real header
// (baseport syborg/specific/syborg.h) gives every constant.
#ifndef __HOSTED_BOARD_SYBORG_H__
#define __HOSTED_BOARD_SYBORG_H__
#define ReadReg __syborg_ReadReg_unused
#define WriteReg __syborg_WriteReg_unused
#include_next <syborg.h>
#undef ReadReg
#undef WriteReg
TUint32 HostedIoRead(TUint32 aAddr);
void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
static inline TUint32 ReadReg(TUint32 base, TUint8 aReg) { return HostedIoRead(base + (aReg << 2)); }
static inline void WriteReg(TUint32 base, TUint8 aReg, TUint32 aVal) { HostedIoWrite(base + (aReg << 2), aVal); }
#endif
