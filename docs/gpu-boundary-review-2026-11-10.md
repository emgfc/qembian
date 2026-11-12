# Graphics boundary review (Symbian^3 / Belle: N8, 701, 808), 2026-11-10

Merged from two static reviews of the guest <-> host graphics path: one by another model (against base
`66e5aca` plus the then-uncommitted GLES forwarder) and ours (gles-host.md, after `9d2f323`). Every claim
below was checked against the tree at `b1337e4`; [V] = confirmed in the source, [I] = inferred. Nothing was
benchmarked and no hostile workload was run: these are source findings, not measurements.

Status: phases 1 and 2 of `docs/gpu-boundary-roadmap.md` (2026-11-11) fixed S1-S6, S8 and S10; S9 is open.

## Verdict

- **Speed: a good base, room to improve.** Batching, host-resident surfaces, dirty-layer reuse and (until
  `b13fc67`) asynchronous frame readback are in place; OpenVG's one-trap-per-call, page-walking memory
  access, per-draw allocations and synchronous rendering on the vCPU are the costs. "Fast enough" needs
  measurements of this revision.
- **Safety: not an isolation boundary.** Any guest process can reach privileged guest memory through the
  transport, can act for another process's graphics objects, and several error and lifetime paths can hurt
  the QEMU process itself. Priority: memory and owner validation, then the lifetime fixes, then speed.

## The path

guest stubs (generated, forwarding only: `src/khr/gles_stubs.c`, `egl2.inl`, `vg2call.cpp`) -> per-process
1 MB command buffer + 1 MB result area in an RChunk (`src/khr/hostgl.cpp:28`) -> one kick (a store to the
device's register page mapped into the process by d_display ctl 0x7f13, or ctl 0x7f12) -> `syborg_gl.c`
runs the batch inside the kick on the vCPU thread -> host GL ES / OpenVG RI (`src/syvg`) in the QEMU
process. gcebe submits the scene; since `b13fc67` the composed screen stays on the host (syborg-fb, the
frame file and screendump show it).

## Safety findings, by priority

| # | Severity | Finding | Where | Status |
|---|---|---|---|---|
| S1 | Critical | Guest memory is accessed like a debugger: `gmem_rw` uses `cpu_memory_rw_debug` for virtual addresses (no page permissions) and unrestricted `address_space_read/write` for physical ones; the OpenVG/EGL accessors (`syvg_guest_read/write`) too; the kick's result word is written the same way. An output pointer can name read-only pages, code or the kernel's memory mapped in every process, where the phone would fault the app [V] | `syborg_gl.c:640`, `:1736` | **fixed** phase 1 (user-permission translation, `user_rw`) |
| S2 | Critical | The whole register page is mapped user-writable (ctl 0x7f13): besides KICK it holds the legacy physical BUF/LEN/EXEC registers, so a process can run a batch at a physical address, bypassing its address space altogether; d_display says "no permission checking" [V] | `d_display.cpp:137, :289`; `syborg_gl.c:1841-1843` | **fixed** phase 1 (`attrs.user`: KICK only) |
| S3 | High | Owner ids are not capabilities: `SYGL_OWNER` and `SYGL_OWNER_FREE` come from the command stream, so a process can select or destroy another process's objects; the GL calls' thread id and the EGL/VG calls' owner are equally caller-supplied; COMPOSE and scan-out commands need no compositor right [V] | `syborg_gl.c:1711, :1721`; `syvg_egl.cpp` | **fixed** phase 1 (BIND / PID / FLAGS from d_display; compositor right; scan-out claim) |
| S4 | High | Client-array read failure leaves a dangling host pointer: `gles_draw_arrays` logs and continues, the previous draw's (freed) copy stays installed in host GL and the draw is issued [V] | `syborg_gles.h:337-341` | **fixed** phase 2 (all arrays read before host state changes) |
| S5 | High | Shared-context root lifetime: `c->root = sh ? sh->root : c`; destroying the root context while a sharer lives leaves the sharer's name tables freed; re-creating an existing context id frees the old one directly [V] | `syborg_gl2.h` `gl2_new`, `gl2_destroy` | **fixed** phase 2 (`Gl2Share`, reference counted) |
| S6 | High | Inconsistent allocation bounds: `gles_in` caps a copy at 64 MB, `gles_room` (outputs sized from guest values, e.g. info logs by bufsize) does not, nor `gles_read_pixels`; `scan_upload` bounds w/h but not the stride span; `g_malloc` aborts QEMU when it fails; no per-owner object/byte budgets for GL names, contexts, GPU memory [V] | `syborg_gles.h` `gles_room`, `gles_read_pixels`; `syborg_gl.c` `scan_upload` | **fixed** phase 2 (`gl_try_alloc0`/`GL_ROOM`, stride span, per-owner budgets; generated `gles_room` outputs capped in phase 4) |
| S7 | High | `glGet*v` answered into a 17-word host stack buffer while the host GL writes whole lists (GL_COMPRESSED_TEXTURE_FORMATS: dozens on Mesa): QEMU stack overflow [V] | `syborg_gles.h`, `syborg_gl1.h` | **fixed** `6a37857` (512-entry buffer, only the first values go back) |
| S8 | High (cond.) | Deferred frame writes to page addresses captured at compose time, without pinning: a forged COMPOSE could name app pages reused before the timer writes them [V for `66e5aca`] | `compose_async`, `frame_write_pending` | **fixed** `b13fc67` (no guest writes: the frame stays on the host); the dead page snapshot in `compose_async` removed in phase 2 |
| S9 | Medium | Containment: the renderer runs in the QEMU process on the vCPU; a wrapper fault, a fatal allocation or a driver crash takes the whole device down; long compiles/readbacks stall the guest [V] | `syborg_gl.c` `run` | open |
| S10 | Low | DisplayDriver's owner cleanup table has 64 entries and leaves untracked owners' objects alive when full; the host EGL's per-thread records are never dropped [V] | `d_display.cpp`; `syvg_egl.cpp` `Proc::threads` | **fixed** phases 1-2 (256-entry table, untracked processes get no host GL; syvg thread records bounded, dropped at eglReleaseThread) |

Worth keeping: command framing and argument-count checks, input copy limits, per-owner name maps, API and
context matching, cleanup at channel close and machine reset (`8e57ee7`), dirty-layer reuse.

## Performance

Present: one trap per batch; scalar GLES calls batched; surfaces never cross the boundary; dirty-layer reuse;
the composed screen stays on the host (no per-frame copy into guest RAM since `b13fc67`).

Costs [V]:
- OpenVG makes a kick per API call (`vg2call.cpp:22`) and flushes its rendering at the end of each call
  (`vg2.cpp`); alf makes hundreds per frame.
- The batch is copied into host memory (`run_batch`), and every pointer's data is read through
  `cpu_memory_rw_debug`: a page-table walk per page, not the TLB.
- GLES pointer and draw calls synchronize (call-time pointer semantics); client arrays and uploads are copied
  into fresh host allocations per call; OES_mapbuffer is a staged copy.
- `scan_upload` re-specifies dirty layers' textures (`glTexImage2D`) even when size and format are unchanged.
- Every kick makes the device's context current and releases it; GLES may switch to the app's context too.
- All GPU work runs while the guest CPU waits.

## Plan, in order

1. **Validation and authority (S1, S2, S3).** User-mode, permission-checked translation for every guest
   access (the vCPU's MMU with the user index and the access direction; refuse the call on a fault, as the
   phone would fault the app); no user mapping of the physical EXEC registers (KICK alone, or kernel-mediated);
   the owner set by d_display per channel, not by the command stream; compositor-only COMPOSE / scan-out.
2. **Lifetime and bounds (S4, S5, S6, S10).** Reject a draw when any enabled array cannot be read, before
   touching host state; a reference-counted share-group object; checked span arithmetic and per-call and
   per-owner byte/object budgets with recoverable errors (no aborting allocations); drop the dead page
   snapshot in `compose_async`.
3. **Fewer kicks.** Batch OpenVG calls without pointers or results, keeping error order, thread/context
   identity and flush/finish/swap boundaries. Copy small pointer inputs into the batch where economical.
4. **Fewer copies and allocations.** Reusable per-context staging buffers; `glTexSubImage2D` for dirty
   layers whose size/format did not change; translate guest memory through the TLB and use the RAM's host
   pointer. Measure each (drivers differ).
5. **Then, if measurements ask for it: a renderer worker.** A ring with stable registered buffers, fences,
   backpressure and cancellation at process exit; best as a separate process for fault containment (S9).
   Do not defer pointer-bearing calls without copying their data at the call.

Not to be traded for speed: MSAA, filtering, precision, advertised behaviour.

## Verification before calling any of this done

On isolated N8 and Belle instances built from the tree: frame intervals including tails, kick time, calls and
commands per kick, bytes copied/uploaded, allocations, context switches (`SYBORG_GL_STATS=1`, the device's
`stats` property) for UI scrolling and animations and GLES 1.1/2 apps. Focused tests: kernel and other-owner
pointers; physical EXEC from user space; forged OWNER/OWNER_FREE/COMPOSE; a good draw then an unreadable
client array; destroying a root context with a live sharer; oversized dimensions, strides and query outputs;
a full owner table; process exit and reset during composition. Run the marshalling with a mocked GL under
ASan/UBSan before real drivers. A boot or a good screenshot exercises none of these boundaries.
