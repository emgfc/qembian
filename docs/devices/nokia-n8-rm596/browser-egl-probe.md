# Browser (BrowserNG) EGL probe on the N8 (Delight 6.7): why no context is made

Static reading of `BrowserEngine.dll` (ROFS1, uid3 0x101f8712, ARM code) and `goommonitor.dll`
(Thumb), 2026-11-11. Addresses are link addresses (`e32load.py` ELF, base 0x8000; runtime = code base
+ (addr - 0x8000)). Tags: [verified] = seen in the disassembly, [inferred] = best guess.

## Who probes

Not Qt. `browserng.exe` imports `browserengine` (the S60 WebKit engine). `BrowserEngine.dll`
imports libEGL ords 2,4,6,7,8,10,11,12,15,16,17,18,21,22,26,30,32 and about 60 libOpenVG ords
[verified]. QtGui also imports eglChooseConfig, but its calls do not match the trace: they pass
configs=NULL first, or EGL_CONFIG_ID [verified].

The probe is a singleton, the engine's "VG context manager" (0x38 bytes):

- `0x18b074` `Instance()`: makes the object once (static slot `*0x18b708 + 4`) and calls Init
  [verified]. About 60 callers.
- `0x18aea8` `Init()` [verified]:
  ```
  this->dpy = eglGetDisplay(0);         if (!dpy) { eglGetError(); return; }
  eglInitialize(dpy, 0, 0); eglBindAPI(EGL_OPENVG_API);
  bpp = TDisplayModeUtils::NumDisplayModeBitsPerPixel(CCoeEnv::Static()->ScreenDevice()->DisplayMode());
  winAttr = {BUFFER_SIZE bpp, SURFACE_TYPE 0x44, RENDERABLE_TYPE OPENVG, ALPHA_MASK_SIZE 0, NONE}
  pixAttr = {BUFFER_SIZE bpp, SURFACE_TYPE 0x02, RENDERABLE_TYPE OPENVG, ALPHA_MASK_SIZE 8, NONE}
  if (!eglChooseConfig(dpy, winAttr, &this->winCfg /*+4*/, 1, &n) || !n) { eglGetError(); return; }
  if (!eglChooseConfig(dpy, pixAttr, &this->pixCfg /*+8*/, 1, &n) || !n) { eglGetError(); return; }
  if (this->goom /*+28*/ .Connect()) return;          // goommonitor ord10
  this->goom.<ord28 = IPC op 10, TPckg<TInt>>(&v);  this->gpuMem /*+48*/ = v > 0 ? v : 0;
  ```
  The window config is requested first and the pixmap config second (our trace lists them the
  other way round). The only checks are ret != 0 and num_config != 0 [verified]. Config IDs are
  not compared, the two configs need not be the same, and no eglGetConfigAttrib follows. Our
  answers pass. eglGetError is called only on failure, and the trace has none, which agrees.
- `0x18d334` and `0x18db28`, the two "make the window context" paths [verified]:
  ```
  for (i = 0; (err = goom.RequestFreeMemory(0x600000 /*6 MB*/)) != 0 && i < 20; i++)   // goommonitor ord4
      User::After(50000);
  if (err) return;                       // no context, no surface; this->vgWin (+20) stays 0
  this->vgWin = CreateWindowContext(win, ...);   // 0x18c9dc
  ```
  `0x18c9dc` does `eglCreateContext(dpy, winCfg, 0, 0)`, then
  `eglCreateWindowSurface(dpy, winCfg, RWindow, attrs@*0x18cd54)` and expects
  eglGetError() == EGL_SUCCESS [verified]. Images go through
  `eglCreatePbufferFromClientBuffer(dpy, buftype@*0x18cd50, VGImage, pixCfg, 0)` at 0x18c9cc
  [verified]. The buffer type is most likely EGL_OPENVG_IMAGE [inferred].
- The fallback in the caller at 0x1abcc [verified]:
  ```
  if (... && (flags & 0x400000) && Instance()->vgWin == 0) {
      view->vfunc16(0);                  // falls back to the non-VG drawing path [inferred]
      Instance()->Release();             // 0x18dc40: eglMakeCurrent(dpy,0,0,0); eglTerminate(dpy)
      DeleteInstance();                  // 0x18d2c4
  }
  ```
  On the next paint, Instance() makes the object again. That repeats the whole sequence, and
  explains the 13 rounds [inferred]. Each round waits about 1 s (20 x 50 ms) [inferred from the code].
  The vgCreateImage / vgClear calls with no context come from other code that uses VG without
  checking for a context [inferred].

## Where the decision is made

The browser gives up because the **GOOM** (graphics out-of-memory monitor) does not grant 6 MB.
EGL itself does not fail.
- `RGOomMonitorSession` (goommonitor.dll): ord10 = Connect() (CreateSession "GOomMonitorServer").
  It always returns 0 and sets a "connected" flag only on success. ord4 = RequestFreeMemory(TInt).
  If not connected it returns -34 (KErrCouldNotConnect), else SendReceive(op 0, (bytes, 0)).
  ord28 = SendReceive(op 10, TPckg<TInt>) [verified].
- The GOOM server runs as the thread "GOOM FW" in EwSrv.exe. The old logs show
  `GOOM: CreateGoomWatcherThread created 0` [verified in serial logs]. goommonitor.dll measures
  GPU memory through **EGL_NOK_resource_profiling** (`eglQueryProfilingDataNOK`; it also looks up
  eglCreateSyncKHR, eglDestroySyncKHR and eglClientWaitSyncKHR) via eglQueryString(EXTENSIONS) and
  eglGetProcAddress [verified: imports ord25/20 and the strings]. Our forwarder libEGL and host EGL do
  not offer that extension (nothing in src/khr) [verified]. So GOOM cannot see free GPU memory, and
  RequestFreeMemory(6 MB) most likely returns an error (KErrNoMemory or not-supported) [inferred].
  It might also block the request while it tries to free memory.

## What to check, then fix

1. Check (main session, one probe): `sydb strace GOomMonitorServer` while the browser opens a page.
   Look at the op-0 requests with arg 0x600000 and their completion codes. Also look at the host EGL
   trace of the EwSrv "GOOM FW" thread for eglQueryString(EGL_EXTENSIONS) and
   eglGetProcAddress("eglQueryProfilingDataNOK").
2. Fix (host side, the whole extension, per "full API on the host"): put
   `EGL_NOK_resource_profiling` in EGL_EXTENSIONS and implement
   `EGLBoolean eglQueryProfilingDataNOK(EGLDisplay, EGLint attribs, EGLint *data, EGLint size, EGLint *count)`
   (data == NULL -> count only). Report EGL_PROF_TOTAL_MEMORY_NOK, EGL_PROF_USED_MEMORY_NOK, and per
   process EGL_PROF_PROCESS_ID_NOK + EGL_PROF_PROCESS_USED_PRIVATE_MEMORY_NOK /
   _SHARED_MEMORY_NOK. Take the token values from the Khronos/Nokia header in the SDK, not from
   memory. Total = the N8's GPU budget (BCM2727: 32 MB is a sane total [inferred]). Used = the host's
   tally of our surfaces and images. The guest libEGL only forwards eglGetProcAddress to a stub.
   GOOM then sees more than 6 MB free and grants the request.
3. Cheaper but a hack: a GOOM stand-in, or a `code-patches.txt` entry that makes
   RequestFreeMemory return 0. Only if 2 is too big. Note it in docs/hacks.md.
4. Our eglChooseConfig answers need no change. The +48 value (ord28, op 10) is compared with 32 MB
   at 0x18b250 to choose bigger tile caches (0x400000/0x100000) [verified compare, inferred meaning].

## Outcome (2026-11-11)

- N8 Delight: fixed. GOOM (the graphics memory monitor) needs EGL_NOK_resource_profiling (added, host side,
  e5f0b11); with the GPU memory counted as the phone does (no host MSAA buffers) and 64 MB declared, alf stays
  on its OpenVG path and keeps the browser's surface hole (b808191). With 32 MB or MSAA counted, alf fell back
  to software rendering (a CPU buffer uploaded with vgImageSubData each frame) and painted the home screen
  over the browser's layer.
- 808 (Belle FP2): BrowserEngine.dll (7c900000, its own build) looks up EGL_NOK_image_endpoint(2) and
  EGLImage functions through eglGetProcAddress in a singleton (`0x7c90b470`: eglCreateEndpointNOK,
  eglDestroyEndpointNOK, eglGetEndpointAttribNOK, eglSetEndpointAttribNOK, eglAcquireImageNOK,
  eglGetEndpointDirtyAreaNOK, eglEndpointRequestNotificationNOK, eglEndpointCancelNotificationNOK,
  eglDestroyImageKHR, vgCreateEGLImageTargetKHR); +4 = all found. At `0x7c9015ee` a set flag (+0x48 bit 31)
  and a missing set call User::Invariant: the panic USER 0 at start. [verified, static + sydb catch]
