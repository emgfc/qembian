# OpenGL ES 1.1 on the host GPU (Symbian^3 phones: N8, 701, 808; 2026-11-10)

Owner's decision: OpenGL ES 1.1 runs on the host like OpenGL ES 2 and OpenVG, complete (every function of
the SDK's `GLES/gl.h` and `GLES/glext.h`), not in the guest. Before: sygl's software fixed-function pipeline
in libEGL (src/sygl) drew window-space triangles through the host; 45 GL ES 1.1 entry points were stubs
(buffer objects, compressed textures, glDrawTex, point sprites, clip planes, most queries, the matrix
palette). Cut the Rope (Marmalade) called the missing glBindBuffer and quit at start-up.

## Pieces

| | where | what |
|---|---|---|
| table | `tools/gen-gl1.py` (from the SDK's gl.h + glext.h) | 235 names: 145 core, 90 extension. 146 own functions (SYGL_GL1), 44 shared with ES 2 (SYGL_GL2 on the ES 1.1 context), 45 `*OES` aliases of core functions (OES_fixed_point, OES_single_precision) |
| guest | `src/khr/gl1.c`, generated `gl1_gen.c` (111 functions), `gl1_procs.c` | the calls; client arrays as slots of gl2.c's attribute table; draws with client arrays copied; names allocated in the guest; local state (bindings, array layout, client active texture, mapped buffers) |
| exports | `src/khr/gen_khr.py` + `gl1_alias.txt` | libGLES_CM/libGLESv1_CM -> libEGL `khr_gl*` -> `khr1_gl*` / `khr2_gl*` |
| EGL | `src/sygl/egl.c` (SYGL_GCE) | ES 1.1 contexts are host contexts (`sy_gl2_create(share, 1)`); eglGetProcAddress serves the extension functions; pixmap surfaces are host surfaces read back into the bitmap at the sync points; eglBindTexImage copies on the host (SYGL_GL_TEXIMAGE) |
| host | `src/qemu-syborg/hw/arm/syborg_gl1.h`, generated `syborg_gl1_cases.h`, `syborg_gl1_shared.h`, `syborg_gl1_counts.h` (shared with the guest) | a Mesa OpenGL ES 1.1 context per guest context, sharing with the device's ES 3 context (surfaces are its textures); framebuffer objects with the OES names; the matrix palette emulated |

## Protocol

`SYGL_GL2_NEW {ctx, share, api}` (api 1 = ES 1.1). `SYGL_GL1 {ctx, fn, words}`: scalars one word each
(floats as bits, shorts widened); a vector parameter `[count, elements]` (counts by pname:
`syborg_gl1_counts.h`); a query's result `[count, values]` in the output area. Draws: as ES 2 (gles2-host.md),
the client arrays' slots: 0 vertex, 1 normal, 2 colour, 3 point size, 4 matrix index, 5 weight, 8 + unit
texture coordinates. `SYGL_GL_TEXIMAGE {ctx, surf, w, h, rgba}`: eglBindTexImage.

The shared functions (buffers, textures, pixels, plain state) are ES 2 commands; their ES 2 host cases call
only functions ES 1.1 has. The host refuses every other ES 2 function on an ES 1.1 context: under an ES 1.1
context, libepoxy aborts QEMU for an entry point it knows no ES 1.1 provider of ("No provider of
glBindFramebuffer", seen in a test). For the same reason the ES 1.1 replay uses the OES framebuffer names.

## What is emulated, and why [V = verified, I = inferred]

- OES_matrix_palette (vertex skinning) is not in Mesa's ES 1.1 [V: its extension string]. The host keeps
  the 32 palette matrices and matrix mode MATRIX_PALETTE_OES itself; a draw with the palette enabled has its
  vertices (and normals, by the inverse transpose) blended on the host CPU into eye space and drawn with an
  identity modelview (H-gles1-palette-cpu). Limits 32 matrices, 3 vertex units: as Broadcom's [I].
- OES_matrix_get (FLOAT_AS_INT_BITS queries) answered from the float matrices.
- OES_mapbuffer: the guest maps a copy of the buffer (read from the host) and uploads it at unmap.
- OES_EGL_image: our EGL has no EGL images yet, so glEGLImageTargetTexture2DOES and
  glEGLImageTargetRenderbufferStorageOES fail with GL_INVALID_OPERATION (H-gles1-no-eglimage). Not listed.
- Pixmap surfaces start black on the host, not with the bitmap's pixels (H-gles1-pixmap-init).
- GL_VENDOR/GL_RENDERER are the device profile's; GL_VERSION "OpenGL ES-CM 1.1"; the extension list is
  what is implemented (gl1.c), not the real phone's: to be checked with a GL ES 1.1 probe on the owner's N8 [I].

## Status (2026-11-10)

Delight copy: Cut the Rope starts (an ES 1.1 context, buffer objects), its menus and level 1 draw at
58.5 fps. Angry Birds and GTA III (ES 2) unchanged at 58 fps. Not GL: Assassin's Creed uses direct screen
access and logs "Current Rotation Mode not supported by the DSA device" (no GL context); The Sims 3
creates its ES 1.1 context and window and then makes no GL call and no IPC (blocked before drawing). The
owner reports The Sims 3 worked on the N8 before (with sygl): a possible regression, left for the owner's
manual test.
