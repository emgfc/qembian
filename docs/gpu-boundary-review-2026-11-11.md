# Graphics boundary review, 2026-11-11 (static, adversarial): phases 1-5 of the roadmap

Scope: commits `750a60e..9396bc0` (`git diff 9819b9d HEAD`) against the plan and claims in
`docs/gpu-boundary-roadmap.md` and the findings in `docs/gpu-boundary-review-2026-11-10.md`. Read: the device
(`src/qemu-syborg/hw/arm/syborg_gl.c`, `syborg_gles.h`, `syborg_gl2.h`, `syborg_gl1.h`, `syborg_gl_capture.h`,
`syborg_fb.c`, `syborg_gl_proto.h`), the kernel stand-in (`src/standins/d_display/d_display.cpp`, the
`DLogicalChannelBase` framework in `src/standins/_shared/nokiastub.h`), the guest transport (`src/khr/hostgl.cpp`,
`vg2call.cpp`, `gles.c`, `tools/gen-vg2.py`, `tools/gen-gles.py`), the host OpenVG/EGL (`src/syvg/vg2.cpp`,
`syvg_egl.cpp`), the probe (`tools/probes/glsec/glsec.cpp`), and QEMU 11.1.3's `accel/tcg/cputlb.c`
(`probe_access_internal`, `probe_access_full_mmu`, `tlb_set_page_full`). Nothing was run; nothing was edited.
Line numbers are those of HEAD (`9396bc0`). [V] = traced in the code, [I] = inferred.

## Verdict

- **Safety: much better than 2026-11-10, not yet a boundary for a hostile app.** The memory path (S1) and the
  register split (S2) hold up: every guest access goes through the user MMU index with the access direction,
  MMIO/ROMD pages and unbound address spaces are refused, host batches never touch guest memory, and the
  owner comes from the kernel (S3). What is still open: the scan-out table (any app can take over or exhaust
  the compositor's layers: H1), guest-sized allocations that can still kill QEMU (H2, M3, M4), and the
  phase-5 worker using the parked vCPU's TLB from another thread, which is outside QEMU's contract and only
  safe by accident of the BQL (M1). Pre-existing, unchanged: a draw with an enabled client array that was
  never given a pointer can crash the QEMU process (M9, S9 class).
- **Speed: no regression is visible in the per-kick host path by reading; the claims are measured on the
  wrong workload.** The new translation (`probe_access_full_mmu`) is cheaper than the old debug walk, the
  arena replaces malloc/free, the lock per command is tens of ns. The two things that *can* cost a GLES game
  milliseconds per frame are state-dependent, not per-op: the kick transport falling back to a kernel
  control (P2) and the arena freeing and re-mapping memory every kick when a call carries more than 1 MB of
  copied input (P1). Both have cheap confirmations below. The "host GPU side < 0.5 %" and "kick time per
  frame" numbers come from a UI bench (OpenVG, few draws) and do not transfer to a GLES game.
- **Release readiness:** not before H1 and H2 (a few lines each), and not with the thread on by default
  until M1 is either documented as a deliberate BQL-dependent invariant with an assert, or removed by
  pre-resolving guest memory on the vCPU. The rest can follow.

## Roadmap claims checked and found correct

| Claim | Where | Status |
|---|---|---|
| 1a S1: every guest-virtual access translated with the user MMU index and the direction; device pages refused | `user_rw` :884-919 (`arm_mmu_idx_el(env, 0)`, `MMU_DATA_LOAD/STORE`, `!host` for MMIO/ROMD/`TLB_FORCE_SLOW`), `user_map_read` :925-954, `gmem_rw` :957-973 (physical only when `!va`: the kernel's EXEC), the SyglKick read and result write :2529/:2539, `syvg_guest_read/write` :2177-2193 require `va && !host_batch` | [V] holds. `probe_access_full_mmu` is non-faulting (`nonfault=true`, retaddr 0: cputlb.c:1430), so no longjmp out of the MMIO handler |
| 1b S2: user mode reads ID/CAPS/KICK and writes KICK only | `syborg_gl_read` :2482, `syborg_gl_write` :2507 (`attrs.user`) | [V] holds. (User reads of KICK return `s->result`, the last result of *any* process: a one-word leak, trivial) |
| 1c S3: OWNER must be the kick's process; OWNER_FREE own or kernel, `~0` refused; COMPOSE/SCANOUT_FREE need the compositor right; BIND by TTBR0 | :2113-2120, :2145-2162, :2095-2109, `bind_space` :2427-2452, `kick_identity` :2456-2475; d_display 0x7f12 :308-320 (PID/FLAGS and KICK under `GlLock`), `GlRunKernel` :185-195, `GlSubmit` :271-288, BIND :329-344, `GlRights` :238 (SID 0x10003B20) | [V] holds. Controls run in the client thread (`DNokiaStubChannel : DLogicalChannelBase`, nokiastub.h:61), so TTBR0 at BIND and at a 0x7f12 kick is the caller's. The 0x7f12 path writes PID/FLAGS and KICK under `GlLock`; `GlSubmit` and `GlRunKernel` too |
| Host batches cannot be steered by the guest | `syborg_gl_exec_host` :2195-2226: words built on the host, queued copies by `g_memdup2`, `gmem_rw` refuses every read and any write outside `host_out` :959-966, `user_map_read` refuses `host_batch` :932, VG2/EGL refused in host batches :2127 | [V] holds |
| Binding dropped when the process dies | OWNER_FREE with the kernel right drops the pid's binds :2153-2155; `GlOwnerClosed` :243-258 sends it at the last channel close, which happens before the address space is torn down | [V] holds (a reused page directory meets no stale entry) |
| 1d S10: 256 owners; an untracked process gets no host GL | d_display :200, `GlOwnerTracked` :227, the 0x7f11-0x7f13 gate :297, 0x7f10 ready :304 | [V] holds |
| 2a S4: every client array read before host state changes | `gles_draw_arrays` :356-371 then :372-398 | [V] holds |
| 2b S5: share groups reference-counted; re-made id destroys the old context | `Gl2Share`, `gl2_share_unref`, `gl2_ctx_free`, `gl2_new` (syborg_gl2.h) | [V] holds |
| 2c S6: checked spans, no aborting allocations, per-owner budgets | `gl_try_alloc0`/`GL_ROOM` :766-787, `scan_upload` stride span :1634-1638, budgets :517-540, `gl2_name` 2^18, syvg 4096 objects / 512 idle threads | [V] partly: see H2, M3, M4 (allocations that still abort or exceed the stated cap) |
| 2d S8, 2e S10 | `compose_async` :1781-1805 (no guest pages), syvg thread records (`syvg_egl.cpp` :483-493, :852) | [V] holds |
| Phase 3: queued void VG calls, inputs ≤ 4 KB copied behind the command, NULL stays NULL, bad sizes synchronous, vgFlush/vgFinish synchronous, error order kept | `khrVgPost` (vg2call.cpp:48-77), `gen-vg2.py` :59, :125-137, host `vg_settle` :1995-2001, :2043-2045 | [V] holds. The inline copy is read by the host from guest memory at the guest VA of the command chunk (user RW), so no host-side offset check is needed; the batch runs in order and `vgGetError` is synchronous |
| Phase 4: in-place reads valid for the kick; host batches never map guest memory; `glTexSubImage2D` only for same size/format | :921-954, :932, :1655-1659 | [V] holds (thread off: the vCPU is inside the MMIO store; thread on: it is parked in the sync job) |
| Phase 5: one ordered queue; async jobs carry copies and no CPU; frame under `frame_lock`; capture under `cap_lock`; reset after what is queued; backpressure 256; the worker takes no BQL | `gl_worker` :2287-2314, `gl_job_run` :2317-2355, `exec_split` :2371-2404, `syborg_gl_exec_host` :2206-2218 (`cpu` stays NULL so `user_rw` refuses), `frame_store` :1690, `gl_capture_locked` :2024, reset :2718-2721 | [V] holds except M1, M2, M8 |
| Phase 5: syvg stays on the vCPU and nothing in it calls GL directly | `grep` of `src/syvg` for `gl*(`/`egl*(`: only `eglGetConfigs/GetConfigAttrib` at load time (syvg_egl.cpp:90-99) | [V] holds |

## Findings, by severity

### High

**H1. Any process can take over, or exhaust, the compositor's scan-out images** [V].
`vg_present` (syborg_gl.c:1607-1627) refuses a present only when `c->host && c->owner != owner`. A `GlScan`
created by the compositor's COMPOSE for a guest-memory layer (`find_scan(s, l->sid, 1)` :1850, `c->host == 0`,
`c->owner == 0`) has no owner, so an app that knows or guesses the sid (TSurfaceId: four words, built from
predictable fields) presents its own surface into it: `c->owner` becomes the app, `c->host = 1`, and from then
on `compose` :1854 (`!c->host && ...`) never uploads the compositor's pixels again: the app's image stands in for
the UI layer (status pane, the lock screen, a dialog) until the window server frees the sid. Second, the
table has 64 slots (`MAX_SCAN` :66) and `find_scan(..., make=1)` is reachable from any app through VG_PRESENT
with any sid; 64 presents with made-up sids fill it, after which the compositor's `find_scan` returns NULL and
its layers vanish (:1851 `continue`): the display stops. Third, OWNER_FREE (:2145-2162) never drops the
owner's scans, and only the compositor may free them (:2105), so a sid presented by a process that then dies
keeps its slot and its claim; if the GCE ever hands the same sid to a new process, that process's presents are
refused ("present into another process's GCE surface"). glsec tests only "present into a surface id another
process presented".
Fix: record the creator in `find_scan` (compositor at compose, presenter at present); in `vg_present` refuse
whenever `c->owner != gl_ex->owner` regardless of `host`; give scans a per-owner budget (8 is plenty for
windows) and refuse creation beyond it; in OWNER_FREE drop the owner's scans (or clear their claim so the
compositor can reuse the slot). Add the three cases to glsec.

**H2. A hostile `glShaderSource` can still take the QEMU process down (S6 incomplete)** [V].
`gles2_shader_source` (syborg_gles.h:672-699): `count` up to 4096, each string either `gles_in(strs[i], lens[i])`
(up to 64 MB each, in the arena: 8 chunks of up to 128 MB, so up to 8 such inputs) or `gles_string` (:55-78, up
to 1 MB each, 4096 times); every piece is appended to one `GString`, which grows with `g_realloc` and aborts
on failure. The NUL-string path alone makes a 4 GB `GString` from a 1 MB guest buffer (4096 appends of the same
address): the host either aborts in GLib or is OOM-killed. The phone would answer GL_OUT_OF_MEMORY or compile
garbage; this kills the whole phone. Fix: a total cap on the assembled source (1 MB is far above any shader)
with `gles_oom(c)` and `break`; count bytes before appending.

### Medium

**M1. Phase 5 fills and modifies the parked vCPU's TLB from the GL worker** [I, mechanism V].
With the thread on, a sync OPS job runs `user_rw`/`user_map_read` on the worker with `gl_cpu()` = `gl_ex->cpu`
(syborg_gl.c:211-214), i.e. `probe_access_full_mmu` on another thread's `CPUState`. A TLB miss runs
`tlb_fill_align` -> `tlb_set_page_full` (cputlb.c:1039 `assert_cpu_is_self(cpu)`: an assertion in
`DEBUG_TLB` builds, a documented ownership rule otherwise), a STORE probe on a page that is clean for the code
client runs `notdirty_write` -> `tb_invalidate_phys_range_fast` and `tlb_set_dirty` from the worker. It is safe
today only because the vCPU holds the BQL while it waits in `gl_job_run`, so neither the main loop
(`tlb_flush` through `tcg_commit`, memory transactions) nor the vCPU itself can touch the TLB meanwhile. That is an
invariant of this code, not of QEMU: a QEMU bump that enforces the assert, a second vCPU, or a future change
that lets the vCPU run during a job breaks it with no diagnostic. Fix, cheapest first: (a) `g_assert(!current_cpu
&& gl_ex->cpu && !qemu_cpu_is_self(gl_ex->cpu))` is not enough: document the BQL dependence next to `gl_cpu` and
in the roadmap, and build the DEBUG_TLB variant once to see whether 11.1.3 fires; (b) better: resolve the job's
guest spans on the vCPU before queueing (the command buffer is already a host copy; `gles_in` inputs and
outputs could be translated into a span list in `exec_split`), so the worker never touches `env`.

**M2. `frame_store` reallocates the host frame outside `frame_lock`** [V].
syborg_gl.c:1686-1689 (`g_realloc(s->hframe, bytes)` before `qemu_mutex_lock(&s->frame_lock)` :1690) while
`syborg_gl_frame` (:1713-1728) hands `s->hframe` to syborg-fb, which copies rows under the lock
(syborg_fb.c:128-160). With the thread on, a composition whose W*H is larger than every earlier one (the first
one is fine: `hfOn` is false) frees the buffer the main loop is reading. Rare (a size change), a read of freed
memory when it happens. Fix: realloc under the lock, or keep the capacity decision inside it.

**M3. The per-call arena bound is about 1 GB, not 128 MB** [V].
`gl_tmp` (syborg_gl.c:791-826) caps a single allocation at `GL_ALLOC_MAX` but has 8 chunks, each sized to the
request; `gles_in` caps an input at 64 MB. One call with eight 64 MB inputs holds 512 MB of arena plus whatever
`GString`/`g_try_malloc` the handler adds (H2). The roadmap states "128 MB per call". Fix: a per-call byte cap on
the arena total (e.g. 256 MB) in `gl_tmp`, returning NULL beyond it.

**M4. Allocations that still abort or throw on guest-driven sizes** [V].
- `GuestBuf` (vg2.cpp:154-172): `std::vector::resize` up to 64 MB throws `std::bad_alloc` through `extern "C"`
  (`syvg_vg_call`) into C: `std::terminate`. Fix: `try`/`catch` returning `iOk = false`, or `g_try_malloc`.
- `gles2_shader_source`: H2.
- `frame_store` :1687 `g_realloc` on W*H*4 (bounded by 4096² -> 64 MB, acceptable but not "try").
- `gl_draw`'s `s->vgv` and the N95 fill path were not re-read in depth; `GL_ROOM` is used where I looked.

**M9. (pre-existing, S9 class) An enabled client array without a pointer crashes the host GL** [V for the
state, I for the driver]. `gles_draw_arrays` skips slots with `r->va == 0 && r->buf == 0` (syborg_gles.h:358,
:377), but the forwarded `glEnableClientState`/`glEnableVertexAttribArray` (:817, :1050) enables the slot in the
host context too, whose pointer is NULL (never set) or stale (a previous draw's arena/guest-RAM address,
installed at :390-392 and never reset). Mesa reads client arrays at draw time without validation: a NULL or
stale pointer is a SIGSEGV in the QEMU process, from a state a phone driver would survive with garbage. The
stale-pointer half is new with phase 4 (the arena is released after the call; before, the copies were freed
after the draw, equally dangling). No path found that dereferences them, since every host draw re-installs
every enabled client array first; the NULL case needs only an app that enables an array and draws. Fix: in
`gles_draw_arrays` refuse the draw (GL_INVALID_OPERATION) when an enabled slot has neither `va` nor `buf`; after
the draw, point the installed client arrays at a small static zero buffer so nothing dangles.

### Low

**L1. PID/FLAGS are not always consumed** [V]. `syborg_gl_write` KICK :2529 tests `(value & 3)` before
`kick_identity(s, false)`, so a kernel 0x7f12 with a misaligned SyglKick pointer (the client's `a1`) leaves
`kpid`/`kflags` set. Every kernel-mediated run rewrites them first (d_display :188-189, :275-276, :316-317),
so nothing consumes the leftovers, but the invariant "cleared after each run" is false. Fix: call
`kick_identity` first, or clear in every early exit.

**L2. `readback` / `vg_texread` row check wraps** [V]. :1075 and :1475 `y0 + rows > h` in uint32: `y0 =
0xffffffff, rows = 2` passes; `glReadPixels(0, -1, ...)` gives a GL error and undefined bytes into the app's own
output area; host buffers are sized from `rows`, so no host overflow. Fix: `(uint64_t)y0 + rows`.

**L3. The moving-model window** [V logic, not reachable on current phones]. The register page is one global
user-RW mapping (d_display :334) and the first BIND of a shared TTBR0 succeeds; until a second process binds
(marking it shared, :2440-2447), any process may write KICK and is taken for the first binder. The moving-model
phone (N95) uses the sygl kernel path, so today nobody binds there. Fix: refuse BIND when the profile is a
moving-model kernel (or when TTBR0 equals the kernel's page directory).

**L4. In-place guest pointers assume a synchronous host GL** [I]. `gles_in` returns pointers into guest RAM
(or the arena) for `glTexImage2D`, `glBufferData`, client arrays and indices. Mesa's `glthread`
(`mesa_glthread=true`, drirc per application) defers client-pointer calls and would read after the kick.
Not on by default for QEMU; worth `MESA_GLTHREAD=false` in the launcher's environment and a line in the docs.

**L5. Stats and settings race the worker** [V]. `gl_get_stats` :2661-2675 reads `g_hash_table_size` of tables
the worker mutates, and the counters; `gl_set_msaa/filter/compose` are read by the worker. Word-sized, benign
on x86; make the counters `qatomic` or read them in a sync job for cleanliness.

**L6. The worker is never joined** [V]. `qemu_thread_create(..., JOINABLE)` :2595, no exit path; at QEMU exit
the GL context goes away under a running job. Harmless at exit in practice.

**L7. The thread's sync round trip per kick** (performance, P3 below) is the design; noted here because
`exec_split` also copies `*gl_ex` per run (:2392) and `gl_tmp_trim` runs per OPS job (:2270).

**L8. Scan slots are not budgeted and not freed with their owner** (part of H1).

## Performance

### What is not slower (checked)

- `user_rw` vs the old `cpu_memory_rw_debug`: a TLB probe per 4 KB page (hit: a few loads; miss: the same
  page walk the debug path always did, plus `tlb_set_page_full`) and a `memcpy`, against a page walk per page
  plus `address_space_rw`. Writes keep `address_space_write` as before. Not slower.
- The TLB effect on the guest: the device fills the EL0 index with the app's own data pages, which the app
  touches itself in user mode; large-page bookkeeping (`tlb_add_large_page`) is the same the guest would do.
  No new flush source found. (`contextidr_write` flushes the whole TLB on every process switch anyway.)
- The arena vs malloc/free per input: cheaper, in steady state (but see P1).
- `gl_capture_locked` (a mutex per command), `vg_settle` (a flag test), `gl_tmp_mark/release`: tens of ns.
- `kick_identity` (one hash lookup), the SyglKick read and result write (two probes, one `address_space_write`).
- The three `eglMakeCurrent` per kick (device -> app -> device -> none: `run_batch` :2422, `gl2_use`,
  `gl_errors_drain` -> `gl2_main`, `run` :1960) are unchanged from `9819b9d` (:1805, :1621, :1735) and are the
  largest fixed host cost per kick (a context switch flushes the driver's batch). A `st_ctx_switches` counter
  would show it.
- The two passes in `gles_draw_arrays` and `memset` in `gles_room` (outputs only): negligible.

### Suspects for the game (GTA III, GLES 1.1 through src/khr): ranked, with the cheapest confirmation

**P2 (first: binary and large). The kick transport fell back to the kernel.** A GLES app kicks once per
pointer-bearing or result-returning call (`gen-gles.py` :180-181: `sync = hasptr or ret != 'void'`: every
`gl*Pointer`, every draw, every `glGetError`), hundreds per frame. If BIND failed or user kicks are refused, each
kick is `ctl 0x7f12` (an executive call, `Kern::MutexWait(GlLock)`, four MMIO writes and a read: d_display
:308-320; new since phase 1: the lock and two writes), and a kick refused with `~0` is retried through `ctl
0x7f11`, which copies the whole batch and output in the kernel (hostgl.cpp:99-104). Confirm in the session
log: `KHR: host GPU on (owner N, kick user|kernel)` (hostgl.cpp:82), `SYBSTUB: DisplayDriver host GL bind pid
... : 1|0` (d_display :345), `KHR: batch copied by the driver` (hostgl.cpp:103), and QEMU's stderr for
`syborg-gl: refused`. Before the phases, every process with the mapping kicked from user mode unconditionally.

**P1. The arena frees and re-maps memory every kick.** `gl_tmp_trim` (:842-852) runs after every kick (thread
off, :1952) or every OPS job (:2270) and frees every chunk but the first; chunk 0 is fixed at its first size
(1 MB unless the first request was larger) and never grows. Any call whose copied inputs exceed what is left
in chunk 0 (a client array or index buffer over 1 MB that is not physically contiguous, a texture or VBO upload
over 1 MB, or several inputs that add up) allocates a new chunk of >= 1 MB (`g_try_malloc` -> `mmap`), touches
it (page faults: ~250 per MB), and `munmap`s it at the end of the kick: hundreds of microseconds per kick,
every frame. The roadmap's "no malloc/free per call in steady state" and the comment at :841 ("or the largest
call so far") are false for this case. Confirm: `in_copied` vs `in_direct` in `sydb gl stats` (how much goes
through the arena per second), and a new counter in `gl_tmp` for chunk allocations (`st_tmp_alloc`), or
`perf stat -e page-faults,syscalls:sys_enter_munmap -p <qemu>` while the game runs. Fix: trim to the
high-water mark (keep the largest chunk, or grow chunk 0 to the largest call seen), and trim only when idle.

**P3 (thread on only). One synchronous worker round trip per kick.** `exec_split` (:2371-2404) makes a sync
job per run of non-VG commands: for a GLES app that is one per kick, i.e. two futex hand-offs and the worker's
wake-up latency (10-100 µs on an idle core) per kick, plus `gl_tmp_trim` per job. Hundreds of kicks per frame
make that milliseconds. The owner saw the same fps with the thread on and off, which says the kick count is
moderate or something else dominates; `sync_jobs` in `sydb gl stats` per frame settles it. The UI bench cannot
see this (its kicks are OpenVG, which stays on the vCPU).

**P4. In-place reads are walked twice when they fail.** `gles_in` (:19-35) calls `user_map_read` (a probe per
page until the first discontinuity) and then `user_rw` (a probe per page again). App heap pages are rarely
physically contiguous, so a multi-page input usually fails at page 2 and costs one extra probe: minor, but
`SYBORG_GL_NO_DIRECT=1` is an existing switch that A/Bs the whole phase-4 read path for free.

**P5. Not in the kick: the compositor's per-frame path changed.** Phase 3 moved alf/wserv's VG flushes to the
end of each run and phase 4 switched dirty guest-memory layers to `glTexSubImage2D` with a guest-RAM pointer
and `GL_UNPACK_ROW_LENGTH` (:1656-1659). A sub-image update into a texture the previous frame's GPU work still
samples can stall or stage in some drivers where a re-specification (`glTexImage2D`) would not. The game's
rate is bounded by the compositor's. Confirm: `compose_ms / composes` before and after (the stats property),
and a one-line switch to force `glTexImage2D`.

**P6. The honest answer.** Nothing in the per-kick host path explains 8 ms per frame by reading. The two
state-dependent candidates (P2, P1) do. The definitive cheap test is not static: boot the game at `9819b9d`,
`750a60e` (phase 1-2), `c9671ed` (phase 3), `b525d3c` (phase 4) on a clone and read `sydb gl stats` (kicks,
kick_ms, in_copied, sync_jobs) over 30 s each; the log lines of P2 come for free.

### What the measured conclusions miss

- "host GPU side < 0.5 % of a UI workload" / "kick time per frame 0.81-0.86 ms": `kick_ms` is the wall time
  inside `run()` (:1949-1964). It excludes the MMIO dispatch, `kick_identity`, the SyglKick read and the result
  write (small), the guest's time in the stubs and in `khrHostFlush`, the kernel control path when the
  transport fell back (P2), and all guest time the device causes indirectly (a GLES app's hundreds of
  synchronous kicks per frame serialize the app against the host: the app cannot overlap its own CPU work
  with the host's). `compose_ms` includes up to 1 s of `glClientWaitSync` for the previous frame
  (`compose_async` :1783) under the compositor's kick. A UI bench (menu scrolling: OpenVG, few draws, 0.9-3.8
  VG calls per kick) says nothing about a draw-heavy GLES app; the only GLES data point is the owner's fps.
- "kick time per frame 0.91-1.01 -> 0.81-0.86 ms" (phase 4) is within run-to-run noise of a 28 s bench with
  two runs each; no error bars.
- "the same either way" (phase 5): measured where the thread does nothing (VG on the vCPU); P3 is exactly the
  path it does not exercise.
- Missing counters that would make the next round cheap: context switches per kick, arena chunk
  allocations, sync jobs per frame (present: `sync_jobs`, needs frames), kicks per process, kernel-path kicks
  (d_display could count 0x7f11/0x7f12 and print at close), `in_copied` per call site.

## What glsec does not cover (add before calling phase 1-2 done)

A present into a compose-created sid and the 64-slot exhaustion (H1); a `glShaderSource` of 4096 x 1 MB
(H2); a call with eight 64 MB inputs (M3); the misaligned 0x7f12 kick followed by a user kick (L1); an enabled
array without a pointer (M9); OWNER_FREE's effect on scans; a frame-size change with the thread on (M2). The
probe runs with the thread on and off, which is good; it should also run under a `DEBUG_TLB` QEMU once (M1).

## Fixes (main session, 2026-11-11)

| Finding | Status | What changed |
|---|---|---|
| H1 scan-out claims | **fixed** | `vg_present` refuses a present into an entry another process claimed, whether or not it has a host image (compositor-made entries are unclaimed, owner 0); a process claims at most `SCAN_PER_OWNER` = 16; the table grew to 256; OWNER_FREE drops the process's claims; a refused present now fails its command. glsec: "S3 present into another process's GCE surface refused" and "H1 at most 16 GCE surfaces per process" (17/17 PASS with the thread off and on). Residual: a process can still claim a guest-memory-only surface id before its own app presents into it, if it knows the id; closing that needs surface ownership from the guest's surface manager (out of scope). |
| H2 shader source | **fixed** | `gles2_shader_source`: 1 MB for the whole source, GL_OUT_OF_MEMORY beyond. |
| M1 worker uses the parked vCPU's MMU | **documented** | The invariant (synchronous job, the vCPU blocked holding the BQL, one vCPU) is written at `gl_cpu`; the thread stays opt-in (`[gl.host] thread`). |
| M2 frame realloc outside the lock | **fixed** | `frame_store` grows `hframe` under `frame_lock`, with `g_try_realloc`. |
| M3 arena bound | **fixed** | `GL_TMP_MAX` = 192 MB for all chunks together. |
| M4 aborting allocations | **fixed** | syvg `GuestBuf`: malloc, no throwing resize; H2's GString capped; `frame_store` non-aborting. |
| M9 enabled array without a source | **fixed** | A draw with an enabled array that has neither a client pointer nor a buffer is refused (GL_INVALID_OPERATION). |
| L1 KICK order | **fixed** | `kick_identity` runs before the alignment check, so a refused privileged kick consumes PID/FLAGS. |
| L2 y0 + rows wrap | **fixed** | 64-bit sums in `readback` and `vg_texread`. |
| L4 Mesa glthread | **fixed** | The device sets `mesa_glthread=false` unless the user set it. |
| P1 arena trim | **fixed** | After a kick the arena keeps one chunk as large as that kick needed (up to 16 MB). |
| L3, L5, L6 | open | Moving-model binding (unreachable today), benign stats races, worker never joined. |
| P2-P5 (game slowdown) | to measure | The owner's game is not a benchmark target (owner's rule); the counters the review names (`sydb gl stats`: in_copied/in_direct, sync_jobs; the log lines `kick user|kernel`, `bind ... : 1|0`) are there when a target is set. |
