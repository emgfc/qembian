# 5800 XpressMusic (RM-356) bring-up notes (2026-11-07)

Firmware V 31.0.101 (prd.core.C00, prd.rofs2.V05, C01_prd.rofs3). Symbian 9.4 / S60 5th Edition on
the BB5 RAPIDO platform (same flash layout as the 5730/N79). Reached the home screen, menu and touch
input (L1); seeded boot to idle 9 s.

- **Personality sym94** (new): 9.4 inserts one slow exec (S45 TimeNowSecure) and appends five; process
  creation info 8 bytes longer than 9.2's. `personalities/sym94/README.md`.
- **Start-up**: the 9.3 modem answers worked unchanged (SIM_IND on channel 35, SIM lock status, lock
  info). Startup then waited for the start-up tone: RAPIDO audio is not emulated. F3 0x34 data requests
  now get "all taken" (iscpeer), the MdaifDriver stand-in buffer is 1 MB, and the tone is off
  (H-startuptone-none, a `[[rom.rofs_patches]]` edit of CenRep 101F8763 in our ROFS2 copy).
- **Display**: 360x640 UI on a natively landscape panel (wsini rotation 270, as the N95); N95 LCD HAL build.
- **Touch**: Nokia's exyin.dll replaced by the `exyin` stand-in (kernel DDigitiser on the Syborg pointer
  device, own DFC thread, 10 ms sampling while pressed); the Syborg mouse driver EPOINTER is left out
  (`kernel.omit_baseport`). Coordinates need `rotate = 270`: the digitiser reports the panel's 640x360
  and the window server turns them by the screen mode. Found with the on-screen keypad of the first-boot
  date query (a tap on "5" must type 5; a list tap selects whatever row it hits, so it proves nothing).
  `sydb phone` sends mouse press/drag/release as absolute QMP events; QEMU's own window shows the cursor.
- **Open**: touch latency in some apps (dialler: seconds per tap); home-screen shortcut row; audio;
  sydb agent on 9.4 start-up; proximity sensor; scripted taps need a hold of at least ~40 ms.
- **Open (2026-11-07, owner)**: parts of the screen flicker in the `sydb phone` window. The 5800 runs
  better since the QEMU TLB work and the touch changes, which may have exposed it. Not known whether
  the real phone does it. Candidates to tell apart, cheapest first:
  1. the window's read races the guest's drawing: `sydb phone` copies the frame buffer from shared guest
     RAM 60 times a second with no handshake, so it can catch a half-drawn region (compare with
     `sydb screenshot` series: if screendumps never show the broken state, it is the window side;
     a fix is to read only after the guest's update or vsync, e.g. a frame counter from syborg-fb);
  2. the d_display stand-in's update path: direct screen bitmap (DSB) double buffering, partial updates
     and the 60 Hz vsync pacing (src/standins/d_display); a region drawn, cleared and redrawn between
     two vsyncs would show the clear;
  3. the GLES path (sygl blob, host GL read-backs) for apps that use it (Carousel);
  4. faithful behaviour of the app (some 5th edition apps redraw whole areas on every change).
  Note which screens and apps flicker, and whether it follows touch input or is periodic.

## Open issue reported by the owner (2026-11-08)

2026-11-09: **Settings-entry pause isolated** to a mismatched SIM service in the
fake modem's response to `GetAlsBlocked`; original/corrected/restored reply tests
give 9.843 s / 0.001 s / 10.187 s. See [settings-delay.md](settings-delay.md) for
evidence, static firmware analysis and the implemented reply correction. Rebuilt
5800 test: 1 ms completion; smoke passed. 2026-11-10: N95 smoke passes, N73 reply unchanged, N8 Delight sub-views 1.2-1.4 s (were 3.7-8.4 s).

Random UI actions take seconds (e.g. Settings, then any item); suspected: a request to a driver or
server that completes only on a timeout. The same happens on the N8. First steps: docs/roadmap.md
"Open issues reported by the owner".
