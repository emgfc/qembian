# N8 (RM-596): window-server composition, EGL/OpenVG, and what to replace

Research note (2026-11-07), static analysis only. Firmware 011.012. Tags: [verified] = seen in ROM
bytes, import tables or RTTI strings; [inferred] = best guess, not checked at run time.

## Chain at a glance

```
EwSrv.exe --RENDERSTAGES alf compositor-->
  alfrenderstage.dll (alf, ECOM 0x200184B6)  -> alfdecoderserverclient -> alf server (huirenderpluginvg -> libEGL/libOpenVG)
  alfclosedrs.dll    (compositor, 0x2002BCF9) -> gce.dll ord1 -> compositionbackend.dll ord1
                                              -> vcbackendserver.dll (server thread "!VC_COMPOSITION_BACKEND_SERVER")
                                              -> rdispman.dll -> vcdispmanldd.ldd -> vcdispman*.dll -> vchi.dll (MPHI/CCP2 to the GPU)
libEGL.dll / libOpenVG.dll (ROFS1, tiny forwarders) -> khmangle.dll / khclient.dll -> khegl_ldd.ldd -> VideoCore
```

## Q1. Who implements the "compositor" render stage, and what does it call

- ECOM implementation 0x2002BCF9 is **rofs1 sys/bin/alfclosedrs.dll** (UID3 0x2002BCF9, one export =
  ImplementationGroupProxy, byte-pair compressed). [verified]
- It imports **gce.dll ordinal 1 only**. It does not import compositionbackend.dll. [verified]
  Its other imports: wsgraphicdrawer (render-stage base classes, ords 15/50/83-100),
  `CFbsScreenDevice::GetSurface(TSurfaceId&)`, RSurfaceManager (Open/Create/Open/Map/Close surface, SurfaceInfo,
  GetBufferOffset, GetSurfaceHint, SynchronizeCache), surfaceupdateclient ords 6-8, alfdecoderserverclient 10/12,
  TvOutBehaviour 1, CRepository::Set/NewL, CFbsBitmap data access. [verified]
- gce.dll (UID3 0x1028372B; source path `sf/os/graphics/graphicscomposition/graphicscompositionengine`;
  classes Gce::CGce, CGceImpl, CLayerImpl, MLayer, MBackendLayer, MDisplayControl, CDirtyRectTracker,
  CSurfaceHash). [verified]
  - ord1 = factory, `CGce* NewL(TInt aScreen)`-shaped: allocates a 60-byte object, ConstructL(aScreen). [verified
    from code shape; the name is inferred]
  - ConstructL calls **compositionbackend.dll ord1(aScreen)** and keeps the returned object at +12; everything
    after that goes through that object's vtable (first call: slot at vtable offset 40). [verified]
  - ord2 is at the very end of gce's text: probably exported data (a const/typeinfo). [inferred]
- The other importer of gce ord1 is core `Sys/Bin/10286378.dll` (two call sites, `CGceWrapper`). [verified]

So the compositor talks to the backend through a **single factory export + a Nokia-private C++ vtable**.

## Q2. compositionbackend.dll

- Role: Nokia-private C++ backend under gce for the VideoCore. Classes: Gce::CCompositionBackend (abstract
  base), CVCCompositionBackendGlobal, CVCDisplayCompositionBackend, CVCInternalDisplayCompositionBackend,
  CVCTvDisplayCompositionBackend, CVCSceneCompositionManager, CVCCompositionQueueManager, CVCEndpointGceLink,
  TV-out helpers. Not an OpenWF-C API (no wfc* anywhere; libwfc is SDK-only). [verified]
- It is a **client of a server thread**: CVCCompositionBackendGlobal::ConstructL starts the
  `!VC_COMPOSITION_BACKEND_SERVER` (vcbackendserver.dll ord28) inside EwSrv, then connects a client session.
  The server (vcbackendserver.dll) does `User::LoadLogicalDevice("vcdispmanldd")` and dies with
  `GCEBE Srv Thread 2` (KErrNotFound) here. It also opens `khegl_ldd.ldd`. [verified strings; panic path from the log]
- Exports: 72 slots, 50 used (the brief's "50"). gce and vcendpointgceserver import **ord1 only**. [verified]
  ord1 = `InstanceL()` of the global object, then `CreateBackendL(global, aScreen)`. Error strings: one GCE
  instance per screen. [verified shape; names inferred]
- Returned object (internal display, RTTI vtable at 0x8102eb64): primary vtable **33 slots** (2 dtors + 31),
  plus two secondary bases: one at +4 with 4 slots (MCompositionSurfaceUpdate-like: ContentUpdated) and one at
  +0x5C with 9 slots (MDisplayControl: resolutions/configuration). Many primary slots are tiny return-constant
  stubs. [verified layout; semantics per slot inferred]
- No software, null or emulator backend: no alternative DLL name, no CenRep switch, no `#ifdef`'d emulator
  strings in gce.dll or compositionbackend.dll. [verified as far as strings go]

## Q3. EGL / OpenVG

- ROFS1 `libEGL.dll` (UID3 0x10281AB8, 249 exports, 1.8 KB code) and `libOpenVG.dll` (0x10281AB9) import only
  **khmangle.dll** (34 and 87 imports): pure forwarders to the Broadcom KHRN client (khmangle/khclient,
  EGL extension string includes `EGL_SYMBIAN_COMPOSITION`, `EGL_NOK_resource_profiling`). [verified]
  They do not select the software VG. [verified: no other imports]
- Core `libopenvg_sw.dll` (89 exports; imports fbscli, i.e. renders into CFbsBitmap), `libopenvgu_sw.dll`,
  `libvgi.dll` (14 exports, VGI bind-to-bitmap API) and `SWVG.dll` (wrapper over these) form a software
  OpenVG used by the SVG/NVG icon path. No EGL in it. Export names: no .dso, ordinals only. [verified imports;
  that their ordinals follow libOpenVG.dso order is inferred]
- What alf needs (huirenderpluginvg.dll, the only hui render plugin in the ROM): 23 EGL calls
  eglGetDisplay, Initialize, Terminate, BindAPI, ChooseConfig, GetConfigAttrib, CreateContext,
  DestroyContext, CreateWindowSurface, CreatePbufferSurface, **CreatePbufferFromClientBuffer** (VGImage),
  CreatePixmapSurface (CFbsBitmap), DestroySurface, MakeCurrent, SwapBuffers, QuerySurface, SurfaceAttrib,
  QueryString, GetError, GetCurrent{Context,Display,Surface}, ReleaseThread; 48 OpenVG + 3 VGU
  functions. [verified]
- Native window type: `RWindow*` (Symbian EGLNativeWindowType). With EGL_SYMBIAN_COMPOSITION the EGL
  window surface is an RSurfaceManager surface set as the window's background surface and pushed with
  surfaceupdate. The compositor then composes it. [inferred, standard Symbian^3 design]
- Configs: alf is OpenVG-only (EGL_OPENVG_BIT, 32-bit ARGB/premultiplied, likely alpha-mask bits).
  [inferred; read the exact attrib lists at run time with sydb when needed]

## Q4. Ranked paths

1. **(a) Stand-in compositionbackend.dll** (ord1 + the 33/4/9-slot vtable that gce uses; reserve the other
   exports as stubs returning KErrNotSupported). It composes layers (surfacemanager surfaces + rects) into the
   Syborg framebuffer with memcpy and alpha blend. About 600-1200 lines. Cost: reverse the meaning of about
   31 slots from gce's call sites (gce is ARM, 38 KB). This is the narrowest seam under both gce users
   (alfclosedrs and the "display" stage). **It gets EwSrv up, but it does not paint the UI by itself.**
2. **(c) Our own libEGL.dll + libOpenVG.dll (+libOpenVGU)** replacing the ROFS1 forwarders, with SDK .dso
   ordinals. VG goes to libopenvg_sw/libvgi (in ROM, software) or to a host VG behind sygl. EGL window
   surface = surfacemanager surface + SetBackgroundSurface + surface update. Needed for any S60^3 UI: alf
   draws everything. Size: EGL around 800 lines; VG is free with libopenvg_sw if its ordinals map, otherwise
   a host backend.
3. **(d) Own wserv render stage** (ECOM, MWsRenderStage + MWsGraphicsContext over BitGdi into the
   framebuffer), Wsini `RENDERSTAGES qembian`, no `alf`. This skips gce, compositionbackend and EGL. The
   interface is the public Symbian Foundation wserv plugin API (EPL, the same licence as our kernel; the headers
   are not in this SDK). Risk: S60^3 apps whose drawing assumes alf (effects, home screen widgets, Photos)
   can fail; the alf server start may need its own stub. Size around 1500 lines (the graphics context is
   large).
4. **(b) Wsini with the built-in `display`/`flickerbuffer display` stages** (10286378.dll): **not
   cheaper**. They also go through gce ord1, and they draw with DirectGDI (82 imports), whose adapter needs
   EGL/VG/SgResource. `graphicsresourceadapter.dll` is a 204-byte stub with no imports. [verified sizes;
   that this is unusable is inferred]

**Do first: (a)**, as a mostly-stub compositionbackend that only blits the topmost full-screen layer. It
gives the fastest proof that EwSrv survives and shows which backend slots the boot really calls (trace them
as they are hit). Then do (c): the UI needs EGL+VG whatever the composition. Keep (d) as a fallback if the
gce vtable turns out to be too opaque.

## Q5. Is Raspberry Pi userland a protocol reference?

**Partly: the same family at the service level, but a different transport and generation.**
- `vcdriver.dll` (1 MB) carries VideoCore-side code with VMCS source paths
  `middleware/dispmanx/dispmanx.c`, `applications/vmcs/khronos/khronos_server.c`,
  `middleware/khronos/common/2727b0/hwcommon.c` (BCM2727), `interface/vchi/message_drivers/videocore/mphi_ccp2.c`,
  `applications/vmcs/vchi/gce_rpc/resource_handlers.c`, `EGL_SERVER_SURFACE_T`, `khrn_worker`, "CTRL SERVICE".
  Fourccs found: `KHRN`, `BUFM`, `FILE`, `HOST`, `VCHI`. `vcdispmanvchi.dll` has `DISP`. [verified strings]
  It is the same Broadcom VMCS tree that Pi userland comes from (dispmanx, khronos_server, KHRN/DISP services).
- Differences: N8 uses **VCHI over MPHI/CCP2** (`DVCHI`, MeSSI-16/CCP2 CRC strings), not the Pi's
  VCHIQ shared-memory slots. The message layouts are from about 2008 (VideoCore III), the Pi's from VC IV. There
  is also an N8-only `gce_rpc` service. So Pi `vc_dispmanx_*` / `khrn_client_rpc` give naming and
  general shape only, not byte-exact formats. [inferred]
- Layer: vchi.dll, vcdispman.dll, vcdispmanvchi.dll, vcdispmanupdatehandler.dll and the LDDs are **kernel
  side** (DVCHI, DfcQ, ekern imports). Serving DISP/KHRN ourselves would mean a fake vchi.dll kernel
  extension or an MPHI device in QEMU, plus a server for KHRN RPC (a whole remote GL/VG). That is heavy
  emulation, far more than (a)+(c). Not recommended. [inferred]

## Open risks

- Slot semantics of the backend vtable are inferred. gce may also query the MDisplayControl secondary base
  (resolutions, rotation) and need sane answers (360x640, Color16MAP).
- The screen surface: alfclosedrs takes `CFbsScreenDevice::GetSurface`, so scdv/our display stand-in must
  give a valid surfacemanager surface for screen 0.
- surfaceupdateserver is started by compositionbackend (`StartSurfaceUpdateServer`). The stand-in must start it
  and register as its provider, or EGL/video content updates go nowhere.
- libopenvg_sw ordinals are unnamed. If they do not match libOpenVG.dso order, VG needs an ordinal map
  or another backend.
- vcendpointgceserver (EGL endpoints / video) also calls backend ord1. Return a backend without endpoint
  support and expect video playback to fail.

## Backend interface (as used by gce.dll; checked 2026-11-07)

Source: decompilation of gce.dll and compositionbackend.dll (`tools/ghidra/decompile.sh`, kept in
build/, git-ignored) analysed by an external agent; the construction path, the extension UIDs and the
constant slots (4 -> 1, 5 -> TVersion 1.8.1, 9 stores the rotation, 10 -> 0, 15 -> 0) were checked
against the ROM bytes. Implemented by `src/gcebe/gcebe.c` (H-gcebe, models/nokia-n8-rm596/hacks.md).

- Factory: compositionbackend ord1 `(TInt aScreen)` -> object; gce keeps it at +0x0C, asks
  `Extension_(0x10286497)` for MDisplayControl (+0x10), then slot 10 (dirty-rect tracking: 0 = none).
- Object: primary vtable (33 slots, CBase: 0/1 destructors, 2 `Extension_(uid, TAny*&, TAny*)`,
  3 Release), MCompositionSurfaceUpdate at +4 (4 slots; UID 0x10286498), MDisplayControl at +0x5C
  (9 slots: NumberOfResolutions, GetResolutions, GetConfiguration, SetConfiguration,
  PreferredDisplayVersion, NotifyOnDisplayChange(+Cancel), NotifyOnConfigChange(+Cancel)).
- Primary slots: 4 ApiVersion, 5 InternalVersion, 6 AttachLayer(L), 7 DetachLayer(L),
  8 ResizeLayerRegionStorage(L, n), 9 SetScreenRotation(r), 10 UseDirtyRectTracking, 11
  SupportsRotation(r), 12 ScreenSize (hidden TSize* in r0), 13 ContentUpdated (= +4 slot 2), 14
  SurfaceSize(out, &id), 15 returns 0 (meaning unknown), 16 Resume, 17 Pause, 18 RegisterSurface(&id),
  19 UnregisterSurface(&id), 20 Stage(regions, L, &target, sync on stack), 21 Commit(&target,
  TRequestStatus*), 22-30 the MDisplayControl calls again, 31/32 unused by gce.
- Layers are gce's CLayerImpl objects; the backend pulls their state through the layer's vtable:
  1 GetSourceRectangle(TRect&) -> explicit crop?, 3 GlobalAlpha, 5 GetDestinationRectangle(TRect&),
  7 SourceRotation, 14 NextLayer, 15 PreviousLayer, 17 Surface() -> const TSurfaceId*, 25 ZOrder,
  27 HasNullSurface. A zero TSurfaceId target = the screen.
- Compose cycle: D[2] configuration -> B[9] rotation -> B[12] size -> B[20] per layer -> B[21]
  (Commit completes the status; a commit that never completes hangs gce's teardown).
- The backend registers itself with the surface update server: `StartSurfaceUpdateServer`
  (surfaceupdateserver.dll ord1, -11 = already running is fine), then provider vtable slot 0
  `Register(screen, CBase* backend, priority = 10 - screen)`; the server finds MCompositionSurfaceUpdate
  through `Extension_(0x10286498)`. ContentUpdated must complete the consumed/displayed statuses.

## EGL and OpenVG (src/khr, H-khr)

- libEGL.dll = the Khronos OpenVG 1.1 reference implementation's MiniEGL + VG/VGU front end (from the
  pinned oss.FCL.sf.os.graphics, MIT-style Khronos licence, compiled unmodified with a no-exceptions
  shim) + `eglos.cpp` (OS layer) + `hostraster.cpp` (the RI's Rasterizer interface). EGL at the SDK's
  libegl13u ordinals 2..35; VG and VGU as `khr_<name>` at 36..131 (libEGL has 249 slots).
- libOpenVG.dll / libOpenVGU.dll: the SDK's libopenvg11u / libopenvgu11u ordinals, each a branch to
  libEGL's `khr_` export (as Broadcom's forwarded to khmangle.dll).
- Windows (EGLNativeWindowType = the client's RWindow): an ARGB_8888_PRE surface-manager surface of
  the window's size, `RWindowBase::SetBackgroundSurface`, and per swap the drawable is copied in and
  `RSurfaceUpdateSession::SubmitUpdate(KAllScreens, ...)` sent (no SDK import library: one is made
  from SF's surfaceupdateclientU.def). Pixmaps are CFbsBitmaps.
- Seen working (2026-11-07): alf's server thread in EwSrv makes its 360x640 window surface, gce adds
  the layer, every eglSwapBuffers reaches gcebe's ContentUpdated. Drawing: vg-host.md.

## Host composition and presentation (2026-11-07)

Pixels do not cross the guest any more: no read-back at swap, no copy into the GCE surface, no copy
into the frame buffer by the guest CPU.

- Swap: `OSBlitToWindow` sends SYGL_VG_PRESENT (host surface, TSurfaceId): the host copies the
  drawable's resolved pixels into the GCE surface's scan-out texture. Then SubmitUpdate as before.
- Commit (and ContentUpdated of a surface in the scene): gcebe sends SYGL_COMPOSE with the scene's
  layers (TSurfaceId, destination and source rectangles, layer alpha, pixel format, the mapped buffer's
  address, stride, size, dirty). The host draws them bottom to top into a screen FBO (premultiplied
  blending, scaling with linear filtering when the rectangles differ), takes a layer's pixels from its
  scan-out texture when it has one, else reads them from guest memory (again only when ContentUpdated
  marked the surface, or always for surfaces never announced), and writes the finished screen into the
  display memory. The phone window and screendump read that as before.
- So far the scene is one layer: alf's window surface (everything on screen is drawn by alf through
  OpenVG; wserv's own UI goes to alf through its render stage).
- Transport (both gcebe and khr): a SyglKick descriptor in the process's own memory names the command
  buffer and the output area by virtual address; the device reads and writes them in place through the
  vCPU's page tables while the kick runs. The kick is one store to the syborg-gl register page, which
  DisplayDriver maps user-writable (control 0x7f13): no kernel call, no copy. Fallbacks: control 0x7f12
  (the kernel writes the kick), control 0x7f11 (the old copy into contiguous RAM) when the device cannot
  reach a page (KErrBadDescriptor).
- Without the host GPU gcebe copies layers itself as before (no blending, no scaling).
