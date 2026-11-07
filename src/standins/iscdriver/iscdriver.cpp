// iscdriver.ldd: fake modem peer for IscApi channels 0..31 on phones with the older ISC naming
// (N73, Symbian 9.1: IscApi loads "iscdriver.ldd" and opens "IscDriver"). Same IscApi request
// protocol as the N95's NokiaISC (all 32 IscApi exports issue the same requests, compared in
// docs/devices/nokia-n73-rm132/onboarding-log.md), so the peer is the shared iscpeer.inl.
#define ISC_CHANNEL_BASE 0
#define ISC_LDD_NAME "IscDriver"
#include "iscpeer.inl"
