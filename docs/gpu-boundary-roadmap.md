# Graphics boundary roadmap (safety first, then speed)

Source: `docs/gpu-boundary-review-2026-11-10.md` (findings S1..S10, performance costs). This file is the
plan with exit tests and status; the review stays the evidence. Phones: the Symbian^3 / Belle path (N8, 701,
808: src/khr, src/syvg, gcebe) and the N95's sygl path (src/sygl/hw.c, ctl 0x7f11), both into
`syborg_gl.c`.

Rule for every phase: the guest only forwards (no state moves back into the guest); a refusal behaves like
the phone (the call fails or the app faults), never like a host abort; nothing traded for speed (MSAA,
filtering, precision, advertised behaviour).

## Phase 1: authority (S1, S2, S3, S10)

| Item | Change | Exit test |
|---|---|---|
| 1a S1 | Every guest-virtual access of a kick (command words, inputs, outputs, the SyglKick itself, syvg's pointer reads and writes) is translated with the **user** MMU index and the access direction (`probe_access_full_mmu`); a page the app could not touch refuses the command. Device pages (MMIO) are refused too. | glsec probe: output at a kernel address, at a read-only (code) page, input from an unmapped page: each refused, memory unchanged |
| 1b S2 | Register access by privilege (`attrs.user`): user mode may read ID/CAPS/KICK and write KICK only; BUF/LEN/EXEC/OUT/OUTLEN and the new kernel registers are privileged | glsec: user writes to BUF/LEN/EXEC are ignored and logged |
| 1c S3 | The owner comes from the kernel, not the stream: d_display binds a process to its user address space (TTBR0) at ctl 0x7f13 (`BIND`), and names the client (`PID`, `FLAGS`) before each kernel-mediated run (0x7f11/0x7f12, under its lock). `SYGL_OWNER` must equal it; `SYGL_OWNER_FREE` is the kernel's or the owner's own (never `~0`); `COMPOSE` and `SCANOUT_FREE` need the compositor flag (the window server, SID 0x10003B20); a GCE scan-out image belongs to its first presenter until the compositor frees it | glsec: forged OWNER, OWNER_FREE of another pid and of ~0, COMPOSE from an app, present into a surface id another process presented: each refused |
| 1d S10 | d_display's owner table bigger, and a process it cannot track gets no host GL (0x7f10 says not ready) instead of objects nobody frees | code review; table size in the log |

## Phase 2: lifetime and bounds (S4, S5, S6, S8 remainder, S10 EGL)

| Item | Change | Exit test |
|---|---|---|
| 2a S4 | A draw whose enabled client array cannot be read is rejected before host GL state changes | mocked-GL test: good draw, then an unreadable array: no draw, no dangling pointer (ASan) |
| 2b S5 | Share groups as a reference-counted object; destroying the root keeps the group for live sharers; re-creating an id goes through the normal destroy | mocked-GL test under ASan: root destroyed, sharer keeps drawing |
| 2c S6 | One checked span helper (`w*h*bpp`, strides, offsets: overflow-safe) for every guest-sized output and upload; `g_try_malloc` with a GL error instead of an abort; per-owner budgets (objects, bytes) | oversized dimensions/strides/query sizes give GL errors; QEMU survives |
| 2d S8 | Drop the unused page snapshot in `compose_async` | code |
| 2e S10 | syvg's per-thread records dropped at thread exit / owner free | long-running app: record count stable |

## Phase 3: fewer kicks (OpenVG)

Batch VG calls that have no pointer outputs and no results, keeping error order (`vgGetError`), thread and
context identity, and flush/finish/swap boundaries. Copy small pointer inputs into the batch. Exit: Delight
menu and alf transitions with kicks per frame and frame times measured before/after (`SYBORG_GL_STATS=1`).

## Phase 4: fewer copies and allocations

Reusable per-context staging buffers; `glTexSubImage2D` for dirty layers of unchanged size/format; guest
memory through the TLB's host pointer for RAM pages (phase 1 already translates through the TLB). Measure
each (drivers differ).

## Phase 5 (only if measurements ask): a renderer worker

A ring with registered buffers, fences, backpressure and cancellation at process exit; preferably a separate
process for fault containment (S9). Pointer-bearing calls copy their data at the call.

## Verification for every phase

- `tests/device-smoke.sh` on the N8 (011.012), the N8 Delight, the 701 and the N95 (sygl path).
- Apps: Cut the Rope (GLES 1.1), a GLES 2 app, the Delight menu (OpenVG through alf).
- `tools/probes/glsec` (phase 1): the hostile cases above, each expecting a refusal; the device logs every
  refusal as `syborg-gl: refused ...`.

## Status

- **Phase 1 done (2026-11-11).** `syborg_gl.c` (`user_rw`, `kick_identity`, `bind_space`, `refuse`, the
  `*_with_attrs` register handlers), `syborg_gl_proto.h` (PID/FLAGS/BIND, rights), d_display (binding at
  0x7f13, PID/FLAGS under GlLock for 0x7f11/0x7f12/close, 256-entry owner table). glsec on the N8 011.012:
  17/17 PASS, every refusal logged. Window server bound with the compositor right (SID 0x10003B20) on the
  N8, Delight and 701. Moving-model phones: a shared address space is never bound (kernel paths only).
- **Phase 2 done (2026-11-11)** except fault containment (S9, phase 5): S4 (`gles_draw_arrays` reads every
  array first), S5 (`Gl2Share`: reference-counted share groups; re-creating an id destroys the old context
  properly; a group's objects go with its last context), S6 (`gl_try_alloc0` / `GL_ROOM`: 128 MB per call,
  no aborting allocations on guest sizes, layer stride span, per-owner budgets: 1024 surfaces, 16384
  textures, 64 GLES contexts, 2^18 names per kind and share group, 4096 EGL objects), S8 (dead page snapshot
  removed), S10 (syvg: idle thread records dropped past 512 per process, a record goes at
  eglReleaseThread). The mocked-GL ASan tests are not written yet: the exits for 2a/2b were checked by code
  reading and the regression runs below only.
- Regression runs (2026-11-11): `tests/device-smoke.sh` N95 8GB (Carousel on the host GL: ctl 0x7f11 path)
  and N8 011.012 PASS; N8 Delight clone: autolock, menu transitions, Cut the Rope menus and a level, no
  refusals; 701 fresh boot: same composition as before, no refusals (its black home is the known bring-up
  state). The 701 and Delight have no seeds for the smoke test.
- **Baseline for phases 3-5 (2026-11-11).** `tools/gl_bench.py` (menu scrolling and home page drags, 28 s,
  keep-awake unlocked) on the N8 Delight clone, two runs: 12-19k kicks, 0.9 VG calls per kick, 125-141
  kicks per frame, 90-139 ms in kicks, 3.6-3.8 ms in composition. Small on this desktop, but the target
  includes much slower hosts (phones): the speed phases go ahead, each measured with the same bench.
- **Phase 3 done (2026-11-11).** Guest: `tools/gen-vg2.py` makes the 51 void OpenVG/VGU calls without
  host-sized pointers queued (`khrVgPost`, src/khr/vg2call.cpp): they ride with the next call that waits;
  inputs up to 4 KB (vgSetfv values, matrices, glyph indices, kernels, LUTs) are copied behind the command
  and the argument names the copy in the command buffer, so the host reads it with no change; NULL and
  bad sizes go the synchronous way. vgFlush/vgFinish stay synchronous. Host: the RI's drawing is flushed
  once per run of VG commands (`vg_settle` before any other command and at the batch end), and a VG
  call writes back its one result word instead of the whole output area. Bench (same clone, workload):
  kicks per frame 125-141 -> 30-33, VG calls per kick 0.9 -> 3.8; host time per frame unchanged (~1 ms:
  the calls still run, the traps and flushes are what went). Correct on the Delight menu, transitions and
  Cut the Rope; no refusals or VG/GL errors. What still ends a kick (`SYBORG_GL_STATS=1` prints it): calls
  with results the app waits for, above all vgGetMatrix (6047 of 6423 kicks: alf reads the matrix back),
  then eglMakeCurrent / GetCurrentContext / GetCurrentSurface and object creation. Cutting those would need
  a guest-side copy of host state (the VG matrix, the EGL current state), which the guest-forwarding rule
  excludes; left as is.
- **Phase 4 done (2026-11-11).** `user_map_read`: an input the app may read that is one run of host memory
  is used in place (GCE surfaces for composition, GLES inputs within a page or physically contiguous); the
  guest does not run during a kick, so nothing can change it meanwhile (phase 5 must copy instead).
  `gl_tmp` arena: GLES inputs that must be copied and every GLES output (`gles_room`) live in a scratch
  stack released after each GL call, kept across kicks (no malloc/free per call in steady state). Composition
  updates a guest-memory layer's texture in place (`glTexSubImage2D`) when its size and format are unchanged.
  `user_rw` reads RAM straight from the host mapping (writes keep QEMU's dirty tracking).
  `SYBORG_GL_NO_DIRECT=1` turns the in-place reads off for comparisons; `sydb gl stats` reports
  `in_direct` / `in_copied` bytes. UI bench (Delight clone): kick time per frame 0.91-1.01 -> 0.81-0.86 ms,
  composition input 1.8 MB in place and 0 copied; no refusals, no GL errors.
  Correction to phase 2 (S6): `gles_room` did have callers (21 generated output cases in
  `syborg_gl2_cases.h`, missed by a search of the hand-written headers); they were still uncapped and
  aborting until phase 4 moved them to the arena with a NULL check (`gen-gles.py`).
- **Phase 5 done (2026-11-11), optional.** `syborg-gl.thread` (profile `[gl.host] thread = "on"|"off"`,
  or `sydb gl set thread=on --save`, a session's `gl.json`; per boot, default off): one worker thread owns
  the GL context. The vCPU keeps syvg (the OpenVG RI and EGL: CPU work answering the app); everything else
  is a job on one ordered queue: the runs of other commands in a guest batch (synchronous: the vCPU waits
  and its MMU is used for guest memory, `GlExec.cpu`), syvg's host batches without results (queued, words
  copied, no guest memory, backpressure at 256), init, the frame timer, reset. Execution state moved into
  `GlExec` (per thread, `gl_ex`), so a host batch on the worker does not disturb a kick; the host frame is
  under `frame_lock` (syborg-fb reads it on the main loop) and the capture journal under `cap_lock`. Not
  a separate process: fault containment (S9) stays open. Checked: glsec 17/17 with the thread off and on;
  UI bench (Delight clone) the same either way (kick time per frame 0.80-0.82 off, 0.71-0.83 on;
  1862 queued and 17413 waited-for jobs); the owner's N8 Delight with the thread on: menu works, and a 3D
  game runs the same with the thread on and off (25-34 fps in a busy scene, owner's run; about 40 before
  the safety phases, owner's earlier observation: a cost to look at in the review, not measured here).
- Queued, not graphics-boundary work (2026-11-11): the 701's locked screen keeps the last frame. The phone
  turns the display off and shows its pop-up clock in partial mode (`popupclock.exe` sends DisplayDriver
  ctl 1/3); we ignore display-off (H-lcd-alwayson) and the 701 profile has `partial = 0` (copied from the
  N8). Fix: partial mode for the 701 once its ctl 3..5 protocol is checked, and a dimmed panel on display-off.
