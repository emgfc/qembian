# GPU (OpenGL ES 1.x + EGL) roadmap for the virtual N95

**Status 2026-11-06: phase 0 done** (src/sygl: blob at 0x80320000, 175 entry patches; ZigGLTri
logs `eglInitialize err=12289` = our EGL_NOT_INITIALIZED, Carousel now shows "Feature not supported"
instead of "Memory full"). **Phase 1 milestone 1 reached 2026-11-06:** ZigGLTri's Gouraud triangle spins
at ~29 fps (app-paced to 30) through our EGL/GL/rasterizer (src/sygl: egl.c, gl.c, raster.c,
plat.cpp; 30 KB), vsync via d_display req 0x7f01. Presentation by DSA (H-gles-present).
**Milestone 2 reached 2026-11-06:** ZigGT2Car renders textured and lit at ~20 fps (light.c:
fixed-function lighting; tex.c: texture objects, all ES 1.1 formats; raster.c: perspective-correct
texturing every 16 px, 5 env modes, alpha test, blend, scissor; surfaces/textures in a per-thread
32 MB chunk heap like Nokia's PVRHeap, since apps often have a 1 MB heap). Carousel now gets past
eglInitialize and stops at eglCreatePbufferSurface ("Feature not supported"): milestone 3.
**Milestone 3 in progress (2026-11-06):** pbuffer and pixmap surfaces (CFbsBitmap 565/444/888/
8888/gray, written back at the EGL sync points), eglCopyBuffers, eglBindTexImage, glReadPixels,
glCopyTex(Sub)Image2D, glPolygonOffset, GL limits answered without a context, EGL_NO_DISPLAY
accepted as our display, GL error log and a per-thread EGL call trace (`SYGL: trace`). Carousel
renders its Music page. **Carousel renders correctly (2026-11-06)** once EGL share contexts
share texture objects (Hitchcock uploads in one context and draws in the window's context).
The USER-EXEC 3 seen when *launching a second Carousel instance* is Carousel's own race: its
ConstructL calls CHuiEnv::Release (start in background) and the first view activation (CIdle)
uploads textures before the foreground event's RestoreL. Bringing the boot-time instance to the
front, as the Multimedia key does, works. Found with `sydb catch` (docs/sydb.md).
**Crash Bandicoot Nitro Kart 3D (N-Gage port, owner's copy) races in 3D (2026-11-06)**: menus
~19 fps, races ~4 fps at first, **~11 fps** after power-of-two texel wrapping (no division per
texel) and inline VFP (`-mfloat-abi=softfp -mfpu=vfp`: same ABI, no DFPAEABI call per float op);
GT2Car 20 -> 29 fps. It picks GL only when GL_RENDERER says PowerVR
(H-gles-identity), and polls key state per frame (sydb: hold keys, `key ok:0.6`). Install: exe +
resources + data on C: (an EXE on E: needs a C:\sys\hash entry from the installer). Also checked:
Video centre (icon ring renders), Gallery and Maps (Avkon UI, no GL problems). Camera crashes
(KERN-EXEC 3) because CamServerCore has no camera behind it: a camera stand-in, not GL.
Next for speed: span-based rasterization (skip pixels outside the triangle), integer vertex path
for fixed-point apps, then phase 5 (executor in QEMU). Note: apparc renames app processes to their
caption (`sydb kill Crash`, not cbnk3d).
Debug aids: `src/sygl/build.sh --trace` (SYGL: trace of the first EGL/GL calls, draw boxes),
host tests of the portable rasterizer: `tests/sygl/run.sh`.

Static analysis, 2026-11-06. **[V]** = seen in code/binary/listing, **[I]** = inferred.
Owner's constraints (2026-11-06): no host GPU / host-GL forwarding for now; start with a software
rasterizer; keep room for acceleration later.

## 1. What the N95's stack looks like

| Piece | Facts |
|---|---|
| `libgles_cm.dll` 0x8049d700, code 0x8049d778–0x804c5698, **175 exports** (table at 0x804c05cc), no `.data`/`.bss` [V] | PowerVR MBX GLES 1.1 CM driver. Imports services_um (43), libimgegl (32), DFPAEABI/Estlib/DRTAEABI (float runtime), EUser (4) [V]. 28 of the 29 `egl*` exports are single ARM `b` instructions to its import thunks (`eglChooseConfig`, ord 1, starts with `mov ip, r3`) [V, rechecked] → `libimgegl.dll` (e.g. 0x804c04e4 `eglInitialize` = `b 0x804c0a20` → `libimgegl_ord17`) [V] |
| `libimgegl.dll` 0x80a655d0, 32 exports, imports services_um (22) [V] | IMG EGL: display/config/surface/context bookkeeping over the services bridge; the only importer is libgles_cm [V] |
| `services_um.dll` 0x80a624a0, 102 exports [V] | PVR services user mode. Kernel bridge: 1× `User::LoadLogicalDevice`, 1× `RBusLogicalChannel::DoCreate`, 2× `DoControl(int,void*,void*)`, 1× `DoRequest`, 2× `DoCancel`, `RChunk` global chunks, `"PVRHeap"`, reads `powervr.ini` through RFs/RFile [V]. A single generic DoControl carries the whole PVRSRV bridge (dozens of commands: device info, heaps, mem alloc/map, sync objects, TA/3D kicks) [I] |
| `pvr2d.dll`, `pvr2dif.dll`, `libpvrTechviewWSEGL.dll` (1 export = `WSEGL_GetFunctionTablePointer`), `libpvrNULLWSEGL.dll` [V] | Window-system glue. The Techview WSEGL imports Ws32 `CDirectScreenAccess::NewL/StartL`, `RWindowBase::AbsPosition/Size`, FbsCli `CFbsBitmap::DataAddress/LockHeap/Create`, BitGdi `CFbsScreenDevice::Update`, `TAcceleratedBitmapSpec`, pvr2d blits [V]: i.e. Nokia's own frame presentation is **DSA + a screen-device update**, with pixmap surfaces on `CFbsBitmap` pixels |
| Kernel side `pvrsrv.dll` 0x801b5d80, `services.ldd`, `pvromaplcd.dll`, `omaplcd.ldd` [V] | In the N95 kernel area, not in our hybrid ROM; `LoadLogicalDevice("services")` → −1, `eglInitialize` → `EGL_BAD_ALLOC` [V, gdb 2026-11-06] |
| XIP callers of libgles_cm: `hitchcock.dll` (0x81cfaba0, 70 GL/EGL imports), `m3gcore.dll` (0x83520c20, 80 imports); `carousel.exe` imports none directly [V] | Their import thunks are resolved at ROM build to libgles_cm's function addresses, not through the export table [I, XIP convention] |
| RAM-loaded apps (ZigGLTri from C:) | Bound by the loader through libgles_cm's export table (ordinal → address) [I] |

Ordinal map (from the SDK `libgles_cm.dso`, 175 symbols, matches the DLL's 175 exports [V]; saved
in this doc as the authoritative list): 1–25 EGL 1.0 (`eglChooseConfig`=1, `eglCopyBuffers`=2,
`eglCreateContext`=3, `eglCreatePbufferSurface`=4, `eglCreatePixmapSurface`=5,
`eglCreateWindowSurface`=6, `eglDestroyContext`=7, `eglDestroySurface`=8, `eglGetConfigAttrib`=9,
`eglGetConfigs`=10, `eglGetCurrentContext`=11, `eglGetCurrentDisplay`=12, `eglGetCurrentSurface`=13,
`eglGetDisplay`=14, `eglGetError`=15, `eglGetProcAddress`=16, `eglInitialize`=17, `eglMakeCurrent`=18,
`eglQueryContext`=19, `eglQueryString`=20, `eglQuerySurface`=21, `eglSwapBuffers`=22,
`eglTerminate`=23, `eglWaitGL`=24, `eglWaitNative`=25); 26–132 GLES 1.0 core in alphabetical order
(`glActiveTexture`=26 … `glViewport`=132); 133 `eglSwapInterval`; 134–172 GLES 1.1 additions
(`glBindBuffer`=134, `glBufferData`=135, `glBufferSubData`=136, `glClipPlanef/x`=137/138,
`glColor4ub`=139, `glDeleteBuffers`=140, `glGenBuffers`=141, `glGet*`=142–161, `glPointParameter*`=
162–165, `glPointSizePointerOES`=166, `glTexEnvi/iv`=167/168, `glTexParameterfv/i/iv/xv`=169–172);
173 `eglBindTexImage`, 174 `eglReleaseTexImage`, 175 `eglSurfaceAttrib`. Function address per
ordinal = word `0x804c05cc + 4*(ord-1)` [V]; e.g. `glClear`=31 @0x804a01e4, `glDrawArrays`=54
@0x804b55b4, `glViewport`=132 @0x804aac48.

## 2. Architecture options

| Option | What it needs | Effort | Risk | Verdict |
|---|---|---|---|---|
| **A** emulate PowerVR MBX, run `pvrsrv.dll`/`services.ldd`/`omaplcd` | Add Nokia's kernel-side modules to our ROM (they import `kaomap`, `power`, `r_display_kernel`, `dma` – OMAP2420 ASSP [V for d_display's siblings, I for pvrsrv]); a QEMU MBX model: undocumented register file, TA/ISP/TSP command streams, VGP vertex programs, PVRTC textures, tiling | XL (months) | very high; no documentation, debugging a GPU model against a closed driver | no |
| **B** stand-in `services` LDD, keep `libgles_cm`/`libimgegl`/`services_um` | Reproduce the PVRSRV bridge (device info, heaps, chunk-backed memory, sync objects) **and** consume what libgles_cm writes into those buffers: MBX hardware command/vertex streams. That is option A's hardest part without the register model | XL | very high; the bridge's command numbers/structs would need full RE (the brief's "mock the bridge" rule stops here: heavy emulation) | no |
| **C** replace the implementation behind the 175 `libgles_cm` entry points, user side byte-identical except code-patches | 175 entry-point trampolines + our own GLES 1.1/EGL implementation placed at a fixed ROM address. No bridge at all: libimgegl/services_um/pvr2d are never entered | C1 software: M–L (a few weeks for milestone 1–2); C2 host GL: +M | medium; all under our control; perf on TCG | **yes (C1 first)** |
| C1 on-target software rasterizer | portable C rasterizer in the guest; frames presented by DSA (as Nokia's WSEGL does) | M–L | perf on TCG ARM1136 (ARM1136 + VFPv2 in TCG, no NEON) [I] | start here |
| C2 paravirtual QEMU device → host GL/Mesa | the same GL command stream shipped to a QEMU device; host executes with llvmpipe/OSMesa/host GL and writes pixels back | M after C1 | host dependencies, sync, texture upload traffic | later phase |
| **D** replace `libgles_cm.dll` wholesale in the ROM (our DLL with the same 175 ordinals) | romcompose would swap a 163 KB file instead of patching; everything else as C | same as C, minus the trampoline generator | breaks "user side byte-identical except code-patches"; XIP importers (hitchcock, m3gcore) are bound to the old function addresses, so the new DLL must keep every export at the old address anyway → it degenerates into C | no; C is D done properly |

### How ordinals and exports constrain C

* RAM-loaded apps (ZigGLTri) bind by ordinal through the export table at 0x804c05cc: patching the
  table (175 words) would redirect them, but **not** hitchcock/m3gcore, whose thunks already hold the
  function addresses [I]. Patching the **function entries** redirects both, so that is the single
  mechanism: 175 patches, one ARM instruction each (`b our_fn`, range ±32 MB) if our code sits in
  the ours window (our files end ≈ 0x8030_1474; the N95 side starts at 0x8038_f9a0, so ≈ 570 KB free,
  about 1.6 MB below libgles_cm [V manifest, rechecked]), or
  8 bytes (`ldr pc,[pc,#-4]; .word`) if it sits after the N95 image (0x83a0_xxxx).
* 28 of the 29 EGL entries are already 4-byte branches [V], so their replacement is exact; GL entries
  start with ordinary ARM code [V], overwriting the first word is safe because nothing returns into
  the old bodies.
* The libgles_cm → services_um / libimgegl imports stay in the ROM but are dead code.
* Nothing imports `libimgegl` or `services_um` except each other and libgles_cm [V, `syb_sym callers`],
  so no other client of the bridge needs a stand-in. `pvr2d.dll` is only used by the WSEGL plugins [V].

### How our code gets into the ROM (new tooling, enabling all of C)

Our hybrid composes "ours" (rombuild output, kernel side) + the N95 core image; our *user-side*
files in the ours image are dead because their imports resolve to our EUser, not the N95's [V,
romcompose.py header]. Proposal: a **prelinked XIP blob**:
1. Build the stand-in with GCC 14 symbianelf (as `src/n95apps/*` do, no C++ EH, `-fno-exceptions`),
   `-mlong-calls`, linked at a fixed ROM address with a generated linker script that `PROVIDE`s the
   N95 export addresses it needs (`User::Alloc`, `UserSvr::DllTls/DllSetTls(int,...)` [V in 9.2 euser.dso],
   `RWsSession`, `CDirectScreenAccess`, `CFbsBitmap`, `CFbsScreenDevice::Update`, `RBusLogicalChannel`),
   addresses from `tools/syb_sym.py sym` / core.json. No elf2e32, no loader involvement.
2. Constraint: **no writable static data** (ROM). All state on the heap, anchored per thread with
   `UserSvr::DllSetTls(KMagicKey, ptr)` [I: EKA2 TLS is keyed by an int handle; any value works].
   EGL state is per thread anyway.
3. `tools/romcompose.py --blob ADDR FILE` places the raw `.text/.rodata` at ADDR (ours window after
   `oend`, checked against `ustart`), and `tools/gen-gles-patches.py` emits the 175 trampolines into
   `models/<id>/code-patches.txt` from the ordinal map above (original bytes verified at compose
   time, as today).
4. The blob runs in the calling process with its capabilities; all it needs is EUser/Ws32/FbsCli/
   BitGdi exec paths, which every GL app already has.

### Backend interface (room for acceleration)

Split the stand-in into (a) **front end**: EGL objects, GL state, matrix stacks, client arrays,
texture objects, T&L and lighting (pure C, portable, no Symbian headers) that emits a compact
**command stream** (clear, draw N triangles with post-T&L attributes, texture upload, read pixels,
present); (b) **executor**: phase 1 = the same C rasterizer linked into the blob; phase C2 = the
stream written to a shared chunk and kicked through a new QEMU device at a free slot
(0xc000a000–0xc000c000, irq 9–11 are unused [V, syborg.c header]) whose host side runs the
*identical* rasterizer code natively (10–30× faster than TCG [I]) or host GL. The front end and
the presentation code do not change between the two.

## 3. Getting frames to the screen

| Path | Pros | Cons |
|---|---|---|
| **DSA** (`CDirectScreenAccess` + `CFbsBitGc::BitBlt` + `CFbsScreenDevice::Update`) | What Nokia's WSEGL does [V]; WSERV clips to the visible region, handles window position (`RWindowBase::AbsPosition`), overlapping notes/menus; the screen device is already in WSERV's rotation and our fb holds the picture in that rotation (H-lcd-native-landscape), so **no rotation work** for us; `Update` is a no-op cost on our live fb (ctl 9 no-op) | DSA abort/restart on window changes (must re-`StartL` on `AbortNow`, as Techview WSEGL does); the blit is a CPU copy (76.8k px, fine) |
| WSERV `CWindowGc::BitBlt` in the app's redraw | simplest clipping | needs the app's draw cycle; `eglSwapBuffers` has no gc; wrong model |
| **DSB** (our H-dsb) | vsync-paced EndUpdate, no WSERV copy | one DSB device-wide, portrait only, no region clipping [V hacks.md]; collides with WSERV/other DSB users |
| QEMU overlay | zero guest copies | not yet a device; invisible to WSERV (notes/dialogs would be hidden); defer to C2 |

Pacing: `eglSwapBuffers` = present the back buffer by DSA, then wait for the next vsync. Add a
"wait for vsync" request to our `d_display` stand-in (new request number outside the real set, the
channel opened once per EGL display with `DoCreate(L"DisplayDriver", TVersion(1,1,1540))` as
`r_display` does [V]); the fb raises VSYNC at 60 Hz (H-vsync). `eglSwapInterval(0)` skips the wait.
Colour: render internally in 32 bpp (`EColor16MU`, our fb/ScDv mode 1); report
`EGL_BUFFER_SIZE 16/24/32` configs with a 16-bit depth buffer. ZigGLTri asks for
`EGL_WINDOW_BIT, BUFFER_SIZE 16, DEPTH_SIZE 16` [V]; `eglChooseConfig` must sort so a config is
returned (it also re-creates the surface on `EEventScreenDeviceChanged` and expects
`eglQuerySurface(EGL_WIDTH/HEIGHT)` = window size [V]).

## 4. Milestones and entry points

| Milestone | Entry points (ordinal) | Renderer features |
|---|---|---|
| **M1 ZigGLTri** (spinning colour triangle) | EGL: 14 GetDisplay, 17 Initialize, 1 ChooseConfig, 6 CreateWindowSurface, 3 CreateContext, 18 MakeCurrent, 22 SwapBuffers, 21 QuerySurface, 15 GetError, 7/8 Destroy*, 23 Terminate (+9 GetConfigAttrib, 133 SwapInterval for the test). GL: 132 Viewport, 90 MatrixMode, 82 LoadIdentity, 65 Frustumf, 129 Translatef, 109 Rotatef, 116 ShadeModel, 32 ClearColor, 31 Clear, 57 EnableClientState, 131 VertexPointer, 41 ColorPointer, 54 DrawArrays [V app sources + brief] | triangle list/strip/fan, Gouraud, viewport, depth (clear only), DSA presentation |
| **M2 ZigGT2Car** (textured lit car) | + 29 BindTexture, 67 GenTextures, 125 TexImage2D, 170 TexParameteri, 167 TexEnvi, 120 TexCoordPointer, 97 NormalPointer, 77 Lightfv, 73 LightModelfv, 87 Materialfv, 56/52 Enable/Disable, 53 DisableClientState, 49 DepthMask, 30 BlendFunc, 38 Color4f, 139 Color4ub, 106/105 Push/PopMatrix, 83 LoadMatrixf, 113 Scalef [V main_gt2.zig] | depth test, lighting (≤8 lights, materials, two-sided off), texturing (RGB/RGBA/565/4444, min/mag filters, mipmaps, wrap), blending, perspective-correct texcoords |
| **M3 Hitchcock/Carousel + M3G** | hitchcock [V]: + 2 CopyBuffers, 4 CreatePbufferSurface, 5 CreatePixmapSurface, 12/13 GetCurrent*, 24/25 WaitGL/WaitNative, 26 ActiveTexture, 37 ClientActiveTexture, 40 ColorMask, 42 CompressedTexImage2D, 44 CopyTexImage2D, 46 CullFace, 47 DeleteTextures, 48 DepthFunc, 55 DrawElements, 58 Finish, 60/61 Fogf/fv, 68–70 GetError/Integerv/String, 147 GetFloatv, 91 MultMatrixf, 98 Orthof, 100 PixelStorei, 103 PolygonOffset, 108 ReadPixels, 115 Scissor, 121/122/123 TexEnvf/fv/x, 127 TexParameterx, 128 TexSubImage2D. m3gcore [V] adds: 19 QueryContext, 20 QueryString, 27 AlphaFunc, 33/35 ClearColorx/ClearDepthx, 39 Color4x, 50 DepthRangef, 63 Fogxv, 64 FrontFace, 71 Hint, 76 Lightf, 72 LightModelf, 86 Materialf, 99 Orthox, 62 Fogx | pixmap surfaces on `CFbsBitmap` pixels (16/32 bpp, LockHeap), pbuffers, `eglCopyBuffers` to a bitmap, multitexture (2 units), scissor, fog, alpha test, cull, polygon offset, `glReadPixels`, PVRTC decode for `glCompressedTexImage2D` (MBX's only compressed formats [I]; Carousel skins may use it), `glGetString(GL_EXTENSIONS)` listing what hitchcock probes [I; check with sydb strace/gdb when there] |

Fixed-point (`x`) variants are thin wrappers; implement them from the start (M3G uses them).

## 5. Recommended path and phases

Recommendation: **C1** (trampolines + prelinked blob + portable software GLES 1.1 with a
command-stream executor), C2 later with the same front end.

| Phase | Work | Check through sydb |
|---|---|---|
| 0 Tooling | `tools/gen-gles-patches.py` (ordinal→address table above → 175 patches), `romcompose.py --blob`, a `src/n95apps/syglesw/` (or `src/sygl/`) build.sh producing the fixed-address blob, linker script generator from `syb_sym`. First blob = every entry returns `EGL_FALSE`/sets `EGL_NOT_INITIALIZED` | `sydb push ZigGLTri.exe` + reg.rsc, `sydb launch 0xA0000047`; `sydb log --grep gl_init` shows our error code, not `EGL_BAD_ALLOC` (0x3003); Carousel still fails but with our error |
| 1 M1 | EGL objects, TLS state, config list, window surface = DSA + back buffer, vsync request in `d_display`; rasterizer core (clear, Gouraud triangles); the 15 GL entries of M1 | `sydb launch 0xA0000047; sydb screenshot` shows the triangle; `--changed` across 1 s shows rotation; serial `SYGL:` fps line ~60 (vsync) |
| 2 M2 | depth, lighting, textures, blending, matrix stack; `glGet*` basics | ZigGT2Car screenshot vs. owner's device/EKA2L1 shot; fps logged |
| 3 M3a Carousel | pixmap/pbuffer surfaces, CopyBuffers, DrawElements, scissor, fog, multitexture, compressed textures, extension string; stub `eglWaitGL/Native` | `sydb launch 0x10282bf6` renders the Carousel instead of "Memory full"; `sydb strace`/`dbg` on hitchcock's leave code if not |
| 4 M3b M3G | fixed-point entries, `eglQueryString`, remaining `glGet*` | a MIDP M3G demo (owner to pick) |
| 5 C2 acceleration | QEMU `syborg-gpu` device (shared chunk + doorbell + done irq), executor built into QEMU from the same C sources; later host GL | same screenshots, fps up; `sydb hmp 'info qtree'` lists the device |

New hacks for `docs/hacks.md`: **H-gles-trampolines** (175 entry patches in libgles_cm.dll),
**H-gles-blob** (our prelinked XIP code in the ours window, no loader), **H-vsync-wait**
(d_display request outside the real set), **H-gles-dsa** (window surfaces presented by DSA, abort
→ restart, no partial-update/TV-out). Patches also mean `models/<id>/code-patches.txt` grows by a
generated section; keep it generated, not hand-edited.

Rasterizer base: write the front end ourselves; for the rasterizer either our own (~3–5 k lines
for M1–M2) or a permissively licensed base (TinyGL, zlib-style; Vincent ES 1.1, BSD, once
targeted at Symbian) [I: licences to be checked by the owner; GPL sources stay naming references].

## 6. Biggest risks

1. ROM policy: 175 code patches plus a raw blob stretch "user side byte-identical except patches";
   if unacceptable, option D (swap the DLL, keep export addresses) is the fallback and is the same code.
2. Performance under TCG for M2/M3 (textured, depth-tested full screen): expect 10–30 fps [I];
   C2 fixes it without touching the front end.
3. Hitchcock may depend on MBX specifics (extension names, config attributes, PVRTC textures,
   `glReadPixels` formats) that only show up at runtime; budget a gdb/strace iteration.
4. DSA restarts and WSERV interplay (focus loss, notes) must not hang the app or leave stale pixels.
5. No static data in the blob: every path must reach state through TLS; a forgotten global is a
   ROM write fault.

## 7. Open questions for the owner

* Accept trampolines + blob as code-patches, or prefer replacing `libgles_cm.dll` in the ROM?
* Rasterizer: own code or a permissive base (which licence is acceptable)?
* Surface colour depth: expose 16-bit configs only (what the apps ask for) or 32-bit too?
* Is Carousel the real M3 target, or M3G demos first (M3G needs fewer MBX-specific behaviours)?
* Is a per-process `DisplayDriver` channel for the vsync wait acceptable, or should the wait live in
  a new tiny LDD (`sygpu.ldd`) that C2 will need anyway?

## Fidelity: window surfaces and resizes/rotation (2026-11-06)

The owner saw, on the real N95 8GB, ZigGLTri drawing off-centre and only partly filling the screen
after a switch to landscape, whatever the app did (recreating surfaces on the rotation event did not
help). sygl instead follows the window (H-gles-window-resize), so it is more permissive than the device.

Evidence from the device's own window-system glue, `libpvrTechviewWSEGL.dll` (ROM 0x80a6xxxx,
decompile `build/n95/ghidra/libpvrTechviewWSEGL.dll.c`):
- `RWindowBase::Size()` and `AbsPosition()` are called in exactly one place, FUN_80a6c76e (window
  surface creation: rect -> FUN_80a6c3b2, then `CDirectScreenAccess::NewL`). [verified]
- FUN_80a6c3b2 (set rect) swaps width/height when the surface's rotation field (+0xc) is 1 or 3,
  i.e. the back buffers are kept in the panel's native orientation and the blit (pvr2d) rotates;
  buffers are reallocated only when that size changes. [verified: code; meaning of +0xc inferred]
- The DSA restart callback (FUN_80a6c33c) gets only the new clipping region; it checks whether the
  region is the single stored rect (full visibility flag) and does not query the window again. [verified]
- So on the device a window surface keeps its creation size and position: after WSERV resizes or
  rotates the window, rendering continues into the old rect, clipped by the new region. [inferred,
  matches the owner's ZigGLTri observation]

Open: how Hitchcock (Carousel) copes on the device; on sygl it asks eglQuerySurface after the layout
switch and sets glViewport from it, without recreating the surface. Carousel passes "orientation ==
landscape" to Hitchcock (carousel.exe: hitchcock ord145(display, Orientation() == 2)), so it may
rotate its own rendering instead. Check on the real phone before making strict the default.

Done 2026-11-06 (owner: compatibility, not imitation): sygl is strict, there is no follow mode
(H-gles-window-fixed). Next: the rotation field. With strict sizes Carousel's landscape switch draws
its 240x320 surface at (0,0) of the 320x240 screen; Carousel works on the device, so the real
driver's native-orientation buffers + rotated pvr2d blit (FUN_80a6c3b2 swaps w/h at rotation 1/3)
probably matter. RE that path (pvr2d ordinals used by WSEGL's present) before the host-GL backend.

## Host GL backend (done 2026-11-06, H-gles-host)

Phase 5 ("real accel") in a cheaper shape than a full GL forwarder: only the rasteriser runs on the
host. sygl stays the single implementation of GLES 1.1/EGL semantics (strict WSEGL surfaces
included), so switching the backend changes speed, not behaviour.

- Protocol: `src/qemu-syborg/hw/arm/syborg_gl_proto.h` (SURF, SURF_FREE, TEX in row bands,
  TEX_FREE, CLEAR, DRAW (state + triangle list, batched while state and surface stay the same),
  READ in row bands, OWNER, OWNER_FREE).
- Guest: `src/sygl/hw.c` (hooks in raster.c; read-backs before every CPU read of a surface:
  present, glReadPixels, CopyTex*, eglBindTexImage). Texture generation `SyTex.gen` (0 = changed).
- Kernel: DisplayDriver ctl 0x7f10 {ready, thread id}, 0x7f11 {cmd, len, out, outLen}.
- Host: `syborg_gl.c`, libepoxy, EGL_MESA_platform_surfaceless, GLES 2 shader with the ES 1.1
  texture environments, alpha test and fog.
- Setting: `--gl host|soft` (sydb start/phone), `GL=soft` (run-gui), `-global syborg-gl.backend=off`.
- Numbers: Crash Bandicoot Nitro Kart 3D 60 fps menus and races (vsync), software 19/11.

Open: the WSEGL rotation path (Fidelity above); vertex transform is still the guest's (VFP in TCG):
if a scene becomes vertex-bound, move transform/lighting too.
