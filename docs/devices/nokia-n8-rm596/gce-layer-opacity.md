# N8 gce layer (CLayerImpl): opacity, source alpha and the other blending inputs

Research note (2026-11-11), static only, firmware nokia-n8-rm596-delight (111.040.1511, gce.dll in core,
code 0x80d20218, **ARM** code in this build). [verified] = read in the ROM bytes; [inferred] = best guess.
Complements composition.md "Backend interface".

## Where

- RTTI `N3Gce10CLayerImplE` at 0x80d2912a, typeinfo 0x80d29044 (bases: MBackendLayer at +0, CBase at +4).
  Primary vtable **0x80d29230, 45 slots (0..44)**, second vtable (CBase dtors) 0x80d292ec. [verified]
- Known slots check out against this vtable: 3 GlobalAlpha = `ldr r0,[r0,#0x7c]`, 14/15 Next/Prev
  (+0x54/+0x58), 17 Surface (+0x94), 25 ZOrder (+0x74), 27 HasNullSurface (+0x84). [verified]

## The opacity answer: slot 13 Flags(), bit 0 = per-pixel source alpha

| slot | code | signature (C, as gcebe calls it) | meaning |
|---|---|---|---|
| 12 | copies bits 0..3 of the argument into +0x78, marks the layer dirty when any changed | `TInt SetFlags(Layer*, const TUint32 *flags)` | setter [verified] |
| 13 | `ldr r1,[r1,#0x78]; str r1,[r0]` | `void Flags(TUint32 *out, Layer*)` (**hidden return pointer: r0 = out, r1 = this**) | TBitFlags32 of the layer [verified] |
| 26 | see below | `TBool IsOpaque(Layer*)` | gce's own opacity verdict [verified code] |
| 2 / 3 | clamp 0..255 into +0x7c / read it | `SetGlobalAlpha(TInt)` / `TInt GlobalAlpha()` | layer alpha, always applied [verified] |
| 10 / 11 | store `*arg` / read +0x8c | `SetX(const TUint32&)` / `TUint32 X()` | a 32-bit value whose top byte is an alpha: a fill/background ARGB colour [inferred] |
| 0 / 43 | slot 0 sets bit 30 of +0xd0; 43 returns it | `TBool HasSourceRect()` | slot 1 GetSourceRectangle falls back to the surface size without it [verified] |

Flag bits (+0x78):
- **bit 0 = alpha from the source pixels.** The real compositionbackend (core 0x80d2a9d0) calls slot 13,
  tests bit 0 and sets the VideoCore element's alpha mode to 2 when set, 0 otherwise; its occlusion pass
  (0x80d2abb6) treats mode&2 as see-through and otherwise calls a layer opaque iff global alpha == 255.
  So: **bit 0 clear = the surface's alpha channel is ignored (ARGB drawn as XRGB)**; bit 0 set = blend by
  source alpha. Equivalent of wselement.h EElementTransparencySource. [verified use; name inferred]
- bit 3 = source flip: the backend combines IsSet(3) with SourceRotation (slot 7) into the element
  transform (0x80d2a936..0x80d2a94a). [verified use; "flip" inferred]
- bit 1, bit 2: not read by the backend's per-layer set-up. In IsOpaque: bit 2 set = opaque (forced);
  bit 1 set = not opaque. [verified code; meaning inferred, bit 1 likely colour/global-alpha-type]
- There is **no global-alpha enable bit** in what the backend reads: GlobalAlpha (slot 3) is copied
  straight into the element (byte at element +44). [verified]

IsOpaque (slot 26, 0x80d27274):
`bit2 ? 1 : (!bit0 && alpha(+0x7c)==255 && !bit1 && (X(+0x8c)>>24)==255)`. [verified]
The real backend does not call it for blending (it uses bit 0 + alpha itself); it is gce's helper.

## What gcebe should do

Per layer in HostCompose: read slot 13; if bit 0 is clear, send the layer as opaque (fmt 0 XRGB) even
when the surface is ARGB; keep global alpha (slot 3) as the layer alpha. Expected effect: the browser's
ARGB window surface with alpha 0 shows as on the phone.

```c
typedef void (*LFlags)(TUint32 *out, Layer *);          /* slot 13, hidden return pointer */
enum { L_FLAGS = 13 };
TUint32 lf = 0;
((LFlags)LV(l, L_FLAGS))(&lf, l);
if (!(lf & 1) && (fmt == 1 || fmt == 2)) fmt = 0;        /* no source alpha: ARGB/PARGB drawn opaque */
```

Check at run time (sydb, one log line per scene): the flags of alf's layer and the browser's layer;
expect bit 0 set where the phone really blends (e.g. alf effects over video), clear for the browser.
