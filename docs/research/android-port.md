# Roadmap: the Nokia 808 (nokia-808-rm807) in QEMU on Android (plan, 2026-11-11)

Target: Samsung Galaxy S23 Ultra (Snapdragon 8 Gen 2: Cortex-X3 + Adreno 740, 12 GB), One UI 9 / Android
17-ish, not rooted, sideloaded APK. Owner's requirements: roughly full screen, touch + multitouch, the
Symbian notification curtain (swipe down) works and Android's shade does not open, a floating
semi-transparent kill/exit button, extendable to the N8, real-time rendering (no VNC, no frame copies),
no over-engineering. Tags: [V] checked in our tree or QEMU 11.1.3 sources / primary docs; [I] inferred.

## Feasibility in one paragraph

Feasible without root: QEMU is TCG-only on Android (no KVM for apps), and TCG arm-on-aarch64 is a mature
backend; Limbo and Termux's `qemu-system-*` packages run exactly this on unrooted phones [I: public
projects, not checked in source]. Our board adds three host dependencies (EGL/GLES 3 through epoxy,
FFmpeg's libavcodec/libswresample/libavutil for the audio device, libsyvg.a C++) [V
scripts/build-qemu-modern.sh], all buildable with the NDK. The one real port is a present path in
`syborg_gl.c`: the composed screen already lives in a host FBO on the GPU [V frame_store/compose_async],
so on Android it is blitted into an EGL window surface on the Activity's SurfaceView and swapped; the
`hframe` CPU read-back and `syborg-fb`'s console copy are simply turned off. Input is a JNI call into
the existing `syborg-pointer`/`syborg-keyboard` devices. The N8 follows by shipping another bundle
(the device model already makes every phone-specific thing data: `tools/device.py qemu-args`).

## Architecture

```
APK (arm64-v8a, minSdk 30)
  Kotlin Activity: fullscreen SurfaceView (eats every touch), immersive sticky, floating button (menu: Menu key,
    Lock, Power, Exit), bundle picker (one bundle = one device), accelerometer -> syborg-sensor "accel"
  JNI (C, ~300 lines): starts libqemu-syborg.so on its own thread with argv from the bundle's qemu-args.txt;
    syborg_android_set_window(ANativeWindow*), syborg_android_touch(id,x,y,action), _key(qcode,down), _accel(x,y,z), _quit()
  libqemu-syborg.so = QEMU 11.1.3 arm-softmmu + our board, linked as a shared library (exported entry, no main())
    syborg-gl (thread=on, present=window): compose -> screen FBO -> glBlitFramebuffer -> eglSwapBuffers (vsync)
    syborg-fb headless (vsync timer only), syborg-pointer (+ multitouch), syborg-keyboard, modem, nvmem (drives), audio (none at first)
  files: <externalFilesDir>/bundles/nokia-808-rm807/{hybrid.img, hybrid.img.rofs, gen/*, keymap.qemu, dtb, qemu-args.txt, bundle.json}
         <filesDir>/drives/nokia-808-rm807/{nvmemory.img, nvmemory2.img}   serial.log, qmp (abstract unix socket for adb)
```

## 1. QEMU in an unrooted APK

| Topic | Finding | Tag |
|---|---|---|
| Accelerator | TCG only; `-accel tcg` is what we use today (`config SYBORG ... depends on TCG`). KVM needs /dev/kvm (root/kernel). | V |
| JIT buffer | QEMU's default on Linux is `split-wx = 0`: one anonymous `mmap(PROT_READ\|WRITE\|EXEC)` (tcg/region.c `alloc_code_gen_buffer_anon`, tcg-all.c default 0 unless CONFIG_DEBUG_TCG). Android app processes may create anonymous RWX mappings (`execmem`): ART/JS JITs and the Termux/Limbo QEMUs rely on it. Fallback if denied: `-accel tcg,split-wx=on` (memfd mapped twice: RW and RX, `alloc_code_gen_buffer_splitwx_memfd`); `memfd_create` is a public bionic symbol from API 30. Last resort: `--enable-tcg-interpreter` (TCI, several times slower). | V (QEMU code), I (SELinux policy) |
| Code buffer size | Default `tb-size` on a 64-bit host reserves up to 1 GB lazily; pass `-accel tcg,tb-size=256` so RSS stays bounded (docs/perf.md mentions tb-size tuning). | V/I |
| Files | App-private storage only: ROM/ROFS/gen files under `getExternalFilesDir()` (adb push works there without permissions), writable drive images under `filesDir`. No `/tmp`: set `TMPDIR=cacheDir` before `qemu_init` (`qemu_memfd_alloc` falls back to `g_get_tmp_dir()` when memfd fails). No `/dev/shm`: the daemon's shared-RAM file (`ram_path`) is a sydb-phone feature we do not need on Android. | V (QEMU code), I (adb push to Android/data) |
| Shared memory | Not needed: everything is in one process. The `frame-file` mmap (sydb phone) stays unused. | V |
| Threads | QEMU: main loop, one vCPU, RCU, the GL worker (`syborg-gl.thread=on`), timers. The Java main thread must not run QEMU: JNI starts a pthread that calls the exported entry (QEMU's own `main()` runs `qemu_init`, unlocks BQL/replay mutex, then `qemu_default_main` = `qemu_main_loop`; our entry does the same 10 lines). `exit()` at shutdown ends the app process: acceptable for Exit (phase 1); a `:qemu` process is the later refinement if in-app restart matters. | V (system/main.c) |
| 16 KB pages | Guest page size (4 KB ARM) is independent of the host's in softmmu; all QEMU host mappings go through `qemu_real_host_page_size()`. Build every .so with `-Wl,-z,max-page-size=16384` (NDK r28 default) so it also loads on 16 KB kernels; check the S23 Ultra with `adb shell getconf PAGE_SIZE` first (expected 4096). | I |
| Entry as a library | The emulator is built by meson `executable()` (meson.build ~4450). Add a meson option that also builds `shared_library('qemu-syborg', same sources minus system/main.c + android glue)`; this is what Limbo does with its `libqemu-system-*.so` [I]. | V/I |

## 2. Build (NDK cross-compile, arm64-v8a)

Toolchain: NDK r28 LTS (through Android Studio's SDK Manager), `aarch64-linux-android30-clang`; meson/ninja/pkg-config on the PC (already there for the host build). QEMU's configure makes its own meson cross file from `--cc/--cxx/--ar/--strip/--ranlib/--host-cc`; a `PKG_CONFIG_LIBDIR` pointing at an Android sysroot prefix (`build/android/prefix`) supplies the deps.

| Dependency | Today (desktop) | On Android | Tag |
|---|---|---|---|
| glib 2.x | host package | meson cross build (needs libffi, pcre2, zlib: NDK has zlib; iconv is in bionic since API 28). The one dependency most likely to need a patch or two. | I |
| pixman | host package | meson cross build, trivial. | I |
| libfdt | QEMU bundled | `--enable-fdt=internal` (our `-dtb syborg-device.dtb`). | V (dtb in `device.py paths`) |
| libepoxy | `dependency('epoxy')`, surfaceless MESA platform | Drop it: link `-lEGL -lGLESv3` from the NDK and add a 30-line `#ifdef __ANDROID__` shim for `epoxy_has_egl_extension` (strstr on `eglQueryString(dpy, EGL_EXTENSIONS)`); headers `<EGL/egl.h> <EGL/eglext.h> <GLES3/gl32.h> <GLES2/gl2ext.h>`. Only two extensions are queried at run time: `GL_EXT_blend_minmax`, `GL_OES_packed_depth_stencil` (both core in ES 3) [V syborg_gl.c:326]. Keeping epoxy is possible (it builds for Android) but is one more cross build. | V/I |
| FFmpeg libavcodec, libavutil, libswresample | host packages (`syborg_audio.c`: MP3/AAC decode + resample) | ffmpeg's own configure: `--enable-cross-compile --target-os=android --arch=aarch64 --disable-everything --enable-decoder=mp3,aac --enable-swresample --disable-avformat --disable-avfilter --disable-avdevice --disable-swscale --disable-programs --enable-pic`. Or phase 1 only: stub `syborg_audio_dec_*` behind `#ifdef` (no FFmpeg at all) and add it back with audio. | V (includes), I (flags) |
| audiodev | pipewire/wav (`daemon.audiodev`) | `-audiodev none,id=snd0 -machine audiodev=snd0` first (the `none` backend exists). Real sound later: a small `audio/aaudio.c` backend (AAudio from the NDK, ~250 lines, modelled on QEMU's smallest backend). | V/I |
| libsyvg.a (C++ RI) | `g++ -m64 -std=gnu++11 -fno-exceptions` (scripts/build-syvg.sh) | Same flags with the NDK `clang++`, drop `-m64`; `VGHandle = uintptr_t` is 64-bit on arm64-v8a as on the PC (guest 32-bit handles are mapped by the device). `-lstdc++` becomes `-lc++_static` (or `c++_shared`). | V |
| Board code Linux-isms | `<sys/mman.h>`, `getenv`, `open/mmap` of frame-file, chardev `file:` serial | All present in bionic; no `/dev/shm`, no `/tmp` (see above). | V |

QEMU configure (proposal): `--target-list=arm-softmmu --disable-docs --disable-tools --disable-guest-agent --disable-werror --disable-sdl --disable-gtk --disable-vnc --disable-spice --disable-curses --disable-opengl --disable-virglrenderer --disable-kvm --disable-xen --disable-linux-aio --disable-linux-io-uring --disable-libusb --disable-usb-redir --disable-curl --disable-libssh --disable-gnutls --disable-nettle --disable-gcrypt --disable-libudev --disable-seccomp --disable-slirp --disable-vhost-user --disable-vhost-net --disable-cap-ng --disable-attr --disable-numa --disable-pipewire --disable-alsa --disable-pa --disable-oss --disable-jack --disable-sndio --disable-plugins --disable-selinux --disable-libdw --enable-fdt=internal --enable-lto` (LTO as today; drop it if the NDK link misbehaves). Expect a handful of bionic fixes in QEMU's `util/` (the Termux package's patch list is a checklist of *which files*, not code to copy: GPL rule).
Script: `scripts/build-android.sh` (deps into `build/android/prefix`, then QEMU into `build/android/qemu`, output `build/android/lib/arm64-v8a/libqemu-syborg.so`), consumed by the Gradle project in `android/` (jniLibs).

## 3. Graphics: the composed frame to the screen with no copy

Today [V]: `gl_init_now` opens a MESA surfaceless EGL display, a GLES 3 (or 2) context with no surface; composition draws every layer into a screen FBO (MSAA renderbuffers, `s->samples = MIN(msaa, GL_MAX_SAMPLES)`), then `glReadPixels` (sync, or PBO + fence + `frame_timer` on the main loop) into `hframe`, which `syborg-fb` memcpy's into the QEMU console and the frame file for the desktop window. With `thread=on` the GL worker owns the context and runs every job in stream order; kicks that read guest memory in place (phase 4) run while the vCPU is parked.

Android changes (`syborg_gl.c`, new property `present = none|frame|window`, default `frame`):

| Step | Change | Tag |
|---|---|---|
| Display/context | `#ifdef __ANDROID__`: `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, config with `EGL_WINDOW_BIT \| EGL_PBUFFER_BIT`, ES 3 context, made current with `EGL_NO_SURFACE` (needs `EGL_KHR_surfaceless_context`; Adreno advertises it [I]); fallback a 1x1 pbuffer. `gl2_cfg` must be a window-capable config so shared ES 2 contexts match. | V/I |
| Window hand-off | `syborg_android_set_window(ANativeWindow*)` from `surfaceCreated/Changed/Destroyed` (JNI, `ANativeWindow_fromSurface`), queued as a GL job: the worker creates/destroys `eglCreateWindowSurface`, `eglSwapInterval(1)`. No window: composes still render the FBO, nothing presented. | I |
| Present | After compose, instead of the read-back: a separate queued job `present` (no guest memory, so the vCPU is not parked) that binds the window surface, `glBlitFramebuffer(screen FBO -> 0)` scaled to fit with letterbox (360x640 -> 1440x2560 on a 1440x3088 panel; bars top/bottom, or user-selectable crop) and `eglSwapBuffers`. The blit also resolves MSAA. One present outstanding at a time: if a present is already queued when the next compose finishes, the new one replaces it (coalescing: the swap blocks on vsync, so composes faster than 60 Hz are dropped, never queued). | I |
| Read-back | `present=window` skips `frame_store`/PBO/`frame_timer` entirely; a screenshot (QMP `screendump` through the fb) switches a single frame through the old path. | V (code paths exist) |
| syborg-fb | New property `headless=on`: keep the vsync timer and IRQ (the N8 line paces display updates on it), skip the console surface copy; with `-display none` no DisplayChangeListener exists, so `gfx_update` is not called anyway [I], but the explicit flag makes it certain and also disables `qemu_console_resize`. | V/I |
| Rotation | Composition already knows the screen rotation (`rot` in `compose`); the Activity is locked to portrait, the picture is drawn as composed. The owner's Turn view becomes the phone's own accelerometer: Android `TYPE_ACCELEROMETER` -> `qom-set /machine/sensor accel` (`object_property_set_str` from a bottom half), so Belle rotates itself as on the real 808. | V (sensor property), I |
| Threads | `syborg-gl.thread=on` is mandatory on Android: `eglSwapBuffers` waits for vsync and must never run on the vCPU thread; the EGL context stays on the worker (never current on JNI/UI threads). | V (worker exists) |
| Adreno specifics | GLES 3.2 on Adreno 740 [I]; `GL_MAX_SAMPLES` is 4 on Adreno (the profile's `msaa = 8` is capped by the existing MIN; set the bundle to 4 to be explicit) [I]; tiled renderer: FBO switches and `glReadPixels` are the expensive operations (the VG path's per-drawable FBOs are fine, the read-back is what we remove); `mediump` is real fp16 on Adreno (the phone's VideoCore was too, so guest shaders behave; our own shaders in `vs_src/fs_src` should say `highp` for positions/texcoords). Required formats `GL_RGBA8`, `GL_DEPTH24_STENCIL8` MSAA renderbuffers: core ES 3. | I |

What does not change: syvg (VG runs on the vCPU, drawing reaches the worker as batches), the kick protocol, gcebe, the H-khr guest libraries, the N8/808 compose path. The desktop keeps `present=frame`.

## 4. Input and Android UI behaviour

Today [V]: `syborg-pointer` takes QEMU input-layer ABS x/y (0..0x7fff) + BTN left into a 64-entry FIFO of `{x, y, z, buttons}` (registers 3..6, LATCH pops); `exyin.cpp` reads it and raises Symbian button-1 down/move/up raw events (single pointer; the 808 profile says "single touch for now"). Keys: `syborg-keyboard` maps qcodes -> Symbian scan codes through `keymap.qemu` (808 keys: menu f8, power f4, lock f13, volup f11, voldown f12, camera f14/f15). phone.py sends QMP `input-send-event`.

| Item | Plan | Tag |
|---|---|---|
| Touch path | JNI `syborg_android_touch(id, x, y, action)` from `onTouchEvent` (normalised 0..1 of the letterboxed picture, `requestUnbufferedDispatch` for latency) -> `aio_bh_schedule_oneshot(qemu_get_aio_context(), ...)` (thread-safe) -> `qemu_input_queue_abs/btn` for pointer 0, as the QMP path does today. No QMP round trip. | V (API), I |
| Multitouch | Pointer FIFO entry gets a pointer number: new register 9 (`POINTER_ID`) read after LATCH, written from QEMU's multi-touch input events (`INPUT_EVENT_KIND_MTT`: slot, tracking id, begin/update/end, from 8.1 on) so phone.py/sydb can also script two fingers later. `exyin.cpp`: `TRawEvent::SetPointerNumber(id)` per event and the digitiser HAL attributes the window server asks for multi-pointer support (`EPointer3DMaxPointers`, `EPointerNumberOfPointers` [I names]); the 808 is capacitive multitouch so the UI expects it. Android `ACTION_POINTER_DOWN/UP` + `getPointerId` map 1:1. ~150 lines across QEMU + exyin. | I |
| Keys | Volume up/down -> `volup`/`voldown` qcodes (eaten from Android: `onKeyDown` returns true). Back (button or gesture): consumed by an `OnBackPressedCallback`, no Symbian action (optionally the Menu key). Power button: cannot be intercepted; the Symbian Lock/Power keys live in the floating menu. | V (keys), I (Android) |
| Fullscreen | `WindowCompat.setDecorFitsSystemWindows(false)`, `WindowInsetsControllerCompat.hide(systemBars())`, `systemBarsBehavior = BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE` (sticky immersive), cutout mode `ALWAYS`, orientation locked portrait, `keepScreenOn`. | I |
| Top-edge swipe (Symbian curtain) | In sticky immersive the system shows translucent transient bars on an edge swipe but the app still receives the touch, so the Symbian curtain opens at the same time; Android's shade only opens on a *second* swipe while the transient bar is visible (a few seconds). No app can block the status-bar swipe outright [I: Android docs]. Fallback that does block it: lock task mode (`startLockTaskMode()`, Android's "pin windows"; the user enables pinning once in One UI settings and confirms a dialog): status bar and navigation gestures are disabled until the unpin gesture. Offer it as a toggle in the floating menu. | I |
| Bottom/side edges | `setSystemGestureExclusionRects` covers only the back-gesture (left/right) edges and is capped at 200 dp per edge; the home swipe cannot be excluded. In immersive mode the first bottom swipe only reveals the transient nav bar, which Belle does not mind (its bottom toolbar is tapped, not swiped). | I (docs) |
| Floating button | A 44 dp semi-transparent draggable `ImageButton` in the Activity's FrameLayout above the SurfaceView (no overlay permission): tap -> popup menu {Menu key, Lock, Power (long), Pin screen, Exit}; Exit -> JNI `syborg_android_quit()` -> `qemu_system_shutdown_request()` (drives flushed by the block layer at main-loop exit) -> `finishAndRemoveTask()`; a watchdog kills the process after 3 s. | I |

## 5. Memory and performance

| Topic | Numbers / plan | Tag |
|---|---|---|
| Guest RAM | `ram_mb = 548` (512 MB phone + unpaged core image) + flash window 256 MB (RAM-backed `memory_region_init_rom` + memcpy of the 187 MB `hybrid.img.rofs`) + TCG cache (bound to 256 MB) + GL/VG state ≈ 1.2-1.5 GB RSS. Fine on 12 GB; a foreground Activity is not the LMK's first target. Later saving: map the flash window from the file (`MAP_PRIVATE`) instead of copying. | V (sizes), I (RSS) |
| Bundle size | hybrid.img 37 MB + rofs 187 MB + gen files; drives are created on the device (nvmemory 64 MB, memory card: ship a 1 GB sparse card instead of the profile's 8 GB sparse; the phone formats it at first boot). | V |
| TCG speed | The desktop reference is a Ryzen 9700X (docs/perf.md: N8 start-up 30 s). A Cortex-X3 at 3.36 GHz has roughly half its single-thread throughput [I], so expect boot 2-3x slower (808 first boot with wizard: 2-4 min; later boots 1-2 min) and app starts scaled alike. Guest idle is `SyborgWFIIdle` (WFI halts the vCPU in TCG), so an idle phone costs little CPU. | V (idle loop, desktop figure), I (ratio) |
| Our host-side choices | GL worker thread: required (vsync pacing off the vCPU). Phases 3/4 (fewer kicks, in-place reads, no per-call malloc) matter more on the phone because every kick is a trap out of the JIT on a slower core; composition on the GPU costs about 1 ms/frame on the desktop GPU, similar on Adreno [I]. `present=window` removes the last CPU copy per frame (hframe memcpy + fb memcpy + PBO read-back). | V/I |
| Thermal/battery | One big core at 100% during boot and heavy UI, then mostly idle; sustained use throttles the X3 after minutes (TCG then slows ~20-30%) [I]. `PerformanceHintManager` is optional polish. Measure before tuning (owner's rule). | I |
| Measure first | (1) boot time to `ui_phase = 104` in serial.log (desktop vs phone), (2) `SYBORG_GL_STATS=1` kicks/frame and ms/frame in logcat, (3) `top -H` of the app: vCPU vs GL worker vs main loop, (4) idle CPU % at the home screen (WFI working?), (5) battery per 10 min at the home screen, (6) home-screen swipe smoothness by eye and swap count per second. | V (tools exist) |

## 6. App shape and files

- `android/` Gradle project (Kotlin, one Activity, `minSdk 30`, `targetSdk` current, `android:extractNativeLibs=false` is fine for JNI libs). No permissions needed (own storage). `jniLibs/arm64-v8a/`: `libqemu-syborg.so`, `libc++_shared.so` (or static), `libsyborg-jni.so`.
- JNI glue `android/app/src/main/cpp/syborg_jni.c`: `start(argv[])` on a pthread (sets `TMPDIR`, `HOME`), `setWindow(Surface)`, `touch`, `key`, `accel`, `quit`; device entry points declared in a new `src/qemu-syborg/hw/arm/syborg_android.h` (weak on the desktop build).
- Bundle = one device, generated on the PC: new `tools/device.py android-bundle ID --out DIR` (~80 lines; reads `qemu_args(prof, rom)` and `paths`, copies hybrid.img, hybrid.img.rofs, gen/*, keymap.qemu, the dtb, writes `qemu-args.txt` with paths relative to the bundle, and `bundle.json`: device id, name, screen size, orientation, msaa, drive images to create with sizes, the key list for the floating menu from `keymap.toml`). The app knows no device names: everything phone-specific comes from the bundle (N8: `android-bundle nokia-n8-rm596`, done). The app appends only the host-side arguments: `-display none -serial file:... -drive if=mtd,...` x2, `-global syborg-gl.thread=on -global syborg-gl.present=window -global syborg-fb.headless=on -accel tcg,tb-size=256 -audiodev none,id=snd0 -machine audiodev=snd0 -qmp unix:qembian-qmp,server=on,wait=off,abstract=on`.
- Transfer: `adb push build/android/bundles/nokia-808-rm807 /sdcard/Android/data/<pkg>/files/bundles/` (no permissions, no picker); fallback a SAF folder picker that copies into `filesDir`. First launch creates the drive images (sparse `ftruncate`) and a `drives/<id>/` per device, so bundles can be updated without losing the phone's C:/E:.
- Debugging over USB: `adb forward tcp:4444 localabstract:qembian-qmp` (QMP), `-gdb tcp:127.0.0.1:1234` + `adb forward` (gdb), `adb logcat` (QEMU stderr redirected to logcat via a pipe thread), `serial.log` pulled with `adb pull`. sydb's QMP/serial ops could later point at these without the daemon (not in scope).

## 7. Phases, effort, first milestone

Effort in working days for someone who knows the tree; "d" = day.

| Phase | Work | Effort | Exit |
|---|---|---|---|
| 0 Feasibility (optional, cheap) | Build QEMU + board inside Termux on the S23 (clang, glib, pixman, ffmpeg, meson/ninja are Termux packages; `syborg-gl.backend=off`, `-display none`, serial to a file) and boot the 808 bundle headless. Answers TCG speed and bionic build trouble before any APK work. | 0.5-1 d | ui_phase 104 in serial.log; boot time measured |
| 1 Cross build + headless APK | NDK builds of glib, pixman, (FFmpeg or audio stubbed), libsyvg, QEMU as `libqemu-syborg.so` (meson option + glue entry); `tools/device.py android-bundle`; bare Activity that starts it headless and shows serial.log tail; `adb` QMP/gdb forwarding. | 3-5 d | 808 boots to ui_phase 104 inside the app |
| 2 Display + touch (**first milestone**) | `present=window` path + JNI window hand-off + coalescing on the GL worker; `syborg-fb.headless`; immersive fullscreen with letterbox; single-pointer touch via bh; volume keys; floating button with Exit (and Menu/Lock/Power). | 3-4 d | **808 home screen on the S23, apps open by touch, Symbian curtain opens on swipe, Exit works** |
| 3 Multitouch + gestures + sensor | Pointer number register + MTT events + exyin multi-pointer; pinch in Photos/Maps; lock-task toggle; Android accelerometer -> syborg-sensor; back/gesture polish. | 2-3 d | two-finger pinch zooms; the UI rotates when the phone is turned |
| 4 Audio + performance + N8 | AAudio audiodev (or FFmpeg-decoded audio through it); tb-size/LTO/thermal measurements; snapshot resume for fast starts if boot is too slow; N8 bundle smoke. | 2-4 d | sound; N8 boots from its bundle |

Total: about 2-3 weeks. Phase 0 can run in parallel with phase 1's dependency builds.

## Risks and the cheapest fallback for each

| Risk | Fallback |
|---|---|
| QEMU/glib do not build on bionic as-is (missing POSIX bits, `getpw*`, pty functions) | `--disable-*` more; a short patch set in `patches/qemu-android/`; phase 0 in Termux tells early which files. |
| RWX JIT mapping refused by SELinux | `-accel tcg,split-wx=on` (memfd dual mapping); if exec of memfd is refused too, TCI (slow, but proves the rest). |
| TCG too slow for a usable UI (boot > 5 min, laggy taps) | Measure first; `tb-size`, LTO/PGO (docs/perf.md), guest-side hacks that cut waits; resume from a saved machine state (`-incoming file:state.bin` as sydb snapshots do) so a "boot" is seconds, with host GL state rebuilt lazily [I: GL device migration not checked]. |
| Android's status bar swipe interferes with the Symbian curtain | Sticky immersive gives the app the touch; the translucent transient bar is cosmetic. Hard block: lock task mode (pin windows). |
| Adreno EGL/GLES differences (no surfaceless context, MSAA 4 max, blit/precision quirks) | 1x1 pbuffer at init; `msaa=4`; `filter`/`msaa` are already runtime properties, compare screens with the desktop's screendump. |
| `exit()` at Exit kills the whole app; no in-app restart | Accept (relaunch from the launcher); later `android:process=":qemu"` with the Surface passed over binder. |
| 16 KB page-size kernel on the target or a future phone | `-z max-page-size=16384` on every .so (NDK r28 default); nothing in the guest depends on the host page size. |
| Drive image corruption on a forced kill (process death) | nvmem writes go through QEMU's block layer; Exit does a clean shutdown; keep a copy of a freshly-booted `drives/` for reset. |

## What the owner must install or decide first

1. Android Studio SDK Manager: NDK r28 (LTS), SDK Platform 35, Build-Tools, Platform-Tools (adb); Developer options + USB debugging on the S23 Ultra. PC: meson, ninja, pkg-config (present).
2. Optional for phase 0: Termux (F-Droid/GitHub build) on the phone.
3. Decide: letterbox (sharp, 16:9 bars) vs crop/stretch for the 19.3:9 panel; Back key mapping (nothing / Menu); memory-card size for the bundle (1 GB proposed); audio off in phase 1-3; whether to offer lock-task pinning in the menu.
4. Check once: `adb shell getconf PAGE_SIZE` and `adb shell getprop ro.build.version.sdk`.

## Owner's decisions (2026-11-11)

- No screen pinning: a brief glimpse of Android's status bar on a top-edge swipe is acceptable (sticky
  immersive mode; the app still receives the swipe, so the Symbian curtain opens).
- Host tools present: NDK 28.2.13676358 (r28c), platforms android-35/36, build-tools 35-36.1, platform-tools
  (adb 1.0.41), CMake 3.22.1 in ~/Android/Sdk. Still to check on the phone: `adb devices`, `adb shell getconf
  PAGE_SIZE`.
- Phone: SM-S918B (S23 Ultra), Android 17, page size 4096 (checked over adb); serial R5CW226ADBF.
- Black bars: the picture keeps its aspect ratio (letterbox); a different guest rendering resolution may
  come later.
- Memory card: 1 GB in the bundle. Audio: off (the audiodev stays none until the owner asks).
- Back gesture (left to us): eaten and ignored, so a stray edge swipe never leaves the phone; Exit is the
  floating button's. Belle's own back is on its screen.

## First profile on the phone (2026-11-11)

808 boot, 25 s, S23 Ultra (system `simpleperf record --app org.qembian.phone -e cpu-clock:u`; the NDK r28
simpleperf cannot open perf events on this Android 17 build, and the shell user may not attach by pid), and
the same 25 s of a desktop boot (`perf record -e cycles:u -t <vCPU tid>`):

| | S23 Ultra (Cortex-X3, core 7) | desktop (Ryzen 7 9700X) |
|---|---|---|
| vCPU thread share of the process | 98.5 % (GL thread 0.6 %, main loop 0.7 %) | (vCPU thread only recorded) |
| translated guest code (`unknown` / `[JIT]`) | 25 % | 14.5 % |
| QEMU's own code | 71 % | 83 % |
| `helper_lookup_tb_ptr` | 21 % | 41 % |
| `qht_lookup_custom` + `tb_htable_lookup` + `tb_lookup_cmp` | 8.5 % (+ inlined) | 9.9 % |
| `arm_get_tb_cpu_state` | 3.2 % | 2.3 % |
| TLB refill (`tlb_set_page_full`, `get_phys_addr*`, `arm_cpu_tlb_fill_align`, ...) | ~4 % | ~6 % |
| Android only: dynamic TLS (`tlsdesc_resolver_dynamic`, `get_ptr_rcu_reader`) | ~1.7 % | - |

Conclusion: the phone runs the same workload as the desktop; nothing Android-specific costs much (GL 0.6 %,
TLS ~2 %). It is slower because its core is slower, and both are dominated by the known hotspot
(docs/perf.md finding 5): Symbian's function returns (`pop {pc}`, `bx lr`) cannot be chained and each one
looks its block up (`helper_lookup_tb_ptr`: its self time is mostly cache misses on the jump cache).
Levers, cheapest first: PGO (docs/perf.md parked levers: 10-20 % estimated for the lookup helper; on the
phone it needs an instrumented build trained on the phone), the jump cache geometry (12/14/16 bits, 2-way),
and a return predictor in QEMU's ARM translator (the real fix for finding 5: largest gain, most work).

## Matched measurement (2026-11-11, the review's experiments 1-2)

808, the same ROM and drives past their first boot, time to start-up done (`100058f4/1 = 3`):

| | wall | vCPU CPU | busy |
|---|---|---|---|
| desktop (Ryzen 7 9700X), `tools/boot_bench.py`, median of 3 | 44.7 s | 32.0 s | 72 % |
| S23 Ultra (Cortex-X3), serial log polled over adb | 55.8 s | 55.3 s | 99 % |

On the phone the vCPU runs on core 7 (Cortex-X3) at its full 3.36 GHz, nice -10, all 8 CPUs allowed, run-queue
wait 1.2 ms per 10 s, ~500 voluntary switches/s, no involuntary ones; the GL thread on core 6, the main loop
on core 4; thermal status 0, AP 44.6 °C. So: no starvation, no throttling, no waits. The phone needs 1.73x the
CPU for the same guest work (about the X3/Zen 5 single-thread ratio); its wall time is only 25 % worse because
the desktop spends 28 % of this phase waiting on guest timers, which the phone fills with computation.
Conclusion: CPU-bound per-clock speed; the levers are QEMU's dispatch costs (docs/perf.md).

Jump-cache sweep (N8 seed, start-up done, 5 alternated runs each, counters on): upstream hash 14/7 13.99 s;
page-mixed hash 14/7 14.02 s, 13/5 14.11 s, 14/5 14.32 s, 15/6 14.32 s: no gain. Counters (one boot): 470 M
lookups (94 % from the helper), 88.6 % jump-cache hits; of 54 M misses 80 % empty slots, 18 % conflicts, <1 %
key; ~61 k whole-cache flushes from our DACR domain drop (patch 0005), 11 k from TLB flushes, 630 k page
clears. The empty slots, not the hash, are the lever: a precise domain drop for the jump cache.
