# OpenGL ES 1.1 and 2.0: the guest only forwards (2026-11-10)

Owner's rule: for what moved out of the guest, the guest code is forwarding only. Supersedes the guest halves
of gles2-host.md and gles1-host.md (their host notes still apply: highp promotion, framebuffer 0, read-back
conversion, the matrix palette).

## Guest (src/khr)

- `tools/gen-gles.py` generates `gles_stubs.c` from the SDK's GLES/gl.h + glext.h and GLES2/gl2.h + gl2ext.h:
  343 functions (190 ES 1.1 + 45 `*OES` aliases, 153 ES 2), each one command `{ctx, fn, words}` (scalars one
  word, floats as bits, pointers as the caller's virtual addresses). A call with a pointer, a result, or a
  draw is sent at once and runs while the caller waits; the rest is batched. No sizes, no state, no checks.
- `gles.c`: the transport (lock, emit, flush; the calling thread's id goes with each call), glGetString's
  copy (the string must stay valid in the process), glMapBufferOES memory (the guest allocates what the host
  asks for), the stub table for eglGetProcAddress (the host says whether and for which API).
- EGL (`egl2.inl`): every egl* call forwarded (SYGL_EGL); kept in the guest only the window glue (a
  surface-manager surface for the RWindow, re-made when its size changes, SubmitUpdate after a presented
  swap) and eglQueryString's copy. eglmux.cpp, sygl's EGL and software pipeline, plat_n8.cpp and hw_n8.cpp
  are gone from libEGL (sygl stays for the N95).

## Host (src/qemu-syborg/hw/arm)

- `syborg_gles.h`: the replay. `syborg_gl1_cases.h` / `syborg_gl2_cases.h` (generated): every call not written
  by hand, with the size of each pointer's data decided on the host (gen-gles.py RULES: pname counts,
  `gles_pix_bytes` from the context's own unpack/pack alignment, strings read to their NUL) and outputs
  written back into the caller's memory (`gmem_rw` in the caller's kick).
- By hand: names (handed out per share group on the host), vertex arrays (client arrays are the caller's
  memory, read at each draw over the index range; buffer arrays set in the host context), queries in guest
  terms (bindings as guest names), errors the replay decides (reported first by glGetError), framebuffer 0,
  read-back conversion, shader sources, OES_mapbuffer, the matrix palette (syborg_gl1.h).
- Strings and extension lists: the device profile's `[gl.identity]` (vendor, renderer, version, extensions,
  gles2_version, glsl_version, gles2_extensions), written by tools/device.py at every launch
  (`-global syborg-gl.identity=FILE`): a change needs no ROM rebuild.

## The host EGL (src/syvg/syvg_egl.cpp)

- Configs: the device's (`[gl] egl_configs`: the real N8's 37 from eglprobe, `tools/egl-configs.py`; the 701 and
  808 use the N8's [I]); eglChooseConfig matches and sorts by EGL 1.4's rules; handles are the CONFIG_IDs.
- Per guest thread: the bound API (default OpenGL ES), one current context per API, the last error.
- OpenVG: MiniEGL's contexts and surfaces (the RI), made when a VG context first uses a surface; the MiniEGL
  config is the one closest to the device config's colour and mask sizes.
- OpenGL ES: syborg-gl's host contexts and surfaces through host batches; the GL replay asks
  syvg_gl_current which context a guest thread has.
- Strings: egl_vendor, egl_version, egl_client_apis, egl_extensions from the profile's [gl.identity].
- Not yet: native pixmaps (H-egl-no-pixmap), EGL images and the phone's EGL extensions (H-egl-extensions,
  H-gles-no-eglimage), multisampled GL surfaces (H-egl-msaa-gl).

## Status

Delight copy, GL forwarding: Cut the Rope (ES 1.1), Angry Birds, GTA III (ES 2) at 58 fps. With the host
EGL too: the home screen (alf, OpenVG) and Cut the Rope and Angry Birds draw.

## Boundary review

Merged with another model's review and checked against the source: docs/gpu-boundary-review-2026-11-10.md
(safety findings S1-S10 with status, performance costs, the plan and the verification it needs).
