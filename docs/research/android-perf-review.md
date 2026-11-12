# Android performance review, 2026-11-11

The dispatch diagnosis is convincing. The claim that a particular Android mechanism explains the
whole desktop/phone gap is not established. I would first measure equal guest work, then improve
lookup locality, then prototype a guarded return predictor. Expect incremental improvements from
these changes; the evidence does not justify promising a several-fold speed-up.

This is advice, as requested in a review brief. No emulator changes or new
boot benchmarks were run. I inspected the local Android QEMU source, build script, existing review,
and phone profile with the NDK simpleperf reporter. The profile reports 21.08% in
`helper_lookup_tb_ptr`, 8.39% in `qht_lookup_custom`, 3.19% in `arm_get_tb_cpu_state`, 1.10% in the
dynamic TLS resolver and 0.94% in PLT entries. The checked Android source still has the by-value
lookup key; patch 0006 exists separately. Record actual binary build IDs and patch manifests for
every experiment: the build script applies patches only when initially extracting its source tree.

## 1. What the evidence establishes

Returns, other indirect branches and cross-page calls are credible dispatch costs. A small guest
basic block can do less work than locating its successor. TLB improvements can therefore be real
without materially improving boot time. The measured 3% desktop improvement from the forwarding
fix is also a warning against interpreting instruction sample percentages as recoverable time.

Three qualifications matter:

- Twenty-five seconds of a fast desktop and twenty-five seconds of a slow phone do not cover the
  same guest workload. First boot versus a seeded boot is an especially large confounder.
- `cycles:u` weights cycles; `cpu-clock:u` weights scheduled user CPU time. Neither includes all
  kernel execution or off-CPU delay. A vCPU consuming 98.5% of *process* CPU does not establish
  that it receives 98.5% of a core's wall time. The recorded phone event count is about 23.9 CPU
  seconds across the process in 25 seconds, which suggests substantial utilization in this window,
  but says little about later boot phases.
- Samples near loads suggest dependency stalls, not proven DRAM misses. L2/L3 hits, host TLB walks,
  cache conflicts and sampling skid are alternatives. The 15–25% jump-cache miss estimate and the
  fraction of lookups attributable to returns remain hypotheses until counted.

The quoted 0.5–0.6x CPU ratio is a prior from other workloads, not a lower bound for this one.
Pointer chasing, a large instruction working set and short blocks with indirect branches can
produce a different ratio. Sustained phone clocks can also differ substantially from peak clocks.

| Candidate | Assessment and discriminating measurement |
|---|---|
| Lookup working set and dependent loads | Leading CPU hypothesis. Count lookups, miss reasons and invalidations per boot phase; measure cost per lookup and hardware cache/TLB events if available. |
| Scheduling and temperature | Plausible additional wall-time multiplier. Measure vCPU CPU time/wall time, actual CPU residency, sustained clocks and thermal state. Verify affinity and effective nice value; `setpriority` in the JNI source ignores errors and priority alone does not select the X3. |
| Guest waits, storage, paging | Must separate from CPU costs, particularly for UI. Correlate guest idle PC, page-ins, faults, serial milestones and host off-CPU intervals. The earlier haptics timeout is directly relevant precedent. |
| AArch64 instruction-cache maintenance | Real cost, not established as large. Local `util/cacheflush.c` uses CTR_EL0 IDC/DIC checks, `dc cvau`/`ic ivau` when required, and barriers. Instrument `flush_idcache_range` by call site: generation **and branch patching**. Low code-generation self time alone does not bound all this work. |
| W^X | Local split-WX support maps writable and executable aliases; it is not inherently an `mprotect` per TB. Check actual configuration/mappings and protection syscall counts. Compare supported split-WX modes only if maintenance is material; never skip required cache synchronization. |
| Dynamic TLS/shared-library binding | Measured, small. Resolver plus accessor is about 1.6%; PLT about 0.9%. Whole-program gains may differ through inlining, but these samples cannot explain minutes versus seconds. |
| AArch64 softmmu fast path | Slow refill symbols exclude the successful inline fast path, which lives in JIT samples. Inspect generated hot TBs and load/store-heavy phases; compare host instructions/cycles per guest work. There is no evidence here of a universally pathological AArch64 fast path. |
| Host page size/huge pages | Guest 4 KiB granules, host base pages and host huge pages are distinct. Query host page size and actual mappings. Huge guest-RAM mappings reduce host translation pressure, not QEMU software-TLB lookup count or the scattered TB metadata dependency. |
| Android allocator/MTE | Inspect actual manifest, build instrumentation, allocation profile and tagged mappings first. An Android version or tagged pointer alone does not establish active hardware memory checking. No supplied profile implicates allocator work as a major cost. |

Bionic's documentation explicitly says it allocates no surplus static TLS. Thus the previous
review's glibc-like rationale for `-ftls-model=initial-exec` in a `System.loadLibrary` library is
unsupported. Do not adopt it as an APK optimization. Hidden visibility also does not turn a
late-loaded library's TLS into static TLS. Prefer fewer repeated accesses or passing the relevant
per-thread state through existing CPU/context pointers, preserving RCU semantics. See
[Android ELF TLS](https://android.googlesource.com/platform/bionic/+/HEAD/docs/elf-tls.md).

## 2. Cheapest experiments, in order

1. **Establish matched runs.** Same firmware, kernel, writable-disk seed, device options and milestone
   on both hosts. Measure pristine first boot separately from seeded boot. Record boot-to-phase-3
   wall time, vCPU user/system CPU time, and guest idle fraction; include app launch and a repeatable
   scrolling or interaction sequence. Alternate A/B binaries for at least five pairs with comparable
   initial temperature. Report median, spread and failures. Keep final timing runs unprofiled.
2. **Check residency and sustained performance.** Use existing app access or `run-as` to inspect the
   vCPU's `/proc/.../task/.../status`, `stat` and, where readable, `schedstat`; verify allowed CPUs,
   effective nice value and scheduling delay. Read accessible cpufreq/thermal data, or use Android
   tracing where permitted. Compare a cooled run with a repeated sustained run. A lower-load
   performance core can be worth comparing with the prime core. Do not assume privileged events
   or sysfs controls are available on this unrooted device.
3. **Finish the counters already in progress.** Separate helper lookups from main-loop lookups;
   identify returns, cross-page direct calls, other indirect transfers and PSR-induced exits.
   Count empty slots, conflicting PCs, key mismatches, invalid TBs, hash hits/misses, full flushes
   by cause, page clears, translations and domain-scan entries. An empty slot after invalidation
   is not necessarily a compulsory/cold miss. Normalize counts by completed phase. Measure
   instrumentation overhead; sample counters if per-transition increments distort the result.
4. **Profile matching phases with the same event type.** Start with CPU-clock profiles on both
   hosts. Use the system simpleperf's available-event list before attempting cycles, instructions,
   branch misses, L1D refills, last-level misses or host TLB refills. Unsupported PMU access is
   not a blocker: matched elapsed/CPU times and software counts remain useful. Include kernel
   samples only where permission permits. Record which symbols remain unresolved.
5. **Run small, isolated A/Bs.** First internal symbol binding, then page-preserving hash changes
   and 12/13/14-bit cache sizes. Measure actual entry bytes and occupied working set, not just
   hit rate. Separately time cache-maintenance calls, including chaining/unchaining. Inspect
   `info jit` flush counts before increasing TB-buffer size.

The decision rule is simple: excess wall time without excess vCPU CPU time sends the investigation
toward waits/scheduling; excess CPU time for equal work sends it toward dispatch, generated code
and memory behavior. More guest work on one host sends it toward initialization state, retries,
timer behavior or firmware/device differences.

## 3. What I would build, ranked for the phone

These are planning ranges for reduction in CPU-bound phase time, **not measurements or additive
promises**. Zero or regressions are possible. End-to-end boot gains shrink with unaffected waits.

| Order | Change | Plausible reduction | Qualification |
|---|---|---:|---|
| 1 | Local binding of internal functions; preserve explicit JNI exports | 0–3% | Cheap. Prefer an audited export/version script and hidden internal visibility or selective function binding over blanket data-symbol `-Bsymbolic`. Inspect final disassembly; `-fno-plt` alone is not a guarantee. Keep stack protection. |
| 2 | Better page-preserving hash and cache-size sweep | 0–5% | Cheap after counters; benefits depend on conflict rate. Compare two-way only if conflicts justify another probe. |
| 3 | Compact cache with copied key/code pointer and correct validity epochs | 2–8% | Removes the dependent TB metadata fetch on valid hits. Larger entries can undo locality gains. Could combine this with a small hot front cache. |
| 4 | Guarded return predictor, helper prototype then inline hit path | 5–15% if returns and hits are frequent | Largest structural opportunity here. Budget for correctness work and paging/debug tests, not just emitting pushes. Helper-only version may be much smaller. |
| 5 | PGO, then Cortex-X3 tuning as a separate comparison | 0–6% | Train on boot and interactive use; validate on held-out workloads. Compiler flags affect QEMU C code, not automatically TCG's emitted instructions. |
| 6 | Eliminate demonstrably redundant hflags rebuilds and safe PSR exits | 0–4% | Audit all changed state and interrupt delivery. Do not simply remove every rebuild or exit. |
| 7 | Domain masks, bounded parked-TLB memory, precise page invalidation | 0–3% direct saving | More possible if footprint indirectly damages cache locality. Count scanned/retained bytes first. |
| 8 | Specialize/in-line ARM state fetch | 0–2% | Preserve the full actual lookup key; overlaps predictor and PGO gains. |

At 14 bits the existing 16-byte jump entries alone occupy 256 KiB per context, about 4 MiB
across 16 contexts, before the software TLBs and TB metadata. A hypothetical 48-byte copied-key
entry makes that 768 KiB per context. A 64 KiB L1D does not imply that either table fits: a smaller
hot cache backed by the existing cache deserves comparison. Holding entry count constant is not
a fair test of copied-key locality; compare equal byte budgets too.

The hash must retain the invariant documented in `accel/tcg/tb-hash.h`: all addresses in one
guest page occupy a known bucket group. Hash the full **page number** into that group and use
page-offset bits for the within-group index. Update `tb_jmp_cache_hash_page` and both active and
parked invalidators together. An arbitrary full-PC hash can silently break page invalidation.
Changing bit count also changes `TB_JMP_PAGE_BITS` and the size of each page-clear operation.

Prefetching the TB immediately before using it has little latency to hide. Try it only when
independent work can be moved between prefetch and use. Copying the key eliminates a dependency;
prefetch merely attempts to overlap it. Similarly, two-way caches can trade fewer misses for more
work on every hit.

Keep one vCPU; multi-thread TCG does not parallelize one guest CPU's instruction stream. Check
that single-step/one-insn-per-TB, debugging, tracing and icount modes are not accidental benchmark
inputs. Increase `tb-size` only for measured eviction/retranslation pressure. QEMU documents these
[TCG options](https://www.qemu.org/docs/master/system/invocation.html); the local `tcg/region.c`
already requests huge pages for its code buffer. A successful advice call does not prove huge-page
backing. For guest RAM, consider a separate huge-page-advice experiment only with host TLB evidence
and verify actual backing; unrooted Android cannot be assumed to provide a hugetlb pool.

Since the kernel is yours, count IPCs, alias remaps, DACR writes and page-ins per useful operation.
Eliminate redundant work or batch suitable operations; check release-kernel configuration and
hot paging/decompression paths. Preserve user-pointer validation, alias revocation and permission
semantics. If optimizing the emulator's DACR scan, retain tracking of executable domains even
after their TLB entries are evicted. A kernel fast path is worthwhile only if measured IPC work
can be reduced without changing the user-side contract.

Superblocks or unrestricted cross-page direct chaining are later projects. They need mapping-aware
guards/invalidation, precise exception recovery and bounded interrupt latency. A small guarded
last-target cache at a hot indirect site can test their potential much more cheaply.

## 4. Return predictor: corrected first implementation

Use a predictor strictly as a cache of a lookup result. It never supplies the architectural
return PC; the guest instruction must compute that normally. A wrong prediction must only cost
time. Avoid implementing guest calls as native host calls/returns.

Start with a bounded per-vCPU shadow ring (32 or 64 entries plus explicit occupancy), containing
the **runtime** expected return PC/state and a stable call-site ID. A small separate memo table
maps a site/context/return-PC to the last successfully resolved full key and RX code pointer:

```c
Frame { runtime_return_pc, return_thumb, site_id };
Memo  { site_id, actual_pc, cs_base, flags, cflags,
        context_identity, context_epoch, code_epoch, rx_entry };
```

Use wide epochs and a defined wrap policy that clears records. A slot number or ASID alone is
not a lifetime identity. Start with one conservative mapping epoch bumped for any relevant TLB
or permission invalidation, and a code epoch for any TB invalidation. Optimize epoch granularity
only after measuring stale misses. Keep publication and invalidation synchronized with QEMU's
existing execution rules: one vCPU does not mean the process has only one thread.

**Call emission.** Instrument executed BL/BLX paths after the architectural LR value is formed.
Honor conditional execution; capture BLX's operand before overwriting LR. Include
`trans_BL_suffix` and `trans_BLX_suffix`: this ARMv6 non-Thumb2 guest uses the split Thumb call
handlers, not just `trans_BL`/`trans_BLX_i`/`trans_BLX_r`. Push at the suffix, not the prefix.
Do not force resolution/translation of the return target at the call: it may not be mapped yet.

Critically, obtain the expected PC from runtime LR, with the interworking bit represented
separately. Local `gen_pc_plus_diff` deliberately computes addresses from runtime R15 under
CF_PCREL. A literal `s->base.pc_next` in a site record can be wrong when the TB executes through
a different virtual alias. A site may be shared between aliases/contexts; memo records need the
actual runtime PC and context as well as the site ID.

**Return emission and helper.** Recognize `bx lr`, ordinary `mov pc,lr`, and ordinary load/POP/LDM
to PC as candidates after all loads, writeback, interworking and state updates complete. Exclude
exception-return forms initially, including flag-restoring ALU-to-PC and LDM exception returns.
Arbitrary indirect transfers can later share a guarded cache, but are not necessarily returns.

Preserve normal `can_do_io` setup, current CPU-state/cflags calculation, breakpoint handling,
logging and the ordinary fallback. Check the top frame's actual target and complete memo key,
then epochs/context. A hit returns the RX entry pointer; a miss performs the normal lookup.
Only fill a memo after that lookup succeeds. If it returns no TB, use the normal epilogue/main
loop to translate; learning on a subsequent execution is acceptable for the first version.
Do not retain/reuse pointers across a helper operation that can exit or flush without validating
their lifetime again.

The **return target's runtime key** is authoritative, not the caller TB's flags: Thumb state,
conditional execution state, privilege/MMU configuration, floating-point flags and cflags may
differ. Run the destination TB's ordinary entry checks. Disable the optimization in unsupported
debug/replay/icount modes initially rather than assuming that a matching PC is enough.

On a top mismatch, clear the prediction ring or use a bounded search and discard skipped frames.
Both are safe with complete guards; compare their accuracy. Track overflow/underflow explicitly.
Recursion, tail calls, hand-written call idioms and Symbian unwinding affect hit rate. Start by
clearing on exception entry/return and ASID changes; preserving per-context rings is a later
optimization. ASIDs identify address spaces, not individual threads: same-ASID thread switches
can still spoil predictions. Full validation must make that harmless.

**Invalidation contract.** Every path that would invalidate a lookup-cache result must invalidate
the memo result too: physical TB invalidation, full code-cache flush, code-buffer reuse, code
permission changes, TLB page/range/full invalidation, mapping-control changes, ASID reuse, DACR
code-domain drops, reset and snapshot load. Clear rings/memos before reclaiming site storage.
Do not rely solely on active jump-cache flushing: parked caches currently also rely on reading
`CF_INVALID` from the TB, which a copied-key predictor deliberately avoids.

With selective epochs later, cover both pages of a spanning target TB; invalidating page N must
also reject entries starting on N-1 when they span into N. Preserve the existing predecessor-page
invalidation behavior. Parked contexts must receive invalidations while inactive. A physical
code epoch and a mapping epoch solve different problems: remapping a VA need not invalidate its
old physical TB. This also applies to the copied-key jump cache.

Validate on repeated N8/808 boots, demand paging, recycled code pages, virtual aliases, ASID
reuse, same-ASID thread switches, recursive calls, split Thumb calls across pages, ARM/Thumb
interworking, interrupts, unwind paths, breakpoints, reset and snapshot reload. A diagnostic mode
should compare sampled predictor hits against normal lookup under the same synchronization and
assert identical destinations. Measure push cost, return coverage, hits, mismatches, epoch misses
and fallbacks alongside elapsed time. Only inline the hit path after the helper version proves
the validity contract and enough hit rate to repay call-side overhead.

The next concrete investment is matched-phase timing plus the counters already being added,
followed by the page-preserving hash/size experiment. Those results determine whether the larger
return-predictor effort can deliver enough improvement to justify its correctness burden.
