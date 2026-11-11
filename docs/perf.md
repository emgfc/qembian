# Performance: where the time goes and what was done

Status 2026-11-07, measured on the N8 (Symbian^3, arm1176, flexible memory model) on a Ryzen 9700X.
Routine checks are timings (start-up phase 3, app start); profiles only when working on a speed-up,
before and after (owner's rule).

## How to measure

| What | How |
|---|---|
| start-up | `grep '100058f4/1 = 3' <session>/serial.log` (KPSUidStartupApp = 3: start-up done) |
| UI steps | tap, then poll `sydb screenshot` hashes until the screen changes and is stable (a scratch script did this; a `sydb wait-screen` would be the tool, see docs/tooling-ideas.md) |
| guest: busy or waiting | `tools/pcsample_hmp.py SESSION SECONDS` samples the guest PC through QEMU's monitor (`info registers`, no gdb stop) and attributes samples to ROM files and RAM-loaded code (`SYBCODE:` lines). The idle loop is `SyborgWFIIdle` in KASYBORG.DLL |
| host: where QEMU spends its time | `perf record -F 1999 -p <qemu_pid> -- sleep 25`, then `perf report --no-children --sort sym` / `--sort dso` (`[JIT]` = translated guest code). `qemu_pid` from `sydb status`. `perf_event_paranoid` 2 is enough for one's own process; call graphs (`--call-graph dwarf`) did not record here |
| who flushes the TLB | temporary counter in `tlb_flush` keyed by the guest PC (`cpu->cc->get_pc`), symbolised against the kernel `.sym` (the return address is useless: the CP15 helpers tail-call) |

## Findings so far

1. **Waits, not CPU, made apps slow** (the clock app took ~6 s). The CPU was 93% in the idle loop:
   every touch feedback waited 3 s for an unanswered ISI haptics request (1C 9E). Fixed by a reply
   (H-haptics-fail, models/nokia-n8-rm596/hacks.md): start-up 47 s -> 30 s, clock app ~6 s -> ~1 s.
   Lesson: before any CPU work, sample and look for the idle loop, then `no reply` lines.
2. **Boot is CPU-bound** (7% idle), and almost all of it is QEMU itself: host profile 87% in
   qemu-system-arm, 10% in translated code. Before the TLB work:
   `helper_lookup_tb_ptr` 28%, TB hash lookups (`tb_htable_lookup`, `qht_lookup_custom`,
   `tb_lookup_cmp`) ~15%, TLB refill (`tlb_set_page_full`, `arm_cpu_tlb_fill_align`,
   `get_phys_addr*`) ~18%, `tcg_flush_jmp_cache` 1.5%.
3. **Why the TLB churn**: QEMU flushed the whole softmmu TLB and the jump cache on every ASID change
   (CONTEXTIDR: ~130k per 25 s of boot, every cross-process IPC) and on every DACR write (~170k per
   25 s: the flexible memory model opens and closes its IPC alias domain around each copy,
   `LockIPCAlias`/`UnlockIPCAlias` in memmodel/epoc/flexible/mmu/arm/xmmu.cia).
4. **Floating point is not a lever on the N8**: the float runtime (dfpaeabi.dll) is already a thin
   wrapper over VFP instructions (138 VFP instructions in 1311 words); nothing to replace.
5. **Function returns dominate now**: Thumb C++ returns through `pop {pc}` / `bx lr`; QEMU cannot chain
   those blocks and looks each one up (`helper_lookup_tb_ptr`, ~33% after the TLB work, about two host
   cache misses per lookup). Normal for this guest, not a misconfiguration (reviewed).

## QEMU TLB work (patches/qemu-modern/0005)

| Part | What | Switch |
|---|---|---|
| ASID contexts | an ASID change parks the current TLB tables and jump cache under (secure, ASID) in one of 16 slots (LRU) and brings back the new ASID's; page invalidations, code-page protection (`tlb_reset_dirty`) and translation cache flushes reach parked contexts; any other flush (TLBIALL/TLBIASID, SCTLR/TTBCR, memory map changes, snapshot load) drops them all | `SYBORG_ASID_TLB=0` |
| DACR domains (ARMv6) | TLB entries record their short-descriptor domain (ptw.c); a DACR write drops only the entries of domains whose access level falls (none < client < manager); the jump cache goes when code ever ran from such a domain in that context | `SYBORG_DACR_TRACK=0` |
| 4K pages on ARMv6 | ARMv6 has no tiny pages; with XP=1 (all our ARM11 kernels) no 1K subpage permissions either; QEMU used 1K pages for every pre-v7 core. ptw.c warns if a guest uses 1K granules | `SYBORG_V6_1K_PAGES=1` |
| jump cache | `TB_JMP_CACHE_BITS` 12 -> 14 | rebuild |
| build | LTO on, TCG plugins off (scripts/build-qemu-modern.sh) | rebuild |

Results: 99.8% of ASID switches find a warm context; TLB refill cost about halved
(`tlb_set_page_full` 6% -> 2.8%, page walks 3.3% -> 1.7%); start-up phase 3 27.8 s -> 26.5 s (before LTO).
The lookup cost (finding 5) is untouched, so the gain is modest; UI steps (0.6-1.9 s) did not change
measurably. Environment switches are read by QEMU, so they work on `sydb start` / `sydb phone`.

Bugs found while building it: QEMU stores the fast TLB tables reversed (`mmuidx_to_fast_index`); the
parked loops first paired `d[i]` with `f[i]` (stale entries, a use-after-free crash). Contexts keyed by
(ASID, DACR) were tried and dropped for the domain tracking.

### Expert review (2026-11-07, static)

Verdict: sound for TCG with one vCPU; no blocker. Checked and found correct: ARMv6 I/D TLB ops,
ASID 0 reuse, SCTLR/TTBCR/SCR.NS, `tcg_commit`, snapshot load, CPU reset, TB invalidation vs parked
jump caches (CF_PCREL + CF_INVALID check), `tb_flush` ordering, reversed `f[]` indexing, the DACR level
table, the secure key, PROCID-only CONTEXTIDR writes, 4K pages on ARMv6 (every ARMv6 bootstrap sets
MMUCR_XP). Prior art: nothing upstream (ASID tags in the TLB fast path would touch every host
backend); gem5 / Arm Fast Models tag entries the same way the coherence rules here assume.

| Finding | State |
|---|---|
| jump cache kept after a DACR drop when the code page's TLB entry had already been evicted (code would run that should fault; reachable on arm926/arm1136 per-thread domains) | fixed: executable domains recorded at fill time, per context |
| arm926 (ARMv5) took the domain path with nothing to gain | fixed: ARMv6 only |
| domain masks never cleared by a full flush; victim entries not counted | fixed |
| domain drop scans every TLB index of the current context on each alias lock (~7k/s) | partly: clean indexes skipped. Open: per-index masks; remember the last few fills per domain so the alias drop is O(1) |
| large-page window: with 4K pages every section is a "large page"; one widening window per index covers the kernel half, so any TLBIMVA there flushes the whole index, now in all 16 contexts | open: count it first (stat in `asid_ctx_flush_page_locked` / `tlb_flush_page_locked`); fix = a few large-page ranges per index instead of one window |
| parked tables never shrink (up to 2^20 entries per index), 16 x 22 indexes | open: report bytes; resize on park when the window expired; maybe 8 slots |
| globals instead of per-CPU state (single vCPU only) | open (nit): move into CPUTLBCommon |

## Parked levers (not now)

- PGO for QEMU (`-Db_pgo=generate`, train on a boot, then `use`): the reviewer's estimate 10-20% for the
  branchy lookup helper.
- Jump cache geometry: sweep 12/14/16 bits, try a 2-way probe; measure the hash-lookup share and
  `perf stat -e L1-dcache-load-misses,LLC-load-misses`.
- `info jit`: translation cache flush count during boot; `-accel tcg,tb-size=1024` if more than a few.
- No gdb breakpoints during measurements (`check_for_breakpoints` runs on every lookup once one exists).
- Guest side: a release (urel) kernel; ROM demand paging (the N8's code is paged and byte-pair
  compressed: page-in and decompression in the guest).
