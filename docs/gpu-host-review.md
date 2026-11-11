# Host GPU path (syborg-gl, src/khr, gcebe): review, presentation options, configurability

Static review, 2026-11-07, of the N8's OpenVG-on-host path and of how frames reach the owner's
window. Nothing was run. Tags: [verified in source] = read in the files named; [inferred] = best
guess from the code or from general knowledge of QEMU/Mesa, not checked here.

Files: `src/qemu-syborg/hw/arm/syborg_gl.c` (device), `syborg_gl_proto.h` (protocol),
`src/khr/hostraster.cpp` / `hostgl.cpp` / `eglos.cpp` (guest), `src/khr/ri-patches/*.patch`,
`src/gcebe/gcebe.c` (composition backend), `src/qemu-syborg/hw/arm/syborg_fb.c`,
`tools/sydb/phone.py`, `tools/sydb/daemon.py`, `src/standins/d_display/d_display.cpp`.

## Status (kept up to date)

| Item | State |
|---|---|
| 1.1 snapshot/restore | logged (docs/sydb.md, vg-host.md); not built while snapshots are not used on the N8 |
| 1.2 objects of dead processes; fixed tables | fixed: d_display sends SYGL_OWNER_FREE when a process's last channel closes; device tables are hash tables (no linear scans, caps 4096 surfaces / 65536 textures) |
| 1.3 blend equation leak | fixed (vg_restore resets GL_FUNC_ADD) |
| 1.4 capability checks | fixed: EXT_blend_minmax and OES_packed_depth_stencil tested on GLES 2 (fallbacks: SRC_OVER, no stencil = fills skipped, logged); surface and texture sizes checked against GL_MAX_TEXTURE_SIZE; GL errors logged (first 8) |
| 1.5 frame buffer tearing | fixed: `syborg-gl.frame-file` seqlock (sydb sessions: `<session>/frame.seq`); `sydb phone` takes only whole, new frames (and repaints every 250 ms anyway) |
| 1.10 refused read-backs, khrHostClear | fixed: SYGL_READ / SYGL_VG_TEXREAD fail for unknown objects; khrHostFlush returns the result; read-backs and present act on it |
| 2.1 async read-back | done: SYGL_COMPOSE reads into a PBO with a fence and returns; a main-loop timer writes the frame to guest RAM (pages translated at compose) under the frame seqlock; the context is released after each kick. Boot + wizard: device time 576 -> 154 ms, compose 475 -> 13 ms (1 of 199 frames waited). `SYBORG_GL_SYNC_READBACK=1` restores the old path; `SYBORG_GL_STATS=1` prints device timings every 5 s |
| 3 configurability (first part) | done: device at `/machine/gl`; runtime QOM properties `msaa` (surfaces resampled lazily: resolve, new buffers, pixels drawn back), `filter`, `compose` (host/guest: the device refuses PRESENT/COMPOSE, the guest's fallbacks take over), read-only `stats` (JSON); profile `[gl.host]` -> `-global`; `sydb gl` / `sydb gl set ... [--save]` (session `gl.json`). Open: `mipmaps`, `pacing`, `overlay`, `debug`, `scale`, `epoch`, CAPS/CFG registers, MCP tools, a phone.py panel |
| 4.1 guest-side rendering cost | measured, parked: during UI work the guest idles 74% and libEGL is 1.8% of samples (about 7% of the busy time): host tessellation, zero-copy uploads and the rectangle fast path would win little now. Host-side ramp cache and vertex batching: device fill time is under 1% of the guest's time; parked |
| 2.2-2.4 GL scan-out, D-Bus dmabuf window, supersampling | not started (the review's steps 2-4) |

## 1. Correctness and robustness

Ranking: **blocker** = breaks a workflow the project relies on; **should fix** = wrong output or a
leak reachable in normal use; **nit** = latent or cosmetic.

### 1.1 Snapshot/restore loses every host object (blocker)

`syborg_gl.c:19-20` says it; there is no `VMStateDescription` for the device at all
(`syborg_gl_class_init`, :1270-1275, sets no `vmsd`), while `syborg_fb.c:267` has one. [verified in
source]

Scenario: `sydb snap save` on an N8 session, then `sydb fork SNAP`. The restored guest still holds
alf's `Surface*` for its 360x640 window, its image render targets and its uploaded image
generations (`m_hostGen == generation()`), so it never re-sends `SYGL_SURF` or `SYGL_TEX`. On the
host `find_surf()` / `find_tex()` return NULL, every `vg_fill`, `vg_clear`, `vg_present` returns 0
silently (`syborg_gl.c:698-706, 802-805, 1001-1005`), gcebe's `SYGL_COMPOSE` finds no scan-out
image and `scan_upload` reads the window surface's guest memory, which khr never filled
(`eglos.cpp:205-212`: the host path skips the blit). Result: a black phone after every fork, with
no log line. The catch/fork workflow in CLAUDE.md is unusable on the N8.

Fix, two layers:
1. Device: a `vmsd` with `pre_save` / `post_load`. Save the `surf[]`, `tex[]`, `scan[]` tables
   (owner, id/key, w, h, flags, gen) and their pixels (read back in `pre_save`; alf's window is
   900 KB, icons are small; cap the total and drop textures beyond it, they re-upload from the
   guest). Restore recreates FBOs/textures in `post_load` lazily: set `s->ready = 0`, keep the
   tables with `tex = 0` and a saved pixel blob, and materialise each object the first time a
   command names it (GL must run on the vCPU thread with the context current, so do not create GL
   objects in `post_load` itself). This gives a pixel-exact restore and needs no guest change.
   `pre_save` runs on the main loop thread with the vCPU stopped: the context is still "current"
   on the vCPU thread, so either release it at the end of every `run()` (`eglMakeCurrent(NULL)`,
   one extra call per kick) or read the pixels at the end of the last `run()` into a host-side
   cache when a `snapshot-pending` flag is set by `pre_save`... the first is simpler and the cost
   is a few microseconds per kick. [inferred: EGL contexts are current to one thread at a time]
2. Protocol (cheaper, lossy, also covers a device reset): an `epoch` word (new register
   `SYGL_REG_EPOCH`, bumped in `post_load`), which khr reads from the user-mapped register page
   at every flush (`hostgl.cpp:75`); on a change it re-sends `SYGL_SURF` for every live drawable,
   sets every image's `m_hostGen = 0` and re-uploads the `m_hostAhead` images from their stale
   guest copy. Content drawn into host-authoritative images (the NVG icons, vg-host.md "patch
   0005") would come back stale or blank until redrawn. Use this as the fallback when the saved
   blob is missing (older snapshot).

### 1.2 Host objects leak when a guest process dies without eglTerminate (should fix)

`SYGL_OWNER_FREE` is only sent from khr on `eglTerminate` [inferred from the protocol comment,
`syborg_gl_proto.h:50`]; `d_display.cpp` contains no `OWNER_FREE` at all, so the channel's close
(process exit, `sydb proc kill`, a panicking app) frees nothing. [verified in source] The tables
are fixed: `MAX_SURF 256`, `MAX_TEX 4096`, `MAX_SCAN 64` (`syborg_gl.c:42-45`). Scenario: a
Qt/OpenVG app that is killed and relaunched a few hundred times, or any app that leaks VGImages
on exit: "too many textures" (`:493`) and then every image draw is dropped without a log
(`vg_fill` `:798-800` returns 0 when the texture is missing). Fix: `DDisplayChannel::~DDisplayChannel`
(`d_display.cpp:595`) kicks a kernel-side 3-word buffer `{SYGL_OWNER_FREE, 3, pid}` via
`KGlKick` (`:206-212`, the same path as control 0x7f12, kernel addresses translate fine); and
make the tables dynamic (`GHashTable` keyed by `owner << 32 | key`), which also removes the
linear scans (1.9).

### 1.3 Blend equation leaks from the VG path into the GLES 1.1 path (should fix, latent)

`vg_blend()` sets `GL_MIN_EXT` / `GL_MAX_EXT` for DARKEN / LIGHTEN (`syborg_gl.c:668-669`);
`vg_restore()` (`:687-695`) does not reset the equation and `gl_draw()` (`:556-561`) only calls
`glBlendFunc`. [verified in source] A GLES 1.1 draw (sygl path) after a VG darken fill blends
with MIN. Today the two paths do not run on the same device at the same time (N8 has no GLES
blob), so it is latent; one line in `vg_restore` (`glBlendEquation(GL_FUNC_ADD)`) closes it. The
rest of the state hand-over looks right: depth test off in `vg_use`, stencil test and colour mask
restored, polygon offset only affects depth (disabled), cull face is never enabled, texture unit
restored to 0, `GL_UNPACK_ROW_LENGTH` reset after use (`:1034-1036`).

### 1.4 Host capability checks are incomplete (should fix)

`gl_init` (`:151-331`) checks for a surfaceless EGL display and takes a GLES 2 context when 3 is
unavailable, and `samples` is 0 without ES3, so `glRenderbufferStorageMultisample` /
`glBlitFramebuffer` are not reached on ES2. [verified in source] Not checked on an ES2 context:
`GL_DEPTH24_STENCIL8_OES` (`:439`, needs `OES_packed_depth_stencil`), `GL_MIN_EXT`/`GL_MAX_EXT`
(`:668-669`, needs `EXT_blend_minmax`), `glCopyTexImage2D` into `GL_RGBA` from an RGBA8 FBO (fine).
There is no `glGetError` anywhere and no `glGetGraphicsResetStatus`, so a GPU reset or a
`GL_OUT_OF_MEMORY` (e.g. a `SYGL_SURF` with a bogus 2^31 width, `:403`, unbounded) just produces
black frames with no message. Scenario: a laptop without Mesa's surfaceless platform (NVIDIA
proprietary EGL lacks `EGL_MESA_platform_surfaceless` [inferred]) warns and falls back to
software rendering, which is correct; a GPU reset mid-session is silent and permanent.
Fix: feature-test the three extensions when `!es3` and disable the dependent paths (darken/lighten
fall back to SRC_OVER, stencil surfaces fail cleanly -> `SYGL_REG_CAPS` bit says "no VG"), clamp
surface and texture sizes (<= `GL_MAX_TEXTURE_SIZE`), poll `glGetError` once per kick in a debug
property (section 3) and `glGetGraphicsResetStatus` if `KHR_robustness` is present.

### 1.5 The frame buffer is written and read without synchronisation (should fix)

`compose()` writes the finished screen into guest RAM with `cpu_memory_rw_debug` page by page
(`:1127-1135`); `phone.py` `ShmFb.tick` copies the same bytes from `/dev/shm` every 16 ms with no
sequence number (`phone.py:88`). [verified in source] A copy can straddle a compose: a frame
whose top part is new and bottom part old. The write takes well under a millisecond for 900 KB
so the window will tear rarely, but it will, and more at a higher render scale (section 2.4). The
QEMU console path (`syborg_fb_update_display`, `syborg_fb.c:86-164`) runs under the BQL on the
main loop thread while `compose()` runs on the vCPU thread also under the BQL, so screendump is
safe. Fix: a frame sequence word the window can check before and after its copy (a new
`syborg-fb` register `FRAME_SEQ` bumped by the device when `compose()` finishes, or simply the
last word of the fb page row written last), plus "paint only when it changed". Better: see
section 2.

### 1.6 MSAA resolve points (ok, one nit)

Resolve happens in `bind_read()` before every read: `SYGL_READ` (`:637`), `SYGL_VG_SURF2TEX`
(`:925`), `SYGL_VG_PRESENT` (`:1006`), `SYGL_VG_TEXREAD` (`:950`) and when a fill samples a live
render target (`:794-796`). Draws go to the multisampled FBO and mark it dirty (`bind_surf`,
`:505-512`), so a resolve is never stale and never repeated without a draw in between. [verified
in source] Resolving premultiplied RGBA8 samples by averaging is the right operation (linear in
premultiplied space). Note that the single-sampled FBO of an MSAA surface has no stencil, so a
`SYGL_SURF` without `SYGL_SURF_MSAA` on a surface id that already exists at the same size returns
early (`surf_make`, `:383-386`) keeping the old flags: harmless today since ids are addresses.
Nit: `resolve()` binds READ/DRAW framebuffers and uses `GL_NEAREST`, which is the only legal
filter for a multisample resolve; fine.

### 1.7 Premultiplied alpha and VG blend modes (ok, documented approximations)

Surfaces are `GL_RGBA8` holding premultiplied sRGB, the paint colour is converted to
`sRGBA_PRE` in the guest (`hostgl.cpp:121-129`), image uploads premultiply `VG_sARGB_8888`
(`:303-305`), the ramp texture is premultiplied whatever the paint's interpolation space
(`syborg_gl.c:744-751`), the colour transform unpremultiplies, applies, repremultiplies (`:234-238`).
SRC_OVER = `ONE, ONE_MINUS_SRC_ALPHA`, DST_OVER, SRC_IN, DST_IN, SCREEN, ADDITIVE are exact for
premultiplied colours; MULTIPLY misses the `src * (1 - dst.a)` term and DARKEN/LIGHTEN assume
both alphas 1, as vg-host.md states. [verified in source] Blending happens in sRGB (non-linear)
space, which is what the RI does for an sRGB surface, so results match the reference. One real
gap: VG_BLEND_SRC disables blending (`:662`) but the cover quad is drawn where the stencil is set,
so the pixels outside the shape are untouched (correct), while inside the shape MSAA coverage is
lost: with blending off, every covered sample takes the source, which is what VG_BLEND_SRC means.
Fine. [inferred: matches the RI's semantics for SRC]

### 1.8 Image orientation (ok, one protocol gap)

VG drawables are GL-oriented (origin bottom left) and VG images are stored bottom-up by the RI,
so uploads and `SYGL_VG_TEXREAD` need no flip; `compose()` flips `v` for host images (`:1101-1103`)
and not for guest-memory layers (rows top-down); `khrHostRead` and `khrHostSync` copy row `y0+y`
to row `y0+y`. [verified in source] The GLES 1.1 path stores surfaces y-down ("surface row 0 = GL
row 0", `:6-7`). A sygl window presented through `SYGL_VG_PRESENT` would therefore be upside down
in the composition: the present/compose path has no orientation flag. Not reachable today; see
the protocol list in section 4.

### 1.9 Keys, owners and tables

- Keys are guest addresses (`Surface*`, `Image*`, `Image* | 1`), unique per process while the
  object lives. Reuse after free is covered: `Image::~Image` sends `SYGL_TEX_FREE` when
  `m_hostGen` is set (`0002-image-generations.patch:56-60`), `khrHostSurfaceFree` sends
  `SYGL_SURF_FREE` and turns a render target into its image's texture (`hostgl.cpp:176-191`). A
  fresh `Image` starts at `m_gen = 1`, `m_hostGen = 0`, so it uploads on first use. [verified in
  source] Gap: if `khrHostEmit` returns NULL (buffer full and the flush failed) a FREE is lost
  silently (`:179, :187`): leak until `OWNER_FREE`. Nit.
- `find_surf` / `find_tex` are linear over 256 / 4096 entries (`syborg_gl.c:333-341, 467-475`).
  An image fill does up to two full scans (`:792-793`); a frame with a few hundred image fills
  costs a few million compares on the host. Not a bottleneck next to the guest CPU, but free to fix
  with the hash table from 1.2. Should fix together with 1.2.
- The owner comes in-band (`SYGL_OWNER` is the first command of every khr batch,
  `hostgl.cpp:110-113`); gcebe's batches carry none and run under whatever owner was last set
  (`gcebe.c:237, 289-291`), which is harmless because `SYGL_COMPOSE` / `SCANOUT_FREE` use the
  global scan table. A buffer that forgets the owner would act on another process's objects. The
  kernel knows the process (`d_display.cpp:201`); put the owner in the `SyglKick` (section 4).
- Any user process with the register page mapped can kick with any address;
  `cpu_memory_rw_debug` ignores page protection, so a process could read or overwrite kernel
  memory through the OUT area. It is an emulator and the owner's firmware is trusted: nit, but
  write it down in `d_display.cpp`'s comment.

### 1.10 Error handling on the guest side

- `khrHostSync` copies `gOut` into the image whether or not the kick succeeded
  (`hostgl.cpp:363-371`); `khrHostFlush` has no return value. After a `SYGL_VG_TEXREAD` that was
  refused (unknown key: `vg_texread` returns 0 and writes nothing, `syborg_gl.c:937-939`) the image
  gets the previous read's bytes. Should fix: return the result from `khrHostFlush` and skip the
  copy.
- A `SYGL_VG_FILL` larger than the 1 MB batch buffer is dropped (`hostgl.cpp:108-109`): about
  65 000 edges. Large maps/charts would vanish. Nit; raise or band it when it shows up.
- `khrHostClear` sets `p[2]` twice (`hostgl.cpp:220, 224`); the second value wins and includes
  zero-sized rectangles, which the host clears as nothing. Harmless; delete the first.
- Dropped draws (`vg_fill` returns 0 for a missing texture or surface) are invisible. Count them
  and expose the counters (section 3, `stats`).

### 1.11 Threading model

All GL runs on the vCPU thread inside an MMIO write, under the BQL, with `eglMakeCurrent` at
every kick (`syborg_gl.c:1151`). [verified in source] That is sound with one vCPU; QMP, QOM
property setters and `screendump` run on the main loop thread and must not touch GL (the design in
section 3 keeps it that way). The Qt window is a separate process that only reads `/dev/shm`
(1.5). Two things to keep in mind for the options in section 2: an EGL context cannot be current
on two threads, so anything that draws on the main loop thread (a QEMU console with GL scan-out)
needs a second, shared context plus fences; and `eglMakeCurrent` per kick becomes measurable when
kicks are small (a few µs each on Mesa [inferred]), so release the context only when something
else needs it.

### 1.12 Smaller observations

- `glReadPixels` in `compose()` (`:1127`) drains the GPU every frame on the vCPU thread: the
  whole frame's fills must finish before the guest continues. This is the main host-side latency
  cost today (section 2.1).
- `vg_ramp` rebuilds a 1024-texel ramp on the CPU and uploads it for every gradient fill
  (`:728-761`). Cache by a hash of (stops, premultiplied) in a small table of ramp textures. Nit.
- Image minification uses `GL_LINEAR` without mipmaps (`:873-875`); `VG_IMAGE_QUALITY_BETTER`
  on a downscaled icon shimmers. A `mipmaps` setting (section 3) and `glGenerateMipmap` at upload
  for that quality. Nit.
- `gcebe` completes `displayed` immediately in `ContentUpdated` (`gcebe.c:581-585`), so the guest
  has no frame pacing from the compositor; see section 3 "pacing".
- `-march=armv6k -mfpu=vfp -mfloat-abi=softfp` for libEGL (`src/khr/build.sh:28`): VFP instructions
  are used (good; soft-float library calls would be several times slower under TCG). An armv7 build
  for the N8 would not change much under TCG. [inferred]

## 2. Presentation path

Today: fills -> MSAA FBO -> resolve -> scan-out texture (`SYGL_VG_PRESENT`) -> `SYGL_COMPOSE`
draws layers into the screen FBO -> `glReadPixels` -> guest frame buffer (through the vCPU's page
tables) -> `syborg_fb` copies it into the QEMU console surface at every refresh (screendump) and
`phone.py` copies it from `/dev/shm` at 60 Hz into a `QImage` -> `QPainter::drawImage` scaled.
[verified in source] Three full-frame copies per frame plus a GPU drain; the window cannot be
crisper than 360x640 and tears (1.5). Latency: compose (drain) + up to 16 ms poll + Qt paint.

### 2.1 Option A: keep the copy path, make it cheap and clean (do first, small)

- Asynchronous read-back: with ES3, `glReadPixels` into a `GL_PIXEL_PACK_BUFFER` plus a
  `glFenceSync`; write the *previous* frame's PBO into guest RAM at the start of the next
  `SYGL_COMPOSE` (or when a QEMU bottom-half sees the fence signalled, from the main loop thread
  with a shared context; simpler: next kick). The vCPU no longer waits for the GPU; the guest
  frame buffer runs one frame behind, invisible to the guest.
- Double-buffer the guest frame buffer: gcebe already knows `EDisplayMemoryAddress`; if the
  display memory has room for two pictures, alternate the destination and flip `BASE` in
  `syborg_fb` (register 1, raises BASE_UPDATE). The window and screendump then always read a
  complete frame. Else the sequence word from 1.5.
- `phone.py`: paint only when the sequence changed; keep the QImage without `.copy()` by
  wrapping the mmap slice (it is read-only, so a copy is needed only if the frame can change
  under the painter, which the double buffer prevents).
Gives: no tearing, no vCPU stall, fewer copies. Costs: a day. Risks: none for the guest.
Keeps screenshots and the guest-visible frame buffer unchanged.

### 2.2 Option B: QEMU GL scan-out to its own display (`-display gtk,gl=on` / `egl-headless`)

QEMU's console API lets a device hand a texture (`dpy_gl_scanout_texture`) or a DMA-BUF
(`dpy_gl_scanout_dmabuf`) to the display and flip with `dpy_gl_update`; virtio-gpu does this.
[inferred: QEMU `include/ui/console.h`, stable since 2.x] `syborg_fb` would register
`graphic_hw_ops.gl_*` callbacks and, when the GL path is on, scan out `scrTex` instead of the
memory picture. QEMU's GL contexts live on the main loop thread (`qemu_egl_rn_ctx`,
`qemu_egl_init_dpy_surfaceless`), so syborg-gl's context must be created *shared* with QEMU's
display context (`eglCreateContext(..., share)`), and each present must insert a `glFenceSync`
that the display thread waits on (`glWaitSync` or `EGL_KHR_fence_sync` -> native fence fd). Doing
`glReadPixels` into the memory picture can then be kept only for screendump and for guest readers,
at a lower rate (every n-th frame, or on demand from the `screendump` hook).
Gives: zero copies, vsynced by GTK, crisp when combined with 2.4.
Costs: the window becomes QEMU's GTK window: keys, touch, menus, scale live in `phone.py` today and
would have to move (QEMU's GTK has no touch injection UI beyond the mouse; `-display sdl` similar).
`egl-headless` alone shows nothing, it only prepares a dmabuf for a client (VNC/Spice/DBus).
Risks: QEMU's GTK GL on Wayland/X11 is fine with Mesa; the shared-context plumbing in our device is
the real work (make-current discipline, 1.11). Recommendation: do the device half (shared context,
`gl_scanout` hooks, fence) because option C needs the same, but do not adopt the GTK window.

### 2.3 Option C: QEMU D-Bus display with DMA-BUF scan-out to the Qt window (recommended target)

`-display dbus,gl=on` exports `org.qemu.Display1`; a listener process receives
`ScanoutDMABUF` (fd, width, height, stride, fourcc, modifier, y0_top) and `UpdateDMABUF` per frame,
and acknowledges with `Done`, which gives frame pacing and damage for free; without `gl=on` it
sends `ScanoutMap`/`UpdateMap` (a memfd of the display surface) instead. [inferred: QEMU docs
"D-Bus display", https://www.qemu.org/docs/master/interop/dbus-display.html] Requirements:
- The device side of 2.2 (scan-out texture, shared context, fences). QEMU exports the texture
  with `EGL_MESA_image_dma_buf_export`
  (https://registry.khronos.org/EGL/extensions/MESA/EGL_MESA_image_dma_buf_export.txt), which
  radeonsi supports. [inferred]
- The window: PyQt6 cannot import a dmabuf by itself. Two ways: (i) a small C helper
  (~200 lines, EGL + `EGL_EXT_image_dma_buf_import`,
  https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt) loaded via
  ctypes that turns the fd into a GL texture inside a `QOpenGLWidget`'s context, or (ii) GTK4
  (`GdkDmabufTextureBuilder`) which handles it natively but means leaving Qt. (i) keeps
  `phone.py`. D-Bus in Python: `dbus-next`/`jeepney` or Qt's own `QtDBus` with fd passing
  (`QDBusUnixFileDescriptor`).
- A "none" fallback: when `gl=on` fails (no dmabuf export) QEMU falls back to `ScanoutMap`
  events; the window code handles both and the shm path of today is the third fallback.
Gives: one GPU-side copy at most, pacing by `Done`, no tearing, no polling, vsync-aligned by Qt,
crisp at any scale with 2.4 (the window scales a high-resolution texture with GL filtering).
Costs: 3-5 days (device shared context + fences; dbus listener in the daemon or the window; EGL
import helper). Risks: Wayland vs X11 does not matter (dmabuf import is display-server agnostic);
D-Bus bus set-up (QEMU can use a p2p socket: `-display dbus,p2p=on,addr=unix:path=...`, no session
bus) [inferred]. Keeps screendump (QEMU keeps the memory surface updated from `glReadPixels` at a
reduced rate or on demand) and the guest frame buffer (still written, 2.1 async).

### 2.4 Option D: render at a higher resolution than the guest (supersampling)

Host surfaces at `k` times the guest size (k = 2: 720x1280) while the guest keeps 360x640: the
device scales edges, scissors, clear rectangles, image/paint matrices and composition rectangles by
k; `SYGL_READ` / `SYGL_VG_TEXREAD` / the guest frame buffer get a `k`-box-filtered downsample
(a blit to a 1x FBO, `glBlitFramebuffer` with `GL_LINEAR` from a resolved texture, then read).
Vector content (paths, text as paths, gradients) becomes genuinely sharper in a 2x window; images
uploaded from the guest (bitmaps, NVG icons rendered into VGImages at guest resolution) are just
magnified with linear filtering; images rendered on the host (`Image* | 1` targets) are rendered
at k and read back downsampled, so the icons drawn by NVG into VGImages *do* benefit if they are
drawn at the time they are sampled (and lose it once read back and re-uploaded).
Gives: a crisp window at scale 2 for everything vector. Costs: every pixel-coordinate path in the
device (a few dozen lines, but each with a precision trap: VG pixel centres at .5 scale to k/2;
scissor rectangles must scale as integers; `floorf/ceilf` of the bbox after scaling). Needs 2.3
(or 2.2) to be visible: with the copy path the window only ever sees the guest frame buffer, so
the scan-out would have to carry the k-size texture and the guest frame buffer the downsample.
Risks: moderate; keep `scale = 1` the default and compare against the RI diagnostic build at
scale 1. Boot-time only (changing it means recreating every surface and rescaling their content).

### 2.5 Option E: native window embedding as the Android emulator does

The emulator's renderer runs *in the emulator process* and draws into a child native window it
creates inside the Qt window (`rcSetupSubWindow`), vsynced by the compositor. [inferred: gfxstream
/ emugl design, https://android.googlesource.com/platform/hardware/google/gfxstream/] Cross-process
embedding is X11-only (XEmbed / child windows); Wayland has no cross-process subsurfaces, and we
run the window in a separate Python process by design. Not recommended; 2.3 is the Wayland-era
equivalent (dmabuf hand-over instead of a shared window).

### 2.6 Recommendation and step plan

1. Option A (async read-back, frame sequence/double buffer, paint-on-change). Immediate relief,
   no new dependencies, and the PBO/fence code is reused later.
2. Device side of B/C: name the device in the board (QOM path), create the context shared with
   QEMU's display EGL context when one exists (`qemu_egl_init_dpy_surfaceless` is already what
   `egl-headless` uses, so `-display egl-headless` + syborg-gl can share Mesa's display), add
   `gl_scanout` hooks to `syborg_fb` behind a property (`syborg-fb.scanout=memory|gl`), fence per
   present. Verify with `-display gtk,gl=on` in a throwaway session (not the owner's).
3. `-display dbus,gl=on,p2p=on` in the daemon, a dmabuf listener in `phone.py` with the ctypes
   EGL helper; keep shm as the fallback (`--screen shm|dbus`).
4. Option D (`scale`) once 3 shows frames straight from the GPU.
Screenshots: `screendump` keeps working at every step because `syborg_fb` keeps its memory
surface (updated from the async read-back at the frame-buffer rate, or on demand in the
`screendump` path through `graphic_hw_update`); `sydb screenshot` is unchanged.

## 3. Configurability

### 3.1 Mechanisms, and which to use

| mechanism | when it applies | thread | verdict |
|---|---|---|---|
| `-global syborg-gl.prop=v` | at device creation; parses *any* QOM property of the type, dynamic ones included [inferred] | n/a | boot-time defaults from the profile/session |
| qdev static property (`DEFINE_PROP_*`) | boot only: qdev refuses sets after realize unless `realized_set_allowed` [inferred] | main loop | keep for `backend`, `scale` |
| QOM dynamic property (`object_property_add` in `instance_init`, with getter/setter) | any time via `qom-set` / `qom-get`; also from `-global` | main loop, BQL held, vCPU may be mid-kick? no: the BQL serialises it against MMIO | **runtime settings**: setter validates and stores a *wanted* value; the vCPU-thread `run()` applies it (GL must not be called from the setter) |
| QMP command (QAPI schema) | any time | main loop | not needed; `qom-set/get` cover it; a `syborg-gl-stats` command only if `qom-get` of a JSON string is too ugly |
| session config file | boot (daemon turns it into `-global`) and runtime (daemon replays it with `qom-set` after `sydb gl set`) | daemon | the single place the owner edits; the device profile holds the defaults |

Needed first: a stable QOM path. `sysbus_create_simple` (`syborg.c:217`) yields
`/machine/unattached/device[N]`, which moves whenever the board changes. Use `qdev_new` +
`object_property_add_child(OBJECT(machine), "gl", OBJECT(dev))` + `sysbus_realize_and_unref` so
the device is `/machine/gl` (and `/machine/fb` for the frame buffer while at it).

### 3.2 Properties

Type `syborg-gl`, QOM path `/machine/gl`. "runtime" = settable with `qom-set`; "apply" = where
the device acts on it.

| name | type | default | validation | runtime | apply |
|---|---|---|---|---|---|
| `backend` | str `host`/`off` | `host` | enum | no | `gl_init` |
| `msaa` | uint32 | 8 | 0, 2, 4, 8, 16; clamped to `GL_MAX_SAMPLES` at apply, reported back through `msaa-effective` | yes | next `run()`: `s->samples` updated; every existing MSAA surface is marked `stale`; `bind_surf`/`bind_read` of a stale surface: resolve into `color`, delete `msfbo`, create it at the new count (or none when 0), draw `color` back into it with the composition shader (a blit *into* a multisampled FBO is illegal in ES 3.0 [inferred: spec 4.3.3]), clear its stencil, `dirty = 1`. Guest-visible state is unchanged: the pixels survive, the stencil is scratch between fills. |
| `filter` | str `auto`/`nearest`/`linear` | `auto` (VG image quality decides) | enum | yes | next fill (`:873`) and compose (`:1106`) |
| `mipmaps` | bool | false | | yes | next upload: `glGenerateMipmap`, min filter `LINEAR_MIPMAP_LINEAR` for quality BETTER |
| `scale` | uint32 | 1 | 1..4 | no | boot (section 2.4) |
| `compose` | str `host`/`guest` | `host` | enum | yes | published in `SYGL_REG_CFG` bit 1; gcebe reads the register before `HostCompose` and khr before `khrHostPresent`: on `guest` both fall back to their memory paths (gcebe `ComposeScene`, eglos's blit, `eglos.cpp:213-223`), which already exist. The device also refuses `SYGL_COMPOSE`/`PRESENT` while off so a stale guest cannot bypass it. |
| `pacing` | str `immediate`/`vsync` | `immediate` | enum | yes | `vsync`: `SYGL_VG_PRESENT` returns a frame number and the guest's gcebe completes `displayed` only after the next `syborg-fb` VSYNC (through the display driver's vsync wait that the N95 stand-in already has [inferred]); the host composes at most once per refresh. Caps the guest's redraw rate, the single biggest CPU saver when alf spins. |
| `overlay` | str, comma list | `""` | subset of `fps,fills,kicks,dirty,stale` | yes | drawn into the scan-out texture only (not the guest frame buffer, so screenshots stay clean once 2.3 exists; before that, document that `overlay` appears in screenshots) |
| `debug` | str, comma list | `""` | subset of `glerror,trace,sync` | yes | `glerror`: `glGetError` per kick, logged; `trace`: one log line per command type per kick with counts; `sync`: `glFinish` at kick end (timing experiments) |
| `stats` | str, read-only | | | get | JSON: kicks, draws, fills, dropped (missing texture / surface), resolves, composes, bytes read back, last kick µs, max kick µs, surfaces, textures, scan-out images, `msaa-effective`, renderer string |
| `epoch` | uint32, read-only | 0 | | get | bumped at `post_load` and at a `gl-reset` (below) |
| `reset` | bool write-only (or a `gl-reset` QMP command) | | | yes | frees every host object and bumps `epoch`: tests the restore path from 1.1 without a snapshot |

Guest visibility: extend the register page: `SYGL_REG_CAPS` (word 1) gets feature bits (0 ready,
1 kick-va, 2 msaa, 3 compose, 4 present, 5 scale>1), new `SYGL_REG_CFG` (word 8: bits 0-7 effective
samples, 8-11 scale, 12 compose-on, 13 pacing-vsync) and `SYGL_REG_EPOCH` (word 9). Because the
page is user-mapped (`d_display.cpp:214-231`) a guest read costs one MMIO access, so khr and gcebe
can read `CFG` at every swap/commit; nothing needs pushing into the guest.

### 3.3 sydb and the device profile

- Profile: `models/<id>/device.toml` gains a `[gl.host]` table (`msaa = 8`, `filter = "auto"`,
  `scale = 1`, `compose = "host"`, `pacing = "immediate"`), validated by `tools/device.py check`,
  emitted by `device.py qemu-args` as `-global syborg-gl.<k>=<v>` next to the existing fb globals
  (`docs/devices.md` line 26 table). Per device, not in the family: the N95's GLES 1.1 path
  ignores `msaa` and the N8 wants it, so the values are not "true of every member for a reason".
  Document the keys in `docs/devices.md` "[gl]".
- Session: `build/sydb*/gl.json` (overrides, written by `sydb gl set --save` and by the window's
  future panel). Daemon start: profile defaults, then session overrides, then `--gl-opt k=v`
  command line, all as `-global`. `--gl soft` stays as the alias for `backend=off`.
- Daemon ops: `gl get` -> `qom-get /machine/gl <each prop>` + `stats`; `gl set {k: v}` ->
  validate against the same table, `qom-set`, optionally persist. CLI: `sydb gl`, `sydb gl set
  msaa=4 filter=linear [--save]`, `sydb gl stats [--watch]`. MCP: `mcp__sydb__gl_get` /
  `gl_set`.
- `phone.py` later: a "Rendering" menu whose entries call the same `gl set` op and read `gl get`
  on open; `stats` behind a status-bar toggle. No new protocol: the window already talks to the
  daemon's socket.

### 3.4 Order of implementation

1. QOM path `/machine/gl`; `stats`, `epoch`, `debug=glerror` (read-mostly, no behaviour change).
2. `msaa` runtime with the stale-surface recreation; `filter`, `mipmaps`.
3. `SYGL_REG_CAPS` bits + `SYGL_REG_CFG`; `compose` with the guest fallbacks reading `CFG`.
4. Profile `[gl.host]`, `device.py qemu-args`, daemon `gl get/set`, `sydb gl`.
5. `pacing=vsync` (needs the gcebe/display-driver vsync hand-shake).
6. `overlay` once the scan-out texture exists (section 2 step 2); `scale` last.

## 4. Other advice

### 4.1 Guest-side performance (the vCPU is the bottleneck)

- **Tessellate on the host.** The RI flattens Béziers, builds strokes (joins, caps, dashes) and
  transforms every vertex in guest float (`riPath.cpp`), then `hostraster.cpp` ships 16 bytes per
  edge. Move the RI's path code (MIT) to the device: `SYGL_VG_PATH` (key, generation, segment
  bytes, coordinates, as `vgAppendPathData` gives them) and `SYGL_VG_DRAWPATH` (path key, 3x3
  matrix, fill/stroke flags, stroke parameters, then the existing paint block). The host caches
  the flattened edge list per (path key, generation, matrix, stroke params): alf redraws the same
  rounded rectangles and glyph outlines every frame, so the cache hits. Same code, same output as
  the RI, and the command stream shrinks. This is the largest win available; the guest keeps only
  API state and the paint set-up.
- **Zero-copy image uploads and read-backs.** `UploadFast` swizzles every pixel in the guest
  (`hostgl.cpp:270-311`) into the batch buffer; `khrHostSync` swizzles back (`:364-371`). Give
  `SYGL_TEX` a format word (ARGB_PRE, ARGB, XRGB, 565, A8, L8, as the RI stores them) and an
  in-place source (`va`, `stride`): the device reads the image memory through the page tables
  (as `scan_upload` already does for GCE surfaces) and converts on the host. Likewise
  `SYGL_READ`/`SYGL_VG_TEXREAD` with a destination `va`, `stride` and the guest's word format:
  no `gOut`, no copy loop. Child images (parent offsets) fit this naturally (base + offset).
- **Pacing** (3.2 `pacing=vsync`): frames nobody sees are pure waste on a slow vCPU.
- **Fewer round trips**: every `vgGetImageSubData`, `vgReadPixels` and `khrHostSync` is a
  synchronous kick with a GPU drain. Group reads that follow each other (the NVG icon path reads
  whole images: one `TEXREAD` per image is already the case) and avoid `khrHostSync` when the
  CPU only *writes* a region that is then fully overwritten (today any CPU write syncs the whole
  image first, patch 0005 [inferred]).
- **Rectangles**: alf draws many axis-aligned rectangles as 4-edge paths; detect them in
  `Rasterizer::fill` (4 edges, axis-aligned) and send a `SYGL_VG_RECT` (or a flag on FILL) that
  the host draws as one quad without the stencil pass. Guest cost unchanged, host cost halved per
  fill; worth it only after tessellation moves.
- **Batching on the host**: one `glBufferData` per fill (`syborg_gl.c:836, 888`) is two
  allocations per fill on Mesa; accumulate all vertices of a kick into one buffer and draw by
  offset. Host-side only.
- Measure first (`stats.last kick µs`, guest-side `gKicks` and time per `eglSwapBuffers` in the
  serial log) per the memory note "measure time, not profile".

### 4.2 Protocol changes to make now, before it ossifies

1. **Version and feature bits**: `SYGL_ID` is a constant; add `SYGL_REG_CAPS` bits and a
   protocol version word (`SYGL_REG_VERSION`) so khr/gcebe/sygl can refuse or adapt. Today a
   mismatch between a ROM's blob and the QEMU binary fails silently.
2. **Owner in the kick**: add `owner` to `SyglKick` (filled by the kernel for 0x7f12 kicks; for
   user kicks the device could compare against the ASID/TTBR, or trust it), drop in-band
   `SYGL_OWNER`.
3. **Per-command status, "declined" vs "error"**: today `result` = failing word + 1 and the rest
   of the batch is abandoned (`run()`, `:1155, 1174-1183`), while unknown objects are skipped as
   success. Add a status word per command that can report (`SYGL_READ`, `PRESENT`, `COMPOSE`),
   written into the kick's `out` area, so the guest can fall back (compose off, 3.2) without
   aborting the batch.
4. **Surface descriptor**: `SYGL_SURF` gets a flags word with orientation (y-up VG / y-down GLES),
   premultiplied or not, and a format; `PRESENT`/`COMPOSE` honour the orientation. Rename
   `SYGL_VG_PRESENT` to `SYGL_PRESENT`.
5. **Tagged paint block**: the colour transform rides as optional trailing words after the edges
   (`:784-786`, `hostraster.cpp:229-234`); the mask will need a texture key; put both into the
   paint block as (tag, length) items, with `flags` reserved. Same for `SYGL_READ`'s optional
   5th word: make it mandatory.
6. **Dynamic limits** announced in CAPS (max surfaces/textures/scan-outs, max batch bytes); the
   device sizes its tables dynamically (1.2).
7. **Epoch register** (1.1) for restore and device reset.
8. **Formats on TEX/READ/TEXREAD and in-place addresses** (4.1).
9. Reserve op numbers for `SYGL_VG_PATH` / `SYGL_VG_DRAWPATH` / `SYGL_VG_RECT` and for a
   `SYGL_FENCE` (returns a frame counter; needed for `pacing=vsync` and for the async read-back).

### 4.3 Things not to do

- Do not route the OpenVG API itself to the host (vg-host.md already says why: no host VG, and the
  RI's handles are pointers). The path/tessellation move in 4.1 keeps the API and state in the
  guest.
- Do not adopt QEMU's GTK window as the owner's window; keep `phone.py` and feed it a dmabuf.
- Do not make `scale` or `msaa` a family value; they are per-device host policy.
