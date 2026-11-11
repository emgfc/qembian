# EGL and OpenVG on the host (v2 design, 2026-11-10)

Owner's decision (2026-11-10): EGL and OpenVG move out of the guest, as the fake modem did. The guest's
libEGL.dll becomes a forwarder; all EGL and OpenVG state and all pixels live in QEMU (syborg-gl), as on the
real N8, whose Broadcom libEGL forwards every call to the VideoCore.

## Why

v1 (vg-host.md) keeps the Khronos RI front end in the guest: API state, paths, paints and a CPU copy of
every image's pixels, with drawing on the host. Two copies of every image had to be kept coherent by hand,
and each fix exposed the next case (2026-11-10 alone):
- an image written by the CPU while bound as a render target kept its stale target on the host (black
  screen with alf effects on);
- the CPU copies filled alf's 16 MB heap (H-khr-pixel-heap: a 128 MB side heap as a stop-gap);
- a CPU change uploaded lazily, after the host had drawn into the target, overwrote that drawing (the
  701's home-screen swipe: a dozen black frames);
- full-surface read-backs and re-uploads on every page swipe (slowness).
With one copy of each object, on the host, these cases do not exist.

## Split

| | guest (libEGL.dll, libOpenVG.dll) | host (QEMU syborg-gl) |
|---|---|---|
| EGL | marshalling of every call; the native window glue (GCE surface for an RWindow, its size, SubmitUpdate after a swap); thread identity | displays, configs (the N8's 37, eglprobe), surfaces (window = FBO + scan-out of the GCE surface, pbuffer, pbuffer from a VGImage), contexts, current context per guest thread, swap/present, errors, strings |
| OpenVG / VGU | marshalling (generated from openvg.h / vgu.h, as tools/gen-gl2.py does for GLES 2); handle numbers allocated in the guest (no round trip on create) | the RI front end (C++, built into QEMU) with its drawing on the GPU (the v1 host rasteriser, called directly); images, paths, paints, fonts, masks; CPU-side RI operations on host memory |
| OpenGL ES 2 | unchanged (already remoted, gles2-host.md) | contexts created by the host EGL |
| OpenGL ES 1.1 | sygl stays in the guest for now (its own EGL side in eglmux) | unchanged (SYGL_DRAW) |

## Protocol (new commands in syborg_gl_proto.h)

- `SYGL_EGL` `fn, thread, args...`: one command per EGL call; calls with results flush with an output area
  (the existing kick/OUT mechanism).
- `SYGL_VG2` `fn, thread, args...`: one per VG/VGU call; data inline (path segments and coordinates, image
  data, parameter arrays); results (vgGet*, vgGetError, vgReadPixels, vgGetImageSubData, vgHardwareQuery,
  vgGetString, vgPointAlongPath, ...) synchronous.
- Handles: guest-allocated 32-bit numbers per guest process, mapped on the host (as GLES 2 names); the
  RI's pointer-to-handle casts are replaced by that table (the host is 64-bit).
- Ownership: every object belongs to the guest process (SYGL_OWNER); its last DisplayDriver channel closing
  frees them (SYGL_OWNER_FREE), as now.
- Threads: the current EGL context is per guest thread; each command carries the thread id.

## Phases

1. Host RI library: the RI front end built for the host (handle table, 64-bit clean, drawing through the
   existing GL fill/clear/image code), a host-only test that draws a scene and compares pixels.
2. VG protocol: generated function table and host dispatch; guest forwarder for OpenVG/VGU.
3. EGL on the host: configs as the N8, window / pbuffer / VGImage-pbuffer surfaces, contexts per thread,
   swap and present, ES 2 contexts through it; guest forwarder with the window glue.
4. Remove the guest RI (src/khr RI build, ri-patches 0001-0010, H-khr-pixel-heap, tex_into_target).
5. Test matrix: N8 011.012 / Anna / Belle / Delight, 701, 808: boot, home screen, menu transition, home
   screen swipe, Angry Birds (GLES 2), ZigGLTri (GLES 1.1), eglprobe against the owner's phone.

Each phase keeps the phones working (v1 stays the default until phase 3 passes the matrix).

## Status

- 2026-11-10, phase 1 started: `scripts/build-syvg.sh` builds the RI front end for the host
  (build/syvg/libsyvg.a, C++, g++ -O2 -fPIC). It compiles 64-bit clean once VGHandle is pointer-sized in
  the host copy of openvg.h (the RI casts objects to handles in 115 places; the guest keeps 32-bit handles,
  mapped by the device). It needs only four things from outside (`src/syvg/syvg_os.cpp`): the current VG
  context and "image bound as an EGL surface" (from the host EGL, `src/syvg/syvg.h`), and a mutex (none:
  the device runs under QEMU's lock). `tests/syvg/run.sh`: a drawable and a context made by hand, a clear
  and a filled rectangle with the RI's own rasteriser, pixels exact. Next: drawing through the GPU (the
  v1 fill/clear/image paths, called in process), then the host EGL.
- Phase 1 done (2026-11-10): the RI's drawing goes to the GPU in process. `src/syvg/vghost.cpp` and
  `vgraster.cpp` are the v1 guest code ported (src/khr/hostgl.cpp, hostraster.cpp; the RI's rasteriser is
  not built), with the v1 RI seams (`src/syvg/patches`, copies of ri-patches 0001-0003, 0005-0008). Their
  commands run in syborg-gl through `syborg_gl_exec_host` (a host batch: no guest memory, results into a
  host buffer). RI objects get 32-bit device ids from a table (`vgKey`), not truncated pointers.
- Phase 2/3 host side (2026-11-10): QEMU links libsyvg (scripts/build-qemu-modern.sh) and executes
  SYGL_VG2 (every OpenVG/VGU call: `tools/gen-vg2.py` generates the table, the guest stubs and the host
  dispatch; pointer sizes in its SIZES table) and SYGL_EGL (`src/syvg/egl2_fn.h`, `syvg_egl.cpp`: the
  RI's MiniEGL on the host, functions renamed syvg_egl*, OS layer `syvg_eglos.cpp`: EGL per guest process,
  contexts per guest thread, windows = the guest's GCE surface id + size, swap = SYGL_VG_PRESENT, no
  pixmaps). Pointer arguments are read and written in the calling thread's memory during its kick. v1
  guests are unaffected (Delight copy boots to its home screen). Next: the guest forwarder libEGL.
- Guest forwarder (2026-11-10): `KHR_V2=1 src/khr/build.sh` builds libEGL.dll without the RI (112 KB, v1
  ~270 KB): the generated OpenVG/VGU stubs (`vg2_gen.c`) over `src/khr/vg2call.cpp`, the RI side of eglmux
  forwarding (`src/khr/egl2.inl`, included by eglos.cpp: window glue kept in the guest), sygl and GLES 2 as
  before. eglmux no longer reads a magic word at small handles (the host's ids). First result: Delight boots
  to its home screen and the menu opens with its transition, all EGL/OpenVG state on the host. v1 is still
  the default build.
- v2 is the only build (2026-11-10): `src/khr/build.sh` builds the forwarder; the guest RI, `src/khr/ri-patches`,
  `hostraster.cpp` and the RI shim are gone (v1 is in git history up to 3de5a70). All eight khr ROMs (N8 PR1.0,
  Anna, 011.012, Belle, Delight, Reborn, 701, 808) boot on it. The RI is built with -DNDEBUG (an RI assert had
  aborted QEMU). `syvg_owner_free` releases the process's MiniEGL instance (patch 0013 `syvgReleaseEGL`):
  displays, surfaces, contexts and their VG objects, with their GPU objects. Image spans with a negative
  stride are marshalled from below the pointer (`imageBack`), and output spans are read in first so the
  write-back keeps the bytes between rows.
- Machine reset (2026-11-10): the owner's Reset (sydb phone) left the 701 with a white 360x360 block and a
  broken home screen. syborg-gl had no reset: owner ids are guest process ids, the next boot's processes got
  the same ids and found the previous boot's surfaces, textures, scan-out images and EGL/VG state. Now
  `syborg_gl_reset` calls `syvg_reset` (every owner freed as at its exit, tables, batch and keys emptied),
  then drops every surface, texture, scan-out image and GLES 2 context (their share-group objects too), the
  pending frame read-back, and clears the screen FBO. The other syborg devices got resets too (fb, intc,
  timer, keyboard, pointer, audio, nvmem, serial: their DMA addresses and queues were the old boot's).

## Why the host library is C++

Only `src/syvg` (libsyvg.a) is C++; the QEMU device (syborg_gl.c) stays C and calls it through the C
interface in `src/syvg/syvg.h`. The library is the Khronos OpenVG 1.1 reference implementation and its
MiniEGL, which are C++ (Image, Path, VGContext, RIEGLDisplay classes), the same code v1 ran in the guest.
Reusing it was the cheapest correct path: a C rewrite of OpenVG would be a project of its own, and hosts
have no OpenVG to call (Mesa dropped its OpenVG state tracker in 2015). Costs: QEMU links libstdc++;
MiniEGL's functions are renamed syvg_egl* so that they cannot interpose the host's EGL in the QEMU process;
the RI is built with -DNDEBUG (an RI assert would abort QEMU).
