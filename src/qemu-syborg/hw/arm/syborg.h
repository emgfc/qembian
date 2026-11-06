/*
 * Syborg (Symbian Virtual Platform) board for modern QEMU — shared definitions.
 *
 * Written for syborg-pg from the register descriptions in the Symbian Syborg baseport and the
 * QEMU 0.9.1-symbian device plugins (behaviour only, no code copied).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_SYBORG_H
#define HW_ARM_SYBORG_H

#define TYPE_SYBORG_INTC     "syborg-intc"
#define TYPE_SYBORG_TIMER    "syborg-timer"
#define TYPE_SYBORG_RTC      "syborg-rtc"
#define TYPE_SYBORG_SERIAL   "syborg-serial"
#define TYPE_SYBORG_FB       "syborg-fb"
#define TYPE_SYBORG_KEYBOARD "syborg-keyboard"
#define TYPE_SYBORG_POINTER  "syborg-pointer"
#define TYPE_SYBORG_NVMEM    "syborg-nvmem"
#define TYPE_SYBORG_PLATFORM "syborg-platform"
#define TYPE_SYBORG_AUDIO    "syborg-audio"
#define TYPE_SYBORG_GL       "syborg-gl"

/* Device ID register values (register 0 of each device). */
#define SYBORG_ID_INTC     0xc51d0000
#define SYBORG_ID_SERIAL   0xc51d0001
#define SYBORG_ID_KEYBOARD 0xc51d0002
#define SYBORG_ID_TIMER    0xc51d0003
#define SYBORG_ID_RTC      0xc51d0004
#define SYBORG_ID_MOUSE    0xc51d0005
#define SYBORG_ID_TOUCHSCREEN 0xc51d0006
#define SYBORG_ID_FB       0xc51d0006   /* the 0.9.1 plugin answers ...06, not ...07 */
#define SYBORG_ID_NVMEM    0xdeadbeef
#define SYBORG_ID_PLATFORM 0xc51d1000
#define SYBORG_ID_AUDIO    0xc51d0010   /* ours: output for the N95 fake DSP */

#endif
