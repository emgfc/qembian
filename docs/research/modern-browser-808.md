# A modern browser for the Nokia 808 in qembian: options and a plan (2026-11-11)

Research brief, no code changed. Marks: [V] verified in this tree (profile, ROFS listing, kernel patch,
syborg-gl protocol, our docs) or in a primary source linked below; [I] inferred (general knowledge of the
engines, Symbian or TCG, not checked here). Budget: 22 tool calls.

## 0. What the 808 and our board give us [V]

- Phone: ARM1176 (ARMv6K), 512 MB RAM (`ram_mb_real`), 640x360, flexible memory model, Belle FP2
  (`models/nokia-808-rm807/model.toml`). Our kernel raised the user stack cap to 1 MB because
  `webprocess.exe` asks for 512 K (`patches/kernelhwsrv/policy/0025-flexible-user-stack-1m.patch`).
- The firmware already ships (ROFS, `tools/rofs.py` listing, stored sizes): QtWebKit.dll 5.4 MB,
  WebkitEngine.dll 5.6 MB (the S60 WebKit), JavascriptCore.dll 950 K, QtCore/QtGui/QtScript/QtDeclarative/
  QtOpenGL (14.5 MB of Qt), browserng.exe 251 K, and the Open C layer: libc.dll 121 K, libpthread, libdl,
  libm, libz, libstdcppv5 (STLport-based Open C++), libglib 294 K, libssl/libcrypto (OpenSSL 0.9.8, TLS
  1.0: `docs/research/tls-modern.md`). So POSIX-style C/C++ code links against a real libc, pthreads and
  sockets on the phone; `RChunk::CreateLocalCode` exists in euser (map files in `build/epocroot`), so a
  guest JIT can get executable memory.
- Graphics: the window server composes GCE surfaces as layers (`SYGL_COMPOSE`, `syborg_gl_proto.h:122`);
  a layer's pixels come from a scan-out image on the host (`SYGL_VG_PRESENT`) or from guest memory;
  the surface manager is our stand-in (`src/standins/surfacemanagerdriver`); the composed frame lives in a
  host FBO and on Android is blitted into the Activity's window with no CPU copy
  (`docs/research/android-port.md` section 3). GLES 2.0, GLES 1.1 and OpenVG are forwarded whole
  (`docs/gpu-boundary-roadmap.md`: guest forwards only, state on the host).
- Networking: no user-side IP path today; cheapest is an Ethernet stand-in into slirp, days of work
  (`docs/research/connectivity-options.md`). Stock TLS is 1.0 (replaceable: tls-modern.md).
- Speed: TCG on the desktop boots the N8 in ~30 s on a Ryzen 9700X (`docs/perf.md`); the owner's estimate of
  a few hundred effective MIPS for the guest is the right order [I]. A phone host is 2-3x slower
  (android-port.md section 5). Guest code that generates code (a JIT) is translated a second time by TCG,
  and every new or patched JIT page invalidates translation blocks [I: QEMU self-modifying-code handling].

## 1. Option A: a real engine inside Symbian (guest side)

| Engine | Fits? | JS/JIT | WebGL/WASM | Effort | Verdict |
|---|---|---|---|---|---|
| Chromium/Blink + V8 | No. Multi-process + sandbox, GN build, C++20, hundreds of MB of binaries, ~0.5-1 GB RAM for one renderer; official 32-bit ARM builds are ARMv7/NEON only [I]. V8's own ARM backend still accepts `--arm-arch=armv6` (ARMv6 + VFPv2) [V: assembler-arm.cc at HEAD, https://chromium.googlesource.com/v8/v8.git/+/HEAD/src/codegen/arm/assembler-arm.cc], but V8 alone is ~27-28 MB jitless [V: https://bellard.org/quickjs/bench.html]. | JIT exists but would be translated twice under TCG | Native WebGL via GLES2 would work in principle | person-years | Not an option |
| WebKit (WebKitGTK/WPE lineage, current) | Barely. Needs C++20, ICU, libxml2, libsoup or cURL, cairo/skia, harfbuzz, freetype, multi-process WebKit2. JSC dropped ARM_TRADITIONAL (ARMv5/v6) LLInt/JIT/DFG in Nov 2018, r238439; ARMv6 gets the CLoop interpreter [V: https://trac.webkit.org/r238439, https://lists.webkit.org/archives/list/webkit-dev@lists.webkit.org/thread/OGSKJVLU3WR7HMUNZEVJT6KTQOJIDKKH, https://bugs.webkit.org/show_bug.cgi?id=278559; Buildroot does the same https://lists.buildroot.org/pipermail/buildroot/2020-October/295060.html]. | CLoop only (interpreter), ~5x+ slower than JIT [I] | WebGL through our GLES2 forwarding is plausible [I]; WASM interpreter-only | 6-12 person-months | Too big |
| WebKit of 2012-2013 (last with ARM_TRADITIONAL JIT and a Qt port; QtWebKit 2.3/5.x) | Maybe: Qt 4.8 is on the phone [V], GCC 14 symbianelf compiles C++11. Still a 2013 web engine: partial ES5/no ES2015+, no modern CSS, no WASM. | ARM traditional JIT ran on ARMv5/v6 then [V: 2017 thread "Bring back ARMv6 support to JSC" https://lists.webkit.org/pipermail/webkit-dev/2017-September/029571.html] | WebGL via QtOpenGL/GLES2 [I] | 3-6 person-months | Not modern |
| Ladybird/LibWeb | No: C++23, 64-bit-oriented, multi-process, its own JIT was removed in 2024 and the interpreter is now assembly (AsmInt) per host arch [V: https://ladybird.org/newsletter/2026-02-28/]. No Symbian/ARMv6 target. | interpreter | - | - | Not an option |
| Servo | No Rust target for Symbian; 64-bit focus [I]. | - | - | - | Not an option |
| NetSurf + Duktape | Yes. C, small, framebuffer frontend (libnsfb), runs on ARMv6 natively (RISC OS, Pi 1) [V: https://en.wikipedia.org/wiki/NetSurf]. HTML 4.01 + CSS 2.1, CSS flex since 3.11, Duktape 2.7 with a thin DOM [V: https://www.osnews.com/story/138159/netsurf-3-11-released/]. | interpreter, little DOM | none | 4-8 person-weeks for a Symbian frontend (Open C + a window) | A real on-phone browser, but not "modern web" |
| litehtml + QuickJS/QuickJS-ng | Renders static HTML/CSS; no DOM bindings for scripts: a widget engine, not a browser. | QuickJS ~620 K, no JIT [V: Bellard's bench page] | none | 2-4 weeks | Only for app-embedded HTML |
| JS engines alone (QuickJS, Duktape, JerryScript, Hermes, V8 jitless, JSC CLoop) | QuickJS/Duktape/JerryScript are C and build on Open C [I]; Hermes is C++17, officially armv7/arm64 [I]; V8 jitless is the whole V8 build. | all interpreters on ARMv6 | - | days to weeks each | Useful only inside a guest engine |
| WASM in the guest (wasm3, WAMR) | wasm3: C interpreter, ~64 K code, minimal maintenance now [V: https://github.com/wasm3/wasm3]; WAMR classic interpreter targets ARM/THUMB generically, ARMv7 tested [V: https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wamr.md]. | interpreter x TCG = 50-300x slower than native [I] | - | 1-2 weeks | Demo value only |

Common guest-side limits [I unless noted]: every page's JS is interpreted ARMv6 under TCG (a modern site's
1-3 s of JIT'd JS on a real 1 GHz ARMv7 becomes tens of seconds); a modern renderer wants 100-300 MB per
tab (one or two tabs in 512 MB); TLS 1.3, HTTP/2, video codecs and fonts all have to be brought along;
the network path is a prerequisite (section 0). A guest JIT would work (code chunks exist) but gains little
under TCG and costs TB invalidations. Conclusion: a *modern* engine in the guest does not fit the CPU
budget, whatever the memory; a *small* engine (NetSurf) fits but is not modern.

## 2. Option B: the engine on the host, a virtual "web device", a thin Symbian shell

The same pattern as our GPU boundary: the guest only forwards, every state (DOM, JS, network, TLS,
cookies, WebGL, WASM) lives on the host, natively compiled and JIT'd, with native TLS 1.3 and the host's
network (no guest IP path needed for the browser at all).

Architecture [proposal]:

```
Symbian shell app (Qt or Avkon, ~2-4k lines): URL field, back/forward, tabs, progress, FEP text input,
  one RWindow with a background GCE surface (RWindow::SetBackgroundSurface) per tab = the page area
   | ctl calls on a `syborg-web` channel (new stand-in LDD or an extension of d_display's ctl space)
   |   OPEN(tab, surface id, w, h, dpi)  NAVIGATE(url)  BACK/FWD/RELOAD/STOP  INPUT(touch id,x,y,action | key | IME text)
   |   SCROLL/ZOOM (or leave to the page)  POLL/EVENT(url, title, progress, can-go-back, favicon, open-new-tab)
   |   DOWNLOAD(stream bytes into a guest file)  CLOSE
QEMU `syborg-web` device: owns one host web view per tab; registers the tab's scan-out image for the
  GCE surface id, so `SYGL_COMPOSE` takes the page from a host texture (as `SYGL_VG_PRESENT` images do today);
  wserv still composes chrome, dialogs, the keyguard and the notification curtain over it unchanged.
Host web view backends:
  desktop: CEF windowless (OSR). OnPaint gives a BGRA buffer per dirty rect (one glTexSubImage2D, like a
    dirty guest-memory layer today); OnAcceleratedPaint gives a shared texture / DMA-BUF on Linux for
    zero-copy later [V: https://bitbucket.org/chromiumembedded/cef/wiki/GeneralUsage,
    https://docs.rs/crate/cef/latest/source/src/osr_texture_import/mod.rs]. Alternative: WPE WebKit with
    wpebackend-fdo (EGLImage/DMA-BUF export, built for embedding) [I].
  Android: the system WebView (Chromium) in the Activity, drawn into a SurfaceTexture -> GL_TEXTURE_EXTERNAL_OES
    in the GL worker's context: zero-copy, nothing to bundle, Google keeps it current [I: standard Android
    API; must be checked on the S23 for the external-texture import into our EGL context].
```

Why this matches the owner's stances: zero-copy on Android (the page is a GPU texture inside our own
compositor, no VNC/frame copy); "full API on the host, guest = forwarding only"; one device model entry
(`[[standins]] name = "web"`, `[web.host] backend = cef|wpe|android`). The guest spends no CPU on the page.

Costs and risks:
- Not the phone running a browser: it is a modern page inside a Symbian window. Fine for "use the web on
  the 808", wrong if the goal is fidelity.
- Desktop dependency: CEF is ~150-200 MB of binaries and wants its own message loop and helper processes
  [I]; run it in a helper process (`qembian-web`) talking to QEMU over a Unix socket + shared memory or
  DMA-BUF, so a crash never takes the phone down (also the fault containment phase 5 wanted).
- Text input: the Symbian FEP / virtual keyboard delivers key events and committed strings; the host view
  needs an IME contract (commit text, composition, focus in/out of input fields -> show/hide the Symbian
  keyboard). This is the fiddliest part; start with key events only.
- Per-instance state: profile, cookies, cache under `build/instances/<name>/web/`; downloads written
  into the instance's C:/E: image by the device (same mechanism as `sydb fs_push`).
- Making it the default browser (links from Messaging, Store): the shell must answer the browser launcher
  interface Belle apps use (browserng.exe's UID / `BrowserLauncher` API) [I]: phase 3.
- QtWebKit-based Symbian apps keep their old engine; a later step could route `QWebView` through the same
  device by replacing `QtWebKit.dll` with a forwarding shim (heavy, out of scope).

Effort: device + compositor hook 1-2 weeks; CEF OSR backend 1-2 weeks; Symbian shell 2-3 weeks; Android
WebView backend 1-2 weeks; IME/downloads/default-browser polish 2-4 weeks. MVP in ~5 weeks, complete in
~10-12 person-weeks.

## 3. Option C: hybrids

- C1 Guest WebKit (2013-era or current/CLoop) + host WebGL through our GLES2 forwarding: WebGL works for
  free once the engine's GL backend calls GLES2 (our libGLESv2 is the phone's API), but JS stays
  interpreted ARMv6 under TCG. Only worth it together with A's "authentic" track; same 3-12 months.
- C2 Guest DOM/layout + host JS engine: every DOM access would cross the boundary; the DOM-JS call rate
  (10^5-10^6 per page load) makes it slower than interpreting locally. Rejected.
- C3 Host engine (B) + guest "web runtime" for WRT widgets/Qt QML WebView: an extension of B once B exists.
- C4 B for pages + guest NetSurf for offline/"on the phone" fidelity: two browsers, two code bases; only if
  the owner wants both aims.

## 4. Recommendation and phased plan

Take Option B. A modern engine cannot run at acceptable speed on an emulated ARMv6 interpreter path, and
every real engine has dropped or never had ARMv6 JIT support; the host already runs the phone's GPU work
with exactly this split, and on Android the system WebView costs nothing to bundle.

MVP first:

1. **Phase 0, compositor hook (1 week).** `syborg-web` device skeleton: `OPEN(surface id)` binds a host
   texture to a GCE surface id as a scan-out image; a test backend draws a static image. Guest: a 200-line
   Avkon/Qt app with `SetBackgroundSurface` on its window. Exit: the image composes under wserv's chrome
   on the N8 and 808, screenshot through `sydb`. Smoke test `tests/device-smoke.sh` unchanged.
2. **Phase 1, desktop MVP (3-4 weeks).** CEF OSR in a helper process (shared-memory frames, dirty rects ->
   `glTexSubImage2D`), NAVIGATE/BACK/INPUT(touch, keys)/EVENT, the shell app with URL field, back,
   progress; profile dir per instance. Exit: a modern site (ES2022, flexbox/grid), a WebGL demo and a WASM
   demo run at host speed inside the Symbian window; touch scroll and tap work; the phone's UI is unaffected.
3. **Phase 2, Android (2 weeks).** WebView -> SurfaceTexture -> external texture in the GL worker's context
   (zero-copy), touch from the guest app. Exit: the same three demos on the S23 Ultra, no CPU copy per
   frame (`SYBORG_GL_STATS`).
4. **Phase 3, polish (3-4 weeks).** IME contract (composition/commit, keyboard show/hide on focus),
   downloads into the instance's drive image, tabs, default-browser registration, favicons/title in the
   Symbian task list, DMA-BUF zero-copy on Linux (`OnAcceleratedPaint`).
5. **Optional, authenticity track.** NetSurf + Duktape as an on-phone browser (4-8 weeks) if the owner
   wants a browser that *runs on the 808*; measure page time before committing; no JIT work in the guest.

Integration with the device model: one stand-in (`src/standins/web/`, channel `syborg-web`, profile
`[[standins]] name = "web"` with `config = { backend = "cef" }`), one QEMU device (`hw/arm/syborg_web.c`)
sharing `syborg_gl.c`'s scan-out image table, one Symbian SIS (`apps/qembian-browser/`) installed by `sydb
install`, the Android backend in `android/` (Kotlin WebView + a JNI texture hand-off). Nothing phone-specific
leaks into the family files; the N8 and 701 get the browser by adding the stand-in to their profiles.

## 5. Open points to check before phase 1

- `RWindow::SetBackgroundSurface` on Belle FP2 from a third-party app: capabilities, and whether wserv
  needs the surface hint set (`SurfaceConfiguration`) [I]; the N8's alf path proves the compositor side only.
- Whether our GL worker's EGL context on Android can import the WebView's SurfaceTexture (same process,
  `EGL_KHR_image_base` + `GL_OES_EGL_image_external`) [I].
- CEF helper-process frame sharing on Linux without X/Wayland (`--ozone-platform=headless` + OSR) [I].
- FEP/IME event flow from an Avkon editor into a non-editor window (candidates: a hidden editor control
  that forwards committed text) [I].
