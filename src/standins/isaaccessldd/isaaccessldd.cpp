// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// ISAACCESSLDD.LDD stand-in (Symbian^3 N8: logical device "IADDriver", ISA Access): the fake modem of
// iscpeer.inl behind the ISA Access channel interface (docs/devices/nokia-n8-rm596/isa-access.md).
#define ISC_CHANNEL_BASE 0
#define ISC_LDD_NAME "IADDriver"
#define ISC_ISA_ACCESS 1
#include "iscpeer.inl"
