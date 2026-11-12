# An emulated VideoCore for Nokia's own graphics stack: feasibility (2026-11-11)

Question: can the N8 (BCM2727 VideoCore III, Symbian^3/Anna/Belle) and the 701/808 (BCM2763 VideoCore IV,
Belle FP2) run Nokia's own EGL/OpenGL ES/OpenVG/composition binaries against a VideoCore device in QEMU, so
the ROM composition stops replacing them with ours (src/khr, src/gcebe, d_display -> syborg-gl)?

Static analysis only (no boot). Firmware: N8 RM-596 011.012 core + ROFS1, 701 RM-774 113.010, 808 RM-807
113.010, through `symindex.json` (import thunks resolved to export ordinals), `tools/rofs.py`,
`tools/e32load.py` (byte-pair decompression of khclient.dll). [verified] = read in the binaries or the cited
source; [inferred] = best guess. Earlier notes: `docs/devices/nokia-n8-rm596/composition.md` (Q5 said the
same in one paragraph), `egl-vg-host-v2.md`, `accelerometer-hw-path.md` (the kernel-side blocker class).

## Verdict

**Not feasible at an acceptable cost, and it would not reach the goal.** Nokia's kernel-side VideoCore
drivers cannot run on our kernel for the same reason as the accelerometer (vcdriver.dll is a client of
karap/egpio/PowerResourceManager, Nokia's RAPUYAMA platform layer), so at least one Nokia kernel DLL would
still be replaced. The cheapest boundary that keeps every Nokia *user-side* DLL unmodified is a set of
stand-in LDDs (khbridge, khegl_ldd, khmonitor, vcdispmanldd) that hand Broadcom's proprietary KHRN RPC
and Nokia's dispman/gce_rpc messages to QEMU. Serving that RPC means reverse engineering about 340
Broadcom-private call layouts (EGL, GL ES 1.1, GL ES 2.0 and OpenVG, which runs on the VideoCore) in two
generations (VC III khclient.dll, VC IV khclient_vc4.dll) plus Nokia's composition RPC, none of which is
documented anywhere. Our current boundary (public Khronos signatures in forwarders one DLL above khclient,
and gce's backend vtable) is already the narrowest seam where the protocol is public. Keep it.

## 1. The boundary: Nokia's graphics stack on the N8 [verified unless marked]

```
app / alf (huirenderpluginvg)                            wserv: alfclosedrs -> gce.dll -> compositionbackend.dll
  libEGL.dll, libOpenVG.dll, libOpenVGU.dll,                  -> vcbackendserver.dll ("!VC_COMPOSITION_BACKEND_SERVER"
  libGLESv2.dll, libGLESv1_CM.dll, libGLES_CM.dll  (ROFS1,        thread in EwSrv) -> rdispman.dll (49 exports)
  1-3 KB each: pure forwarders)                                        |
      -> khmangle.dll (60 KB, ROFS1)                                   | RBusLogicalChannel
      -> khclient.dll (60 KB, ROFS1: Broadcom KHRN client)            v
         RBusLogicalChannel "RKHRBridge"             vcdispmanldd.ldd -> vcdispman.dll -> vcdispmanvchi.dll ("DISP")
              |                                                        + vcdispmanupdatehandler.dll (UPDH [inferred])
              v                                                        + vcbufmanx.dll ("BUFM": pixel push)
 ====== user / kernel ======                                           + dispcommanddrv.dll (DSPC panel), tv_out.dll
 khbridge.ldd (DKHRBridge: RPC lock, command buffers, bulk buffer)
 khegl_ldd.ldd (DEGLServer, + virtualsurface.dll: EGL_SYMBIAN_COMPOSITION surfaces)
 khmonitor.ldd
              |
              v
 vchi.dll (DVCHI: 32 rx / 8 tx slots, credits, services by fourcc, BULX bulk; channels MeSSI, CCP0-3)
              |
              v
 vcdriver.dll (1011 KB: DVCDriver host bus = MeSSI (MPHI) tx/rx + CCP2 rx, boot of the VideoCore over MeSSI
               (2-stage boot ROM handshake), and the embedded VideoCore III firmware (VMCS: dispmanx,
               khronos server GL11/GL20/VG, GLSL compiler, gce_rpc, bufman, vcam, tvout, vcos/nucleus))
     imports: karap (7), egpio (11), PowerResourceManager (12), ekern
```

| Module | Side | Size | Imports (non-ekern) | Role |
|---|---|---|---|---|
| libEGL/libOpenVG/libGLESv2/... | user, ROFS1 | 1-3 KB | khmangle only | forwarders (composition.md Q3) |
| khmangle.dll | user, ROFS1 | 60 KB | khclient [inferred from Q3] | name-mangling shim over khclient |
| khclient.dll | user, ROFS1 | 60 KB | euser (74 thunks: RBusLogicalChannel::DoCreate...) | Broadcom KHRN client: EGL/GL/VG client state, RPC marshalling; literals `khbridge.ldd`, `khegl_ldd.ldd`, `khmonitor.ldd`, `RKHRBridge`, `RKHRMonitor`, "VideoCore III HW", "OpenGL ES 2.0", "OpenGL ES-CM 1.1", "OpenGL ES GLSL ES 1.00", client APIs "OpenGL_ES OpenVG" |
| khbridge.ldd | kernel | 6 KB | vchi (18 ords), vcdriver (4) | the KHRN channel: strings "DKHR too many commands", "invalid command number", "DKHR eglMakeCurrent parameter mismatch", "DKHRBulkBuffer", "DKHRRPCSema", "Bulk data length too large" |
| khegl_ldd.ldd | kernel | 3 KB | vchi (10), virtualsurface (5) | EGL server side glue (DEGLServer, EGLMutex), opened by vcbackendserver too |
| khmonitor.ldd | kernel | <1 KB | khbridge (2) | monitor channel |
| vcdispmanldd.ldd | kernel | 3 KB | vcdispmanupdatehandler (8), vcdispmanvchi (7), dispcommanddrv (3), vcbufmanx (2), tv_out (1) | composition channel for rdispman |
| vcdispman.dll / vcdispmanvchi.dll | kernel | 3 / 2 KB | vchi (11) | "DISP" service client (DVCDispmanVCHI) |
| vcbufmanx.dll | kernel | 12 KB | vchi (17), vcdriver (2) | BUFM: push/pull (striped) images to VC resources |
| vchi.dll | kernel | 30 KB | vcdriver (24 ords) | VCHI transport (DVCHI, DVCHIService, power handler) |
| vcdriver.dll | kernel | 1011 KB | karap (7), egpio (11), PowerResourceManager (12) | host bus, VC boot, VC firmware image |

The IPC to the VideoCore happens in kernel mode (vchi.dll over vcdriver.dll). User-side code never
touches the bus: the KHRN RPC buffers cross the user/kernel line as RBusLogicalChannel requests to
khbridge.ldd; composition crosses as rdispman -> vcdispmanldd.ldd. These channel interfaces are the only
places where Nokia's user side could be kept unmodified.

## 2. Can Nokia's kernel-side VideoCore drivers run on our kernel?

- **vcdriver.dll: no** [verified imports]. It imports karap ords 22, 25, 34-38 (interrupt bind/enable
  etc.), egpio ords 1, 3, 6, 39-52 (the attention/RUN GPIOs), PowerResourceManager ords 4-46: the same
  RAPUYAMA platform layer our Syborg kernel replaces (H-accel-standin, accelerometer-hw-path.md section 4:
  fixed kernel virtual addresses 0xc603xxxx mapped by Nokia's bootstrap). It would also need the MeSSI/MPHI
  and CCP2 blocks emulated at those addresses and a QEMU model that boots a VideoCore III firmware image
  through the 2-stage boot ROM handshake and then runs it: a VPU emulator. Out of the question.
- **vchi.dll, khbridge.ldd, khegl_ldd.ldd, vcbufmanx.dll, vcdispman*.dll: only through vcdriver**
  [verified]. Their non-ekern imports are vcdriver's exports (24 + 4 + 2 ordinals) and each other's.
  A stand-in vcdriver.dll with Nokia's 59 ordinals (28 real) would keep them unmodified, but its
  interface is a Nokia-private C++ ABI (DVCMessiChannel/DIspCcp objects, DFC queues, bulk descriptors)
  that would have to be reversed from vchi.dll, and QEMU would then have to parse VCHI slots (header:
  magic 0xaf05, size, fourcc, timestamp, credit, payload; CTRL types 0-5; BULX bulk with 0x1c-byte header:
  per rapuyama-linux, E6, [inferred to hold for the N8: same vchi.dll generation]). Their ekern ordinals
  against our kernel are unchecked: the symbian3 personality has no kabi.toml yet (`tools/personality/check.py
  --device nokia-n8-rm596`: "nothing checked") [verified]. The XIP-data/prelink problem of
  rom-memory-investigation.md applies to every Nokia kernel DLL alike [inferred].
- **Cheapest unmodified-user-side boundary: stand-in LDDs at the RChannel line** [inferred]:
  `khbridge.ldd` (RKHRBridge controls: RPC command buffer in, replies out, bulk buffer), `khegl_ldd.ldd`
  (+ its `virtualsurface.dll` import), `khmonitor.ldd`, `vcdispmanldd.ldd` (rdispman's 49-export client
  interface), `vcfilesys.ldd` (vcfsys.exe, probably optional). Nothing of vchi/vcdriver is needed then; the
  RPC payloads go to a QEMU "VideoCore" that serves KHRN, DISP/UPDH, BUFM and gce_rpc. This is the only
  path worth costing (section 5). Note it still removes 5 Nokia kernel modules from the ROM, so the goal
  "no Nokia graphics DLL replaced" is unreachable either way; only the user-side DLLs could stay.

## 3. What an emulated VideoCore would have to answer

| Piece | Who sends it | Documented? |
|---|---|---|
| Boot handshake (MeSSI, 8-word messages, magic 0xafe45251, 2 stages) | vcdriver | rapuyama-linux (E6), not needed with stand-in LDDs |
| VCHI connect (CTRL 0: slot size 0x1020, counts, max msg), service availability, credits, xon/xoff, BULX | vchi.dll | rapuyama-linux (partial, "Next" section); not needed with stand-in LDDs |
| KHRN RPC: EGL (~47 ids), GL ES 1.1 (~75 + 87 common), GL ES 2.0 (~52), OpenVG (~74), misc; each with Broadcom's own argument packing, reply ("khdispatch") and bulk-data rules; GLSL source compiled on the VC | khclient via khbridge | **Not documented for VC III.** Pi userland `interface/khronos/common/khrn_int_ids.h` (BSD-3, Broadcom 2012) gives VC IV ids: GLBASE_ID 0x7000 (glClearColor 0x7007), GLBASE_ID_11 0x1000, GLBASE_ID_20 0x2000, VGBASE_ID 0x3000 (vgClear 0x3014), EGLBASE_ID 0x4000 (eglIntCreateSurface 0x4000, eglIntSwapBuffers 0x400a) [verified in that header]. Whether khclient.dll (VC III) uses the same numbering is unknown [inferred: likely similar for EGL, different for GL/VG; the pasted answer's "0x2003 glClearColor, 0x2076 glDrawArrays" match neither the Pi header nor anything in rapuyama-linux] |
| **OpenVG: on the VideoCore, by RPC**, not on the ARM | khclient | [verified] khclient's client-API string is "OpenGL_ES OpenVG"; libOpenVG.dll only forwards to khmangle; vcdriver's VC firmware carries `VG_SERVER_SHARED_STATE_T`, `VG_PAINT_T`, `VG_FONT_T`, `VG_RAMP_T`, `vg_tess`. The ARM-side `libopenvg_sw.dll` is a separate software VG for icons (composition.md Q3) |
| EGL_SYMBIAN_COMPOSITION window surfaces: EGL surface = GCE surface, composed on the VC; `gce_rpc` service, khegl_ldd + virtualsurface.dll | khclient, vcbackendserver | Nokia-only; **nowhere documented** |
| DISP (dispmanx): N8-era op table (E6: ops 0x00-0x27, GCE-specific 0x1c surface, 0x1e element add, 0x1f advance, 0x17 change attributes, 0x11 async submit), UPDH completions, resources and image types | rdispman -> vcdispmanldd | rapuyama-linux `videocore-display.md` gives a handful of ops with layouts for the E6 (same VC III generation, [inferred] same for the N8). Pi's `vc_dispservice_defs.h` is an older/different enum (0..20: SET_DESTINATION, UPDATE_START, OBJECT_ADD, RESOURCE_CREATE...), useful for names only |
| BUFM/BFMX pixel push (striped, bulk), DSPC panel commands (DCS), tv_out | vcbufmanx, dispcommanddrv | rapuyama-linux (partial); our host has no panel to drive |
| vcfilesys (FILE service), vcam, audio, codecs (vlls/*.vll) | other subsystems | out of scope, but their LDDs/servers exist and will try to open services |

Status of the two external leads:
- **github.com/fluffleswaffles/rapuyama-linux** [verified by fetching README, docs/videocore-link.md,
  docs/videocore-display.md]: real, active (Oct-Nov 2026), Linux 6.12 bring-up on the E6 (BCM2727B1).
  Its docs recover the ViSSI host bus (PIO at 0x20025800, gfx DMA 0x20024800, 16-bit words, GPIO 63
  attention, 92 RUN; the pasted "0x20025810" is a register inside that block), the boot handshake, the VCHI
  slot/entry format and CTRL/BULX, DSPC panel sequences and a few DISP ops; GLES 2 works through its own
  `linux/vc-khrn` (`libvcgl`, `khrntest`, `vctri`) against the VC's GL server, but the docs give **no KHRN
  opcode table or layouts** (the pasted numbers are not there). It has a Unicorn harness that runs Nokia's
  display drivers from the ROM and records their messages, and its own QEMU VC model that "checks MMIO
  counts, not the real VC's behaviour" (no `work/qt/run.py` is visible; `work/` is not in the listing).
  **No licence file** in the repository: naming reference only, copy nothing. VC firmware is extracted
  from the user's own ROM, not shipped.
- **raspberrypi/userland** [verified]: BSD-3-clause (Broadcom Europe 2012). `interface/khronos` is the
  VC IV-era KHRN client with its id table; `interface/vchi` is VCHI over VCHIQ (shared memory), not over
  MeSSI/CCP2; `interface/vmcs_host` has dispmanx. Names and general shape, not byte-exact for the N8.

## 4. VideoCore III (N8) vs IV (701/808) [verified]

| | N8 (011.012) | 701 / 808 (113.010) |
|---|---|---|
| Symbian kernel-side set | vchi.dll (67 exp), khbridge/khegl_ldd/khmonitor, vcdispman*, vcbufmanx, rdispman (49), vcbackendserver (39) | same names; vchi 69 exp, vcbackendserver 41, rdispman 50, compositionbackend 56 exp (N8: 50) |
| vchi -> vcdriver imports | 24 ordinals | 28 ordinals (+ 49, 90, 92-94) |
| vcdriver.dll | 1011 KB: host driver + embedded VC III firmware ("2727A0/B0/B1") | 32 KB: host driver only (same DVCMessiChannel/DIspCcp classes, "no images returned from DVCExe"); firmware images come through `vc_exe_registry.dll` / DVCExe from a carrier not found in core or ROFS 1-3 (only `vlls/bcm2763c1/*.vll` plug-ins) [verified absence; location inferred: another flash section] |
| KHRN client | khclient.dll, 60 KB, 74 euser thunks, "VideoCore III HW" | khclient_vc4.dll, 80 KB, 101 thunks, "VideoCore IV HW", `KHRN_CACHE_T`, `KHRN_GLOBAL_IMAGE_MAP_T` (Pi-era khrn names) |
| Verdict | same transport generation (MeSSI/CCP2 VCHI), same Symbian channel set, **different KHRN client generation** [inferred from the DLL swap] and a different VC firmware. Pi userland is closer to the 701/808 KHRN than to the N8's. Two RPC dialects would have to be served. |

## 5. Effort and risks against what we have

| | Current (forwarders + gcebe -> syborg-gl) | Stand-in LDDs + QEMU VideoCore (section 2) |
|---|---|---|
| Nokia binaries replaced | libEGL, libOpenVG(U), libGLES* (6 forwarders of 1-3 KB), compositionbackend (gcebe), d_display; Nokia's VC kernel set is simply left out | khbridge, khegl_ldd, khmonitor, vcdispmanldd (+ vcfilesys): still 4-5 Nokia modules replaced; vcdriver/vchi left out |
| Protocol to implement | public Khronos signatures (generated from the headers) + gce's 33-slot vtable (done) | ~340 Broadcom-private RPC layouts x 2 generations, khdispatch replies, bulk rules, EGL process-state init, Nokia's gce_rpc/virtualsurface composition, dispmanx ops 0x00-0x27 with Nokia's GCE additions, BUFM, UPDH; all from decompiling khclient(_vc4).dll (60-80 KB ARM) and rdispman/vcbackendserver |
| Guest-only-forwards rule | holds (all state on the host) | holds, but the guest side is Nokia's client state machine (its own EGL/VG object caches), opaque to us |
| State of play | apps, alf, Delight, Cut the Rope run; phases 1-3 of gpu-boundary-roadmap.md done | nothing; first triangle after the EGL subset + process init + one surface path are reversed |
| Risk | known | the khbridge LDD inspects commands ("eglMakeCurrent parameter mismatch", "too many commands"): the RPC also has kernel-side semantics to match; khclient asserts (`RKHRBridge::Panic khronos_assertion`) on any reply it dislikes; no reference implementation of VC III KHRN exists outside Broadcom; rapuyama-linux has no licence |
| Rough size | - | 3-6 months of reverse engineering for parity with today, if the RPC generations do not fight back |

Cheapest alternative if the wish is "fewer of our DLLs in the ROM": none that pays. The forwarders are
Nokia's own design (Broadcom's libEGL.dll is itself a 1.3 KB forwarder to khmangle); ours sit at the same
place with the public API as the wire. A middle option (stand-in `khmangle.dll` only, leaving Nokia's
libEGL/libOpenVG/libGLES* forwarders in place) would change nothing visible and needs khmangle's 87+34+...
mangled export ordinals reproduced: not worth a ROM variant either.

## Open points (not pursued, budget)

- The ekern ordinal sets of vchi/khbridge/khegl against our kernel (needs the symbian3 kabi.toml).
- Where the 701/808 keep the VC IV firmware (DVCExe / vc_exe_registry source).
- The exact RKHRBridge control numbers (khclient.dll decompiled: `tools/syb_sym.py fn` on the extracted
  image) if the stand-in-LDD path is ever reopened.
