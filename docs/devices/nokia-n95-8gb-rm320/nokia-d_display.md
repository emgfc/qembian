# Nokia `d_display.ldd` ("DisplayDriver") — real interface, by static analysis

Source: XIP `Sys/Bin/d_display.ldd` in `build/n95/core-flat.img` (code 0x80120ce8..0x8013ce84,
115664 bytes, built "Nov 6 2008 22:25:15", version 1.1.1540). Decompile:
`build/n95/ghidra/d_display.ldd.c` (ekern imports resolved by ordinal against
`eka/eabi/ekernu.def`). Client: `build/n95/ghidra/r_display.dll.c`. All addresses below are
ROM addresses in those files. **V** = verified in code, **I** = inferred.

Source paths embedded in the binary (panic strings): `base_ca_sw\display\display_driver.cpp`,
`hal_handler.cpp`, `atid.cpp`, `display_channel.cpp`, `omap\2420\display_driver_specific.cpp`,
`path\common_hw\dispc\dispc2.cpp`, `omap\2420\client\aino\display_driver_product.cpp`
("Aino" = N95 8GB product variant). RTTI names: `DDisplayDevice`, `DDisplayChannel`,
`DisplayEntry`, `TDisplayModule`, `DisplayHandler`/`DisplayHandlerMessi`, `InterfaceMeSSI`,
`InterfaceMeSSIRfbi`, `InterfaceMeSSICommands`, `DispC`, `Vrfb`, `RotateSw`, `DmaCopier`,
`DOmapDmaChannelHelper`, `DOmapSysDmaChannelHelper`, `DDisplayPowerHandler`, `DResourceHandler`,
`DvfsDisplay`, `TResourceStateChangeNotifier`.

## 1. Driver structure

### 1.1 Dual role: kernel extension + LDD (V)

The module entry `FUN_8013a2a4` handles `KModuleEntryReasonExtensionInit1` (-1): it allocates the
0x6a0-byte `DDisplayDevice` (`FUN_801211dc` ctor, `FUN_8012102c` init), stores it in the global
at 0xc800f040, and registers it in the GAPE name registry (`GAPE.DLL` ord2 = `Register(name,ptr)`,
ord1 = `Get(name)`, 31 slots, `TDesC8::Compare`) under the literal **"SecDisp"** (0x8013b0d8 —
Nokia's naming; there is no secondary display) . The channel ctor looks the same name up as a
fallback (`FUN_8012e8b8`, literal 0x8012e9b4). The kernel-side API object (`DResourceHandler`,
device+0x37c) is registered as **"PriDisp"** (`FUN_80127250`); `r_display_kernel.dll`,
`tv_out.dll` and `pvromaplcd.dll` use it.

Export 1 (`d_display_ord1` @ 0x8012e79c) is the LDD factory: `DLogicalDevice` of 0x38 bytes,
`iVersion = (1,1,0x604=1540)`, `iParseMask = 1 (KDeviceAllowUnit)`, `iUnitsMask = 1` (unit 0
only); name "DisplayDriver". No PDD. `r_display.dll` ord12 opens it with
`LoadLogicalDevice`, then `DoCreate(L"DisplayDriver", TVersion(1,1,1540), KNullUnit, NULL, NULL,
EOwnerProcess)` (0x8084cac6) and immediately issues ctl 10 (select screen) + ctl 11 (caps) — the
"open / 10 / 11 / 11 / 11" pattern in our logs.

Imports (V): ekern (104), `kaomap.dll` (21: OMAP2420 ASSP — clocks/GPIO/IRQ), `power.dll`
(10: Nokia power/DVFS resource manager; ord50/19/17/12 used in `FUN_80121a98` to request
display clocks), `dma.dll` (4: system DMA for RFBI transfers), `GAPE.DLL` (2).

Companions in the ROM (V): `r_display_kernel.dll` (kernel client of "PriDisp"; drives HAL
display functions 10/13 and the `DResourceHandler` command set), `tv_out.dll` (registers
"TvOut"), `boot_figure_240x320.dll` (registers "BootFigure0/1", 240×320 16-bit bitmaps that
`FUN_80120da8`/`FUN_80122428` centre on the panel at init), `omaplcd.ldd` + `pvromaplcd.dll`
(PowerVR display class for `pvrsrv.dll` 3D; imports `r_display_kernel`), `LightLdd/LightPdd`
(separate backlight driver; the display driver also sets brightness through the panel).

### 1.2 Threads, queues, state machine (V)

* DFC queue **"DisplayThread"** (`Kern::DfcQCreate` in `FUN_801211dc`, name 0x8013b094),
  stored at device+0x670. Every channel calls `SetDfcQ(device+0x670)` in `DoCreate`
  (`FUN_8012ea6c`) and registers itself in the device channel array (+0x678) via internal ctl
  0x22.
* **DisplayHandler** command queue at device+0x2f8: `FUN_80126de0` appends a 16-byte command
  `{cmd | arg<<16, a, b, c}` and enqueues a DFC; `FUN_80126c78` pops one command at a time and
  dispatches it in `FUN_80126998` (18-case switch) to the per-screen state machine
  (`DisplayEntry` +0x2d4 = state, vtable 0x8013caf4 for the product class). Commands:
  1 display on/power-up sequence (`FUN_80136208`: DCS 0x11 sleep-out, 0x3a pixel format,
  0x36, 0x35 tearing line, 0x2c RAM write, 0x29 display on), 2 display off (`FUN_80136928`),
  3 self-test (req 0x8001), 4 partial-display on (`FUN_80137204`: DCS 0x30 set partial
  area, 0x12 partial mode, 0x39/0x38 idle mode on/off), 5 partial off (`FUN_80137320`: DCS
  0x38, 0x13), 6 re-init, 7 panel setting from ctl 7, 8 retry, 9|n<<16 frame-buffer reload,
  10 transfer rect to panel (`FUN_80137c98`, RFBI DMA, 90-tick timeout), 0xb|mode<<16 set
  colour mode (`FUN_80137dd0`: DCS 0x3a 0x05/0x50 = 16 bpp, 0x06/0x60 or 0x07/0x70 = 18/24 bpp),
  0xe DSB posting, 0xf|rot<<16 rotation (`FUN_801248c8` → `FUN_80137f14`: DISPC/VRFB
  reprogram), 0x10 posting buffer, 0x12, 0x13 backlight/brightness (`FUN_80137f98`),
  0x14 (ctl 0x1e window), 0x15.
* The real panel is a MIPI-DCS command-mode panel behind the OMAP2420 RFBI ("MeSSI");
  the picture lives in a **VRFB** (hardware rotation) buffer with a 2048-pixel stride
  (`+0x3cc != 0` → stride = bpp<<11, `FUN_801223f4`). The user-visible frame buffer is a
  separate physically contiguous "VgaBuffer" (screen+0xcc, allocated in `FUN_80130c2c`/
  `FUN_80120ef8`), 320-pixel stride; updates copy rects from it into the VRFB buffer and then
  DMA the rect to the panel. There is no continuous scan-out; nothing is visible until ctl 9/8.

### 1.3 Power handler (V)

`DDisplayPowerHandler` (`FUN_80121968`, `DPowerHandler::Add` in `FUN_80121a48`,
`imp_power_ord12(0x42,…)` registers resource 0x42). `PowerDown(standby|off)`
(`FUN_801217e0`) turns the display off (`FUN_80122a7c` → cmd 2) and releases clocks
(`FUN_80121a98`: `power` ord50/17); `PowerUp` only calls `PowerUpDone` — the screen is
re-enabled by WSERV through HAL `EDisplayHalSetState`/`WsSwitchOnScreen`.

### 1.4 HAL functions (V)

`Kern::AddHalEntry(EHalGroupDisplay=4, …)` once per screen in `FUN_80126518` (screen 0 →
thunk 0x801264fc). The handler `FUN_801258f0` runs on DisplayThread (message queue):

| TDisplayHalFunction | behaviour |
|---|---|
| 0 ScreenInfo | `TScreenInfoV01{0,0,1,userBuffer,(w,h)}` (w/h follow rotation) |
| 1 WsRegisterSwitchOnScreenHandling | stores flag |
| 2 WsSwitchOnScreen | display on (cmd 1) |
| 3 MaxDisplayContrast / 5 DisplayContrast | 0 / KErrNotSupported |
| 4 SetDisplayContrast, 6 SetBacklightBehavior, 8 SetBacklightOnTime | capability WriteDeviceData checked, then KErrNotSupported |
| 10 SetBacklightOn | cap WriteDeviceData; `Kern::MachinePowerStatus()>1` else KErrBadPower; `FUN_80124aec(screen,on)` → cmd 0x13 |
| 11 BacklightOn | current value (only if panel has backlight, +0x260) |
| 12 MaxDisplayBrightness | 100 |
| 13 SetDisplayBrightness | cap WriteDeviceData; clamp 0..100; `FUN_80124b04` → cmd 0x13 |
| 15 ModeCount | 2 (`FUN_80122a08`) |
| 16 SetMode | cap MultimediaDD; mode 0/1 → `FUN_80123170` (same path as req 0x8002) |
| 17 Mode | screen+0x3b4 |
| 18/19 Set/PaletteEntry | KErrNotSupported (not palettized) |
| 20 SetState | cap PowerMgmt; 0 → off (cmd 2), else on (cmd 1) |
| 21 State | screen+0x2d0 |
| 22 Colors | 0x1000000 |
| 23/24 Current/SpecifiedModeInfo | `TVideoInfoV01` from handler words [0x17..0x23]; size is swapped twice when rotated so **HAL X/Y pixels are always native 320×240**; `iOffsetBetweenLines = 320 × bytes/pixel` (table 0x8013b3c4 = {0x140,0x140}); mode 0 = 16 bpp EColor64K, mode 1 = 32 bpp EColor16MU |
| 27 Secure / 28 SetSecure | handler[0x16]; cap MultimediaDD |

This matches our BSP `lcd_n95` reporting native 320×240 (hack H-lcd-native-landscape).

### 1.5 Channel object (`DDisplayChannel`, 0x80 bytes, `FUN_8012e8b8`) (V)

+0x58 client `DThread`, +0x5c TRequestStatus of req 0x23, +0x60 client `DProcess`, +0x64
device, +0x68 selected screen (ctl 10), +0x6c status of the one outstanding 0x8xxx request,
+0x70 req 0x2002, +0x74 req 0x2001, +0x78 req 0x2003/0x2004, +0x7c "mode change allowed"
(1 at create; ctl 0x17 clears, ctl 0x19 sets; req 0x8002 needs it).

Dispatch (V): `Request` override `FUN_8012ff20` — fn ≥ 0 with bit 0x4000 is handled in the
caller's context without a message (0x4001 = DVFS tickle for TV-out → KErrNone; other 0x4xxx →
KErrNotSupported); everything else is sent to DisplayThread. `HandleMsg` `FUN_8012fdc8`:
`KMaxTInt` → `DoCancel` `FUN_8012fc18` (ignores the mask: completes +0x5c/+0x6c/+0x70/+0x74/
+0x78 with KErrCancel, releases DSB buffers, cancels partial mode if this channel owns it);
`ECloseMsg` → unregister + cancel; negative fn → `DoRequest` `FUN_8012f99c(~fn, status, a1,
a2)` (the two arg words are read from user memory in `Request`); otherwise `DoControl`
`FUN_8012eb64(fn, a1, a2)`. Unknown fn → KErrNotSupported (-5).

### 1.6 Per-screen object (`DisplayEntry`, screen 0 = device+0x65c) — fields that matter (V)

+0x40 backlight on, +0x44 brightness, +0x64/+0x68 current width/height (**320/240; swapped
by `FUN_801268b0` when the rotation parity changes**), +0x94 current mode index, +0xcc user
frame buffer, +0xd4 bytes/pixel, +0xdc TDisplayMode, +0xe0 VRFB buffer, +0x1a0 screen index,
+0x1a4 display type (0 LCD, 8 TV), +0x1a8 enabled (1), +0x1ac `TDisplayModule` panel
descriptor (`FUN_8012d658`: product 0x290000 "Aino", panel id 0x5b: **320×240**, max
partial lines 240, backlight present), +0x230 max partial lines (240), +0x27c/+0x280 panel
height/width (240/320, never swapped), +0x28c/+0x290 = 0, +0x294 = 1, +0x298/+0x29c/+0x2a0
rotation 90/180/270 supported = **0/1/1** (tables 0x8013b2fc..), +0x2a4 EColor4K = 0,
+0x2a8 EColor64K = 1, +0x2ac EColor16M = 0, +0x2b0 EColor16MU = 1, +0x2b4 EColor16MA = 0,
+0x2d0 display on, +0x2f0 partial active, +0x2f4/+0x2f8/+0x2fc partial start/end/full-colour,
+0x304 channel owning partial mode, +0x318 update in progress, +0x3b4 mode, +0x3c4 current
rotation index 0..3, +0x3cc VRFB on (1), +0x46c panel ID (DCS 0x04 read), device+0x698 update
freeze flag, device+0x69c swallowed-update counter.

## 2. DoControl / DoRequest table

`r_display.dll` exports are listed as `ordN` with the `CDisplayDriver` vtable slot (vtable at
0x8084da68; e.g. slot +0x7c = ord38). "a1 → N words" means the driver `ThreadRawRead`s N words
from a1; "a1 ← …" means it `ThreadRawWrite`s. Errors: -5 KErrNotSupported, -6 KErrArgument,
-18 KErrNotReady, -22 KErrLocked, -46 KErrPermissionDenied, -3 KErrCancel.

### 2.1 DoControl (`FUN_8012eb64`)

| fn | export (slot) | arguments | what the driver does | result |
|---|---|---|---|---|
| 3 | ord42 (+0x10) | a1 → `{startRow, endRow, fullColour}` | **Set partial display** (`FUN_801231fc`): -5 if +0x230 == 0; does a full-screen copy first; requires `start < end < 240` (+0x27c, panel rows, *not* rotated) else -6; clamps `end ≤ start+239`; stores +0x2f0=1, +0x2f4/+0x2f8/+0x2fc; if display is off only marks pending (+0x2ec); else +0x304 = channel and posts cmd 4 → DCS 0x30 (rows + panel offset), 0x12, and 0x39 (idle = 8-colour low power) when fullColour == 0 else 0x38 | 0 |
| 4 | ord47 (+0x1c) | none | cancel partial display (`FUN_80123ad4`): -5 if +0x230 == 0; +0x2f0 = 0; cmd 5 → DCS 0x38, 0x13 | 0 |
| 5 | ord55 (+0x14) | a1 ← TInt | partial-display active flag (+0x2f0) | 0 |
| 6 | ord39 (+0x24) | a1 ← TInt | panel ID (+0x46c, 24-bit DCS 0x04 read); -5 if +0x1a8 == 0 | 0 |
| 7 | ord40 (+0x2c) | a1 = index 0..3 **by value** | +0x468 = {8,2,1,4}[index] (module words +0x23c.., default 1); cmd 7 (panel register, purpose unknown) | 0 |
| 8 | ord34 (+0x80) | none | full refresh (`FUN_80122ffc`): copy whole user buffer → VRFB, cmd 10 unless an update is in flight; -5 if type == 1; counted if frozen | 0 |
| 9 | ord33 (+0x88) | a1 → 2 words = ushorts `{x, w, y, h}` i.e. `[x \| w<<16][y \| h<<16]`; 0xffff = whole for x/w | **Update rect** (`FUN_801240f8`): clamps to current +0x64/+0x68 (follow rotation → 240×320 after 270), w/h 0 → 1; `FUN_80122e28` copies the rect from the user buffer (stride = ctl 31 value) into the VRFB buffer (or SW-rotates/DMA-copies); if no update in flight posts cmd 10 with the rect (whole screen after 10 retries), else just re-kicks the DFC (coalesced). Counted if frozen (ctl 0x2b). Observed args: `00f00000 01400000` (0,240,0,320), `00f00000 00190127` (y=295,h=25) — portrait coordinates | 0 |
| 10 | ord60 (+0x78) | a1 = screen number **by value** | only 0 accepted, stored at channel+0x68 | 0 / -5 |
| 11 | ord38 (+0x7c) | a1 ← 0x68 bytes (26 words) | capabilities, see 2.3 | 0 |
| 12 | ord68 (+0x38) | cap WDD; a1 → 7 words | TV-out start via GAPE "TvOut" (`FUN_80127c00`) | -1 if no tv_out, -16 busy |
| 13 | ord49 (+0x54) | a1 → 3 words `{1,0xffffff,0}` defaults | TV-out colour/image settings (`FUN_8013123c`) | |
| 14 | ord48 (+0x40) | cap WDD; a1 → 7 words | video posting buffer setup (`FUN_80127f94`) | -6 if too big |
| 15 | ord3 `CDisplayPosting::NewL` | a1 → `{ptr→9 words, ptr→13 words, screen}` | create posting (overlay) session on device+0x4e0 (`FUN_80128b0c`); owner = channel | |
| 16 | ord1 | a1 ← 10 words | posting capabilities (`FUN_80131fac`) | |
| 17 | ord16 | a1 ← 3 words | posting buffer addresses (`FUN_80129654`) | -2 if none |
| 18 / 19 | ord15 | a1 → 3 words / none | post buffer with/without rect (`FUN_801297d0`) | |
| 20 | ord17 | a1 → 13 words | posting image settings (`FUN_801299c4`) | |
| 21 | ord19 | a1 ← TInt | posting status (`FUN_80129b7c`) | -18/-5 |
| 22 | ord87 | none | cancel req 0x2002 (completes +0x70 with KErrCancel) | 0 / -1 |
| 23 | ord26 (+0xec) | a1 → 2 words rect, a2 = mode flag | **Direct-screen-bitmap buffer create** (`FUN_8012a888`): -18 if TV/posting active; channel+0x7c = 0 | |
| 24 | ord45 (+0xf0) | a1 ← 7 words `{displayMode, address, w, h, lineBytes, pixelShift 4/5, ?}` | DSB buffer info (`FUN_8012aa20`); r_display adds `RChunk::Base` to the address | -18 none, -39 in use |
| 25 | ord69 (+0xfc) | none | DSB release (`FUN_8012b10c`); channel+0x7c = 1 | |
| 26 | ord57 (+0xa8) | a1 ← TInt | **current rotation index** (+0x3c4: 0/1/2/3 = 0°/90°/180°/270°) | 0 |
| 27 | ord67 (+0x48) | none | returns `Kern::HalFunction(EHalGroupKernel, EKernelHalPageSizeInBytes)` = 4096 as the return value | 4096 |
| 28 | ord43 (+0x58) | none | TV-out stop | 0 |
| 30 | ord75 (+0x28) | a1 → 8 words `{enable, start, end, …}` | rows window (`FUN_80124d1c`, cmd 0x14; purpose unknown — I: tearing/TE window) | -6 if end ≤ start, -41 if start == 0 |
| 31 | ord77 (+0xb0) | a1 ← TInt | **scan-line bytes** = 320 × bytes/pixel of the current mode (`FUN_8012557c`; table {0x140,0x140} so **independent of rotation**): 640 (64K) / **1280 (16MU)** | 0 |
| 32 | ord83 (+0x3c) | cap WDD; a1 ← 7 words | TV-out state (device+0x43c..) | 0 |
| 33 | ord85 (+0x90) | a1 → 2 words rect (ctl-9 format) | copy rect user → VRFB buffer **without** panel transfer (`FUN_80124070`+`FUN_80122e28`); DSB path only (r_display requires its +0x74 flag) | 0 |
| 34 | internal | | register channel (from DoCreate) | |
| 36 | ord74 (+0x60) | none | cancel req 0x23 (KErrCancel) | 0 |
| 37 | ord93 (+0x44) | cap WDD; a1 ↔ 7 words | posting settings get/adjust (`FUN_801280f8`) | |
| 38 | ord91 (+0x64) | cap WDD; a1 → 7 words | posting pixel formats (`FUN_80127d00`), validated with masks 1/2/4/8/16 | -5/-6/-12 |
| 39 / 40 | ord90 / ord92 | cap WDD; none | return supported source / destination format masks | mask |
| 41 | ord88 (+0x70) | cap WDD; a1 → TInt | return mask for that format | mask |
| 42 | ord89 (+0x74) | cap WDD; a1 ← TInt | TV screen index; -5 unless that screen's type == 8 | |
| 43 / 44 / 45 | ord50 (generic `DoControl(fn)`) | none | **freeze updates** (device+0x698 = 1) / unfreeze / return number of swallowed updates. 0x2c observed 3× per boot | 0 / 0 / count |
| 0x4001 | ord41 (+0xd8) | none | fast path, no message: DVFS tickle when TV-out active | 0 |
| other | | | | -5 |

### 2.2 DoRequest (`FUN_8012f99c`; a1 = first user arg word, by value)

| req | export (slot) | a1 | behaviour | completion |
|---|---|---|---|---|
| 0x23 | ord73 (+0x5c) | — | event notification; stored at channel+0x5c (previous one completed KErrCancel) | KErrCancel on ctl 0x24 / cancel / close; otherwise stays pending |
| 0x2001 | ord20 | — | stop posting (owner channel only), +0x74 | when stopped |
| 0x2002 | ord18 | a2 | posting wait-for-buffer/vsync (`FUN_80129608`), +0x70 | |
| 0x2003 | ord36 (+0xf4) | — | **DSB EndUpdate (whole)**: cmd 0xe posts the DSB buffer, +0x78 | after transfer |
| 0x2004 | ord37 (+0xf8) | a1 → 8-byte rect | DSB EndUpdate with rect | after transfer |
| 0x8001 | ord46 (+0x20) | sub-command | cap WDD; -18 if display type disabled, -5 if +0x1a8 == 0; turns the display on if off; **0 = panel self-test** (read ID with DCS 0x04, write/read-back test pixels, `FUN_80136ab0`); 1 and 2 → KErrNotSupported (0x801239f4/0x80123a00) | test result |
| 0x8002 | ord51 (+0x98) | mode index (0 = EColor64K 16 bpp, 1 = EColor16MU 32 bpp; r_display ord52 maps TDisplayMode → index) | -46 if client process field +0x9c == -2 (I: no SID); -22 (KErrLocked) while a DSB buffer exists; validates against +0x2a8/+0x2b0/+0x2b4; cmd 0xb → DCS 0x3a; HAL mode info/stride/TDisplayMode updated (`FUN_80125244`) | 0 after the panel accepted the format. Observed: a1 = 1 |
| 0x8003 | ord54 (+0xa0) | rotation index 0..3 | 1 (90°) → -5, 2/3 need +0x29c/+0x2a0 (both 1); cmd 0xf: swaps +0x64/+0x68 and twips when parity changes (`FUN_801268b0`), +0x3c4 = index, reprograms DISPC/VRFB | 0 (`FUN_801249a8`). Observed: a1 = 3 |
| 0x8004 / 0x8005 | ord30 / ord35 (+0x4c/+0x50) | | cap WDD; posting start / stop (`FUN_80127e38`/`FUN_80127ee8`) | -18/-16/-11 when no TV-out resource |
| other | | | | -5 |

Only one 0x8xxx request may be outstanding per channel (+0x6c); a second one returns -5.
`r_display` issues all 0x8xxx requests synchronously (`DoRequest` + `User::WaitForRequest`).

### 2.3 Capabilities block (ctl 11, 26 words, `FUN_80122928` + channel code) (V)

Values are what the N95 8GB (product 0x29, panel 0x5b, screen 0) returns.

| word | offset | source | N95 8GB value | meaning |
|---|---|---|---|---|
| 0 | +0 | `+0x1a4 != 1` | 1 | display present (type is not "none") |
| 1 | +4 | client process priority == 650 (`EPriorityWindowServer`) | 1 for WSERV | caller is the window server |
| 2 | +8 | +0x294 | 1 | **direct-screen-bitmap buffers supported** — ScDv stores `!word2`; with 0 its `CDirectScreenBitmap` allocates its own memory instead of using ctl 0x17/0x18/0x19 + req 0x2003/0x2004 (`FUN_80878c20`) |
| 3 | +12 | const | 0 | `r_display` keeps the block at `this+0x18`; word 3 (`this+0x24`) non-zero = DSB info word 1 is an `RChunk` handle + word 6 offset (ord45); 0 = word 1 is the address (V) |
| 4 | +16 | +0x290 | 0 | partial-width (x-range) panel updates |
| 5 | +20 | +0x28c | 0 | update retry strategy (ctl 9 retry counter +0x31c) |
| 6 | +24 | +0x68 | 240, **320 after rotation 270** | current height |
| 7 | +28 | +0x64 | 320, **240 after rotation 270** | current width |
| 8 | +32 | +0x230 | 240 | **max partial-display lines** (ord62; `power_save_display_mode` ord5) |
| 9 | +36 | +0x298 | **0** | rotation 90 supported |
| 10 | +40 | +0x29c | 1 | rotation 180 supported |
| 11 | +44 | +0x2a0 | 1 | rotation 270 supported |
| 12 | +48 | +0x2a4 | 0 | EColor4K (ScDv mode 10) |
| 13 | +52 | +0x2a8 | 1 | EColor64K (ScDv mode 7) |
| 14 | +56 | +0x2ac | 0 | EColor16M |
| 15 | +60 | +0x2b0 | 1 | EColor16MU |
| 16 | +64 | +0x2b4 | 0 | EColor16MA (also gates ord52/ord65 16MA↔16MU choice) |
| 17 | +68 | module[5] | 0 | panel flag (unknown) |
| 18–21 | +72..+84 | const | 0 | |
| 22, 23 | +88, +92 | const | 1, 1 | unknown constants |
| 24, 25 | +96, +100 | const | 0 | |

`r_display` copies the block into `CDisplayDriver`+0x18 and re-reads it after ctl 10 (ord60).
ScDv (`FUN_808786xx`) picks the colour mode from words 16/15/14 (16MA/16MU/16M) and checks
word 13/12 for 64K/4K devices; `FUN_80878a38` stores word 0 and `!word2`.

### 2.4 Coordinate spaces (V)

* ctl 9 / 0x21 / 0x17 rects are in the **current orientation** (after 0x8003 index 3 the
  driver's width/height are 240×320) — matches the observed args.
* ctl 3 rows are **panel rows 0..239** (never rotated). `screensaver.exe` (`FUN_82dd6030`)
  passes the UI rect's **X** range (`iTl.iX`, `iBr.iX`) when WSERV's rotation is 1 or 3 and the Y
  range otherwise, i.e. on the N95 (rotation 270) a partial band is a range of portrait
  columns = rows of the native 320×240 picture.
* ctl 31 is the stride of the user buffer: 320 pixels × bytes/pixel regardless of rotation
  (so a portrait 240×320 picture is drawn into 1280-byte lines on the real phone).
* HAL `EDisplayXPixels/YPixels` stay native 320×240; `r_display` ord59 swaps them when
  `ctl 0x1a & 1`.

## 3. What our boot actually uses

Observed in every run (`SYBSTUB: DisplayDriver` lines, e.g. `build/runs/skipphone/serial.log`):
open ×4 by `EwSrv.exe::Wserv` (ScDv probes `CFbsDrawDevice::NewScreenDeviceL` for several
modes + the DSB object), ctl 10 (0), ctl 11 (×16+), ctl 31, req 0x8002 (a1 = 1 → 16MU),
req 0x8003 (a1 = 3 → 270°), ctl 9 (dirty rects, portrait coordinates), ctl 0x2c (unfreeze, 3×).
Never seen so far: 0x8001, ctl 8, ctl 0x1a, ctl 0x2b/0x2d.

Other clients in the ROM (V, by import census):

| client | path | functions |
|---|---|---|
| ScDv.dll (WSERV) | boot/UI | 10, 11, 31, 9, 0x8002, 0x8003, 0x2b/0x2c/0x2d, 0x1a (slot +0xa8 in the screen device), 0x8001 (slot +0x20) possibly; `CDirectScreenBitmap`: 0x17/0x18/0x19/0x21, 0x2003/0x2004 only when caps word 2 != 0 |
| screensaver.exe via power_save_display_mode.dll | screensaver | ord1 → ctl 3, ord2 → ctl 4, ord4 → ctl 5, ord5 → caps word 8 |
| einfolcdplugin.dll | phone info | ctl 6 (panel ID string) |
| TvOutConfig.dll, TVOutEngine.dll, DeviceModeHandlerPlugin.dll | TV-out | 0xc, 0xd, 0x1c, 0x20, 0x25–0x2a, 0x8004/0x8005, 0x23/0x24, 0x4001 |
| IvaPostproc*, CamCtrlPrimary.dll | video posting / viewfinder | 0xf–0x16, 0x2001/0x2002 |
| radio_freqs.dll | | ord4 only (caps) |
| accel2d.dll | | does not import r_display (goes through ScDv) |

## 4. Stand-in corrections (`src/standins/d_display/d_display.cpp`)

Our board: QEMU Syborg frame buffer, always live, 32 bpp, portrait memory 240×320 with
960-byte lines, HAL native 320×240. Priorities: **now** = affects the current boot path,
**saver** = needed for the screensaver / power-save display mode, **later** = DSB/TV-out/
posting clients, **n/a** = not needed.

| item | current stub | faithful behaviour on Syborg | prio |
|---|---|---|---|
| caps word 0 (+0) | 1 | 1 (V) | now, already right |
| caps word 1 (+4) | 0 | 1 when the client process priority is 650 (WSERV), else 0 (V); ScDv does not read it — optional | n/a |
| caps word 2 (+8) | **1** (2026-11-06) | 1 as on the device: ScDv's `CDirectScreenBitmap` uses ctl 0x17/0x18/0x19 + req 0x2003/0x2004, implemented in the stand-in (DSB, see below) | done |
| caps words 4, 5 (+16, +20) | 1, 1 | **0, 0** (V: +0x290/+0x28c are 0 on this product); ScDv does not read them | low |
| caps words 6, 7 (+24, +28) | 0, 0 | height/width in current orientation: 240/320 before, 320/240 after req 0x8003 with index 1 or 3 (V); not read by ScDv | low |
| caps word 8 (+32) | 0 | **240** (max partial lines) (V) — `power_save_display_mode` ord5 reports it to the screensaver | saver |
| caps words 9/10/11 (+36/40/44) | 1/1/1 | **0/1/1** (V: 90° unsupported); WSERV only asks for 270 | low |
| caps words 12..16 (+48..+64) | 0,0,0,1,0 | 0,**1**,0,1,0 (V: 64K also supported). Keeping 64K off is a safe simplification since our fb is 32 bpp only | low |
| caps 17..25 | 0 | 0,0,0,0,0,**1,1**,0,0 (V constants) | low |
| ctl 10 | returns 0 for any a1 | a1 is the screen number by value; **0 → KErrNone, else KErrNotSupported** (V) | low |
| ctl 11 | block | as above (26 words, 0x68 bytes) | now |
| ctl 31 | 960 | real = 320 × bytes/pixel = 1280 for 16MU, rotation-independent (V). For us it must equal the stride of the buffer ScDv draws into (our HAL/fb: 960); **keep 960 unless the BSP stride changes** (then change both) | now |
| ctl 9 | ignored, 0 | rect `{x\|w<<16, y\|h<<16}` in current-orientation coordinates, 0xffff = whole; clamp to 240×320; return 0. Our fb is live so no copy is needed; when partial mode (ctl 3) is emulated this is where rows outside the band would be masked | now (no-op is faithful) |
| ctl 8 | default 0 | full refresh → 0 (V) | now, already right |
| ctl 0x2b / 0x2c / 0x2d | ignored, 0 | freeze flag, unfreeze, return swallowed-update count as the **return value** (V) | low |
| ctl 3 | ignored, 0 | validate `start < end < 240` (else **KErrArgument**), remember `{start,end,fullColour}`, set active flag, return 0 (V). Rows = native 320×240 rows = portrait X columns `[start,end]`. Emulation (optional): dim/blank the fb outside those columns (QEMU `blank-dim`-style) | saver |
| ctl 4 | ignored, 0 | clear partial state, return 0 (V) | saver |
| ctl 5 | ignored | write TInt = partial active (0/1) (V) | saver |
| ctl 6 | ignored | write a 24-bit panel ID (any value, e.g. 0) (V); einfolcdplugin prints it | low |
| ctl 7 | ignored, 0 | accept index 0..3, return 0 (V) | n/a |
| ctl 0x1a | **implemented** (2026-11-06): writes the rotation index of the last req 0x8003 | `r_display` ord59 (vtable +0xac) swaps HAL's native 320×240 to the current 240×320 with it; without it `CDirectScreenBitmap::Create(0,0,240,320)` failed -6 (V). The UI was unaffected in tests | done |
| ctl 0x1b | ignored | return 4096 (V) | n/a |
| ctl 0x21 | ignored | copy-without-transfer → 0 (DSB only) | later |
| ctl 0x17 / 0x18 / 0x19 | **implemented** (2026-11-06) | 0x17 create (rect `{x\|w<<16, y\|h<<16}`, a2 1 = double buffer, 2 = incremental), 0x18 BeginUpdate info `{mode 11, address, w, h, w*4, 5, 0}` (-39 while a buffer is begun), 0x19 release. The address is a global user-accessible mapping (`DPlatChunkHw`, `EMapAttrUserRw`), because `r_display` ord45 only treats word 1 as an `RChunk` handle (adding word 6) when **caps word 3** (`this+0x24`, caps are copied to `this+0x18`) is non-zero, and the N95 reports 0 there (V) | done |
| ctl 0xc–0x16, 0x1c, 0x1e, 0x20, 0x25–0x2a (TV-out/posting) | ignored, 0 | real driver without TV-out returns -1/-18/-5; return **KErrNotSupported (-5)** so TvOutConfig/CamCtrl take their "no TV-out" paths instead of believing they succeeded | later |
| unknown fn | 0 | -5 (V) | low |
| req 0x8002 | complete 0 | complete 0 for a1 == 1 (16MU); KErrNotSupported for anything else (our fb is 32 bpp; real would also accept 0 = 64K) (V for the real check) | now |
| req 0x8003 | complete 0 | complete 0 for a1 ∈ {0,2,3} and remember it (for ctl 0x1a and caps 6/7); **KErrNotSupported for a1 == 1** (V) | now |
| req 0x8001 | complete 0 | complete 0 for a1 == 0 (self-test passes), KErrNotSupported for 1/2 (V) | low |
| req 0x23 | completes at once (stub completes every request) | **must stay pending** (notification); complete KErrCancel on ctl 0x24, cancel, close (V). An immediate completion would make a TV-out/mode notifier spin. Add `{KStubRequest\|0x23, EPendRequest}` and a hook for ctl 0x24 | later |
| req 0x2003 / 0x2004 | **implemented** (2026-11-06) | EndUpdate whole / rect (a1 → packed rect, screen coordinates): the posted buffer is copied into the frame buffer at the next vsync (Syborg fb INT_CAUSE bit 0, fixed rate `syborg-fb.refresh-rate`, default 60 Hz) and the request completes then. Measured with `src/n95apps/dsbtest`: 60.0 fps, 16.5–16.9 ms per frame, also for back-to-back EndUpdates | done |
| req 0x2001, 0x2002, 0x8004/0x8005 | complete 0 | complete **KErrNotSupported** (posting/TV-out absent) | later |
| DoCancel | logged only | complete pending 0x23 with KErrCancel; clear partial mode if this channel set it (V) | later |
| factory | version any, units any | real: version 1.1.1540, `KDeviceAllowUnit`, units mask 1; client opens with KNullUnit — current permissive settings are fine | n/a |
| HAL (`lcd_n95` BSP, not this stub) | native 320×240, display-off ignored | matches the real driver's HAL size reporting (V). The real driver honours `EDisplayHalSetState 0` (panel sleep) and `SetBacklightOn/SetDisplayBrightness` (needs cap WriteDeviceData, power status > 1); `ModeCount 2`, `MaxBrightness 100`, `Colors 0x1000000`, stride 320 × bpp | H-lcd-alwayson review |

Minimal "needed now" change set: keep ctl 10/11/31/9 as they are, fix caps word 8 = 240 and
words 4/5 = 0, make 0x8002/0x8003 validate their argument, and add the partial-display trio
(ctl 3/4/5 with state) before the screensaver is reached. Everything in the "later" rows is
about not lying to TV-out/posting/DSB clients.

## 5. Correction from runtime (2026-11-05)

The stand-in first mapped the ctl 3 band to picture columns always, assuming the 270° portrait
rotation set at boot. At runtime WSERV switches the rotation index with req 0x8003 (e.g. 3 → 0
before the screensaver shows), and the screensaver then passes its UI **Y** range. The band is
in native panel rows: picture rows at rotation index 0/2, picture columns at 1/3. The stand-in
now tracks the rotation index and tells the frame buffer which (PARTIAL_MODE bit 2).
