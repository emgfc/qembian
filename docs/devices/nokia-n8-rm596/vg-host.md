# N8 OpenVG on the host GPU (src/khr M2): design

Status 2026-11-07: steps A, B, C (image draws, pattern paints), D (linear and radial gradients),
colour transform, F v1 and v2 (images as render targets, read back when the CPU needs them) and
anti-aliasing (8x MSAA) work; frames are presented and composed on the host (composition.md "Host
composition"). The first-boot screens (language, Phone switch, country, date with the virtual keypad)
draw completely. Open: masks (logged when used: "fill with masking"), filters on the host, exact
multiply/stencil image modes. Owner's direction: our own EGL/OpenVG, drawn on the host GPU; no
software rendering path now (H-khr).

## Split

| | where | what |
|---|---|---|
| VG semantics | guest, libEGL.dll | the Khronos RI front end: API state and error rules, paths (flattening, stroking, dashing), paints, image objects, fonts, VGU; MiniEGL |
| pixels of drawables | host, syborg-gl | one FBO (colour + stencil) per RI `Drawable` (window, pbuffer, pbuffer from a VGImage) |
| pixels of images | guest, with a host texture | RI `Image` memory stays authoritative; uploaded (SYGL_TEX, by key + generation) when drawn |
| images rendered into | host | an image bound as an EGL pbuffer is host-authoritative while it has a host FBO; drawn from it directly, read back only when the CPU reads it |

Why not remote the OpenVG API to a host VG library: no host OpenVG on GLES 2 exists that we could
use, and the RI on the host would rasterise on the CPU (and cast pointers to 32-bit handles).

## Seams in the RI (patches on our copy, src/khr/patches/)

1. `Rasterizer::fill()` (ours, hostraster.cpp): the edge list (already in surface coordinates), the
   fill rule, the scissor rectangles and the pixel pipe's state go to the host as one SYGL_VG_FILL.
   Host: clear the stencil in the edges' bounding box, draw a triangle fan per edge from a pivot
   with increment/decrement by direction (non-zero) or invert (even-odd), then cover the box with the
   paint shader where the stencil test passes, blended as the VG blend mode says.
2. PixelPipe state the fill needs (friend access): paint (colour; linear/radial gradient: points,
   ramp as a 256x1 texture, spread mode; pattern: image + tiling mode), surface-to-paint and
   surface-to-image matrices, image draw mode (normal / multiply / stencil), image quality, blend
   mode, colour transform, masking (mask surface as a texture).
3. `Surface::clear` / `Surface::blit` on a host-backed drawable: SYGL_CLEAR with scissors, image to
   surface (upload + textured quad without blending), surface to image (SYGL_READ, marks the image
   changed). vgReadPixels / vgGetPixels / vgCopyPixels go through these.
4. `Image` writes (subdata, clear, blit, copy, filters, child images share the parent's memory) bump a
   generation counter so the host texture is refreshed only when the pixels changed.
5. `Drawable` construction/destruction: SYGL_SURF / SYGL_SURF_FREE with a stencil buffer.

## EGL

- Window: on swap SYGL_VG_PRESENT makes the frame the GCE surface's scan-out image on the host, then
  SubmitUpdate; gcebe's SYGL_COMPOSE puts it on the screen (composition.md). No read-back.
- Pbuffer from VGImage: the image gets a host FBO; while bound it is host-authoritative.

## As built

- Host ids: a drawable's surface = its RI `Surface*`; an image render target = `Image* | 1`; image
  textures = `Image*`. Draws of an image sample the live render target if one exists.
- RI patches: 0001 drawables on the host (clear, read), 0002 image generations (every pixel writer
  bumps the root image's counter; textures re-uploaded only then), 0003 image render targets (copy in
  at bind; at release the host keeps the pixels as the image's texture: the guest copy is then stale,
  so CPU reads of such images (vgGetImageSubData, filters) still see old pixels: step F v2).
- Anti-aliasing: drawables and image targets are multisampled (SYGL_SURF_MSAA, `-global
  syborg-gl.msaa=N`, default 8, needs a GLES 3 host context): the stencil fans and the cover run per
  sample, so path edges, strokes and image edges get coverage like the RI's 32-sample software fill.
  A surface is resolved into its texture when read (read-back, present, sampled as an image).
- Gradients: the paint block carries the surface-to-paint matrix, spread mode, the RI's points or
  centre/focal/radius and the stops (sRGBA, not premultiplied); the host builds a 1024-texel ramp
  (premultiplied or not as the paint says) and evaluates the RI's gradient functions per pixel (no
  ramp integration over the pixel's footprint).
- Colour transform: 8 floats after the edges; applied to the paint colour unpremultiplied.
- Images rendered on the host (RI patch 0005): CPU reads and changes of such an image (vgGetImageSubData,
  vgCopyImage, filters, vgSetPixels into a guest drawable...) first read its pixels back
  (SYGL_VG_TEXREAD). The N8's icons are drawn so (NVG into a VGImage, then read): without it they were
  missing.
- Failing EGL calls are logged once per RI line (patch 0004, "KHR: EGL error").
- VG errors are logged once per calling address (patch 0007, "KHR: VG error CODE from ADDR": the address
  where the API call returns to, in the calling DLL), fills the RI drops silently (non-invertible paint
  matrix, patch 0008) and fills without edges once ("KHR: fill with no edges").
- Diagnostic builds (a separate image, `device.py build ID --out .../hybrid-diag.img` after the khr
  build; rebuild khr without the variables afterwards): `KHR_FILLLOG=N src/khr/build.sh` logs the first N
  fills of each kind (colour, fill rule, edge box, target size and surface, image key);
  `KHR_VGTRACE=N` logs the first N VG/VGU calls by name. Used to show that the N8 boot splash draws
  exactly what its NVG frames contain.
- Fill limits: multiply/stencil image modes and the multiply/screen/darken/lighten blends are
  approximations exact for opaque destinations; masks are not applied.
- Not saved in snapshots: a restored N8 session shows a black screen (docs/sydb.md "Catch,
  snapshots and forks"; fix in docs/gpu-host-review.md 1.1).
- Host objects are kept per process (the owner id) in hash tables; the display driver frees a
  process's objects when its last DisplayDriver channel closes (exit, kill, panic).

## Effects (transitions), 2026-11-10

alf (`external/oss.FCL.sf.mw.uiaccelerator`, hitchcock) draws effects only when its renderer allows "visual
pbuffer surfaces": CHuiFxEffect::Draw/CachedDraw return EFalse otherwise and the visual is drawn plain (the
menu transition was 30 identical frames). HuiVg10RenderPlugin allows it for an EGL vendor starting "Br";
ours now says "Broadcom" as the phone (H-khr-egl-n8). The effect path then:
- keeps canvas render buffers (full-screen VGImages bound as pbuffers) across frames and draws them while
  bound: the host samples the live target (vg_fill);
- writes such images from the CPU (TEXREAD, guest write, upload of a new generation): the upload now goes
  into the live target (`tex_into_target`), else every draw showed the cleared target (black screen);
- needs ~17 full-screen images in alf's process: the image pixel heap (H-khr-pixel-heap).
Menu open on Delight: three layers slide in (38.4 px) and fade in through VG_DRAW_IMAGE_MULTIPLY over 30
frames. Debugging aid: `SYBORG_GL_DUMP=N:DIR` in QEMU's environment writes every host surface as PAM at the
N-th present (0: every present, overwriting); `tools/gpu_capture.py` and the session's `gpu-history.bin`
journal show the commands per frame.

## Steps

A. drawables on the host + vgClear + read back at swap (alf's clears visible)
B. fills with a colour paint, blend modes, scissoring (rectangles, rounded rectangles, text paths)
C. vgDrawImage (normal/multiply/stencil), pattern paint, image upload by generation
D. gradients
E. masks, colour transform, pixel blits
F. images as render targets (alf's offscreen render buffers)
Every step compared against the RI's software output on the same scene (a diagnostic build).
