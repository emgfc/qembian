# TB lookup hotspot: diagnosis and levers (advice, 2026-11-11)

Static review of the two profiles in `build/android/perf/` (desktop `desk.data`: 808 boot, vCPU thread,
25 s, `cycles:u`, Ryzen 7 9700X, our GCC 16.2 LTO `-O2` build; phone `perf.data`: same 25 s on the
S23 Ultra, Cortex-X3, `cpu-clock:u`, NDK clang LTO `-O2 -march=armv8.2-a`) against the QEMU 11.1.3
sources as built and `patches/qemu-modern/0005`. No QEMU was run. Each claim is marked
[verified] (read in the code or the profile) or [inferred] (estimate, best guess).

Summary: the diagnosis "returns cannot be chained, each runs the lookup helper" is right, but
"the helper's time is cache misses on the jump cache" is only right on the phone. On the desktop
81 % of the helper's 41 % sits on one instruction right after a 16-byte reload of the
`TCGTBCPUState` struct that was just written with 8+4+4-byte stores (a GCC/x86 codegen
artefact: store-to-load forwarding failure; the jump-cache and TB loads themselves take ~3.5 % of
the helper there). That is the cheapest and largest lever on the desktop. On the phone the helper
is 21 % and 48 % of it is exactly the two expected misses (jump-cache entry, TB struct). The
structural fix for both is a return stack / key-copying cache; the next cheap one is that every
`msr cpsr`/`cps` ends its block with an exit to the main loop plus a double hflags rebuild.

## 1. Diagnosis

### 1.1 Desktop (x86-64, GCC): where `helper_lookup_tb_ptr` spends 41 %

`perf annotate helper_lookup_tb_ptr` (15.5k samples in the function) [verified]:

| share | instruction (our binary, `cpu-exec.c:377`) | what it is |
|---|---|---|
| 80.9 % | `40447a: shr %cl,%rax` | first use after `movdqa 0x20(%rsp),%xmm0` (16-byte reload of `s` = {pc, flags, cflags}) which follows `mov %r8,0x20(%rsp)` (pc, 8 B), the callee's `flags` store (4 B at 0x28) and `mov %esi,0x2c(%rsp)` (cflags, 4 B). Three narrow stores, one wide load. |
| 4.6 + 2.3 % | `push %rbp`, `lea ...` at entry | skid of the indirect `call` from translated code |
| 1.8 % | `lea -0x7(%rax),%ecx` | `TARGET_PAGE_BITS - TB_JMP_PAGE_BITS` (target_page is a runtime variable) |
| 1.7 % | `test %rax,%rax` after `mov (%rcx),%rax` | the jump-cache entry load (`jc->array[hash].tb`) |
| 1.6 % | `jne` after `cmp %rdi,0x8(%rax)` | the TB struct load (`tb->cs_base`) |
| 0.9 % | miss path entry (`sub $0x20,%rsp` before `call tb_htable_lookup`) | |
| ~0 | `check_for_breakpoints`, `one_insn_per_tb`, `qemu_loglevel` tests, hash arithmetic | cheap |

What GCC did [verified in the disassembly]: `tb_lookup(cpu, s)` and `tb_htable_lookup(cpu, s)` take
`TCGTBCPUState` (24 bytes, pc/flags/cflags/cs_base) by value; it is passed in memory, so GCC builds
the copy for the (unlikely) `tb_htable_lookup` call *before* the jump-cache probe, with a 16-byte
`movdqa` straight over the slots the callee and the helper wrote with 8/4/4-byte stores. A load that
is wider than the stores it overlaps cannot be forwarded from the store queue; the core waits until
the stores retire and commit to L1D (Zen 4 measured ~19 cycles for one such failure,
[chipsandcheese Zen 4 part 2](https://chipsandcheese.com/p/amds-zen-4-part-2-memory-subsystem-and-conclusion);
Zen 5 not measured there). [inferred] The sample pile-up on one instruction, and the fact that the
memory-miss loads (which dominate on the phone) are nearly free here, point at this stall and possibly
a memory-renaming misprediction on top (the load has the same base+offset as the 8-byte store two
instructions earlier, which is the pattern Zen's memory file speculates on; a wrong prediction is a
pipeline flush plus the forwarding wait). A pure 19-cycle stall is too small for 33 % of all time
unless the helper runs ~70 M/s; a flush + wait (~40-60 cycles) at ~25-30 M lookups/s fits the other
shares (translated code 14.5 % = ~20 host cycles per block, which is what short Thumb blocks cost).

Decisive check, 10 minutes, no code: `perf stat -e ls_bad_status2.stli_other,ls_stlf,cycles:u -t <vCPU tid>
-- sleep 20` during a boot (both events exist on this host, `perf list`). STLF failures at tens of
millions per second confirm it. Then the fix (section 3A) and a re-annotate: the `shr` line must drop
to noise.

Other desktop costs around the lookup [verified shares]:
- `cpu_exec_loop` 3.95 %, of which 89 % is on the load right after `xchg %ax,0x4042(%rbx)`
  (`qatomic_set_mb(&cpu->neg.icount_decr.u16.high, 0)` in `cpu_handle_interrupt`: an implicitly
  locked instruction, a full barrier on every pass through the main loop). It is upstream code; what
  we control is the number of main-loop passes (section 1.4).
- hash path 9.9 % (`qht_lookup_custom` 4.1 % — 80 % of it on the first touch of the bucket line,
  i.e. the bucket miss; `tb_htable_lookup` 3.2 %; `tb_lookup_cmp` 2.6 %; `get_page_addr_code_hostp`
  0.5 %, the code-page TLB probe inside `tb_htable_lookup`).
- `arm_get_tb_cpu_state` 2.3 %: pure compute (flag assembly, `shl $0x17`), plus its `ret` 17 %
  (return mispredict/skid); GCC devirtualised the `tcg_ops->get_tb_cpu_state` call into a compare
  and a direct call.
- hflags rebuild chain ~3.5 %: `rebuild_hflags_a32` 0.98, `rebuild_hflags_common` 0.41,
  `rebuild_hflags_common_32` 0.32, `arm_rebuild_hflags` 0.39, `fp_exception_el` 0.37,
  `arm_mmu_idx_el` 0.49, `arm_security_space_below_el3` 0.24, `arm_mmu_idx_to_security_space` 0.23,
  `helper_cpsr_write` 0.28, `cpsr_write` 0.24.
- our patch: `asid_ctx_jmp_clear_page` 1.1 %, `dacr_write` 0.83 %, `tlb_drop_domains_locked` 0.31 %,
  `tlb_flush_page_by_mmuidx_async_0` 0.74 %.

### 1.2 Phone (arm64, clang): where the 21 % goes

`simpleperf report --symbols helper_lookup_tb_ptr --sort vaddr_in_file` + `llvm-objdump` [verified]:

| share | instruction | what it is |
|---|---|---|
| 28.1 % | `e02cd8: cbz x20` after `ldr x20,[x25,#0x10]` | jump-cache entry load (`array[hash].tb`) |
| 20.0 % | `e02cec: cmp x11,x9` after `ldr x11,[x20,#8]` | TB struct load (`tb->cs_base`) |
| 3.2 % | `e02cf4: ldr w11,[x20,#0x10]` | `tb->flags`, same line |
| 2.8 % | `e02c3c: blr x9` | the indirect call to `arm_get_tb_cpu_state` (clang did not devirtualise) |
| 2.8 % | `e02c30: str xzr,[sp,#0x18]` | zeroing the sret struct |
| rest | ~0.4-0.5 % per instruction, evenly | prologue/epilogue, stack protector (`mrs TPIDR_EL0` + canary), flags, hash |

Clang keeps pc/flags/cs_base in registers (x24/w10/x9) and writes the by-value struct only on the
miss path (`e02d98-e02da0`), so the phone has no equivalent of the desktop stall. The phone's helper
is the memory picture we assumed: two dependent far loads per hit (entry line, then TB line) ≈ 10 %
of all vCPU time, the rest of the helper (~11 %) is call overhead, the state fetch and the key compare.

Hash path on the phone: `qht_lookup_custom` 8.4 % + `tb_lookup_cmp` 2.2 % (the unnamed `+e0333c`,
`+e03354`) + `get_page_addr_code_hostp` 0.9 % ≈ 11.5 %. Unnamed hot addresses resolved with
`llvm-symbolizer`: `+e043f8` = `cpu_test_interrupt`, `+e197a4/+e19804` = `asid_ctx_jmp_clear_page`
(0.94 % together), `+e1deb4` = `do_ld_4`, `+e1c4b4` = `tlb_hit_page`, `+12eb7b4` = `arm_ldl_ptw`.

### 1.3 How many lookups miss the jump cache

No counter exists; estimate from the shares [inferred]. Let H be the helper's own cost per call
(every call pays it) and M the extra cost of a miss (xxhash, bucket miss, TB compare, code-page TLB
probe). Phone: hash path / helper = 11.5 / 21 = miss_rate × M/H. A miss costs 2-3 far misses against
the hit's 2, so M/H ≈ 2-3 → miss rate ≈ 15-25 %. Desktop: 9.9 / 41 looks lower, but the helper's 41 %
is inflated by the stall; against the helper's "memory" share only, the ratio is similar. So:
roughly one lookup in five goes to the hash table. That is high for a 16K-entry direct-mapped cache
with a warm working set, and three things explain it:

1. [verified] The hash uses only pc bits 0-23: `tb_jmp_cache_hash_func` folds `pc ^ (pc >> 5)` and
   takes bits [6:0] and [18:12] of that (TB_JMP_PAGE_BITS 7). Code 16 MB apart maps to the same slot:
   the ROM (tens of MB at 0x80000000+), RAM-loaded DLLs and the kernel collide systematically.
2. [verified] With CF_PCREL (set unconditionally for ARM system mode, `target/arm/cpu.c:1824`),
   **every `tb_phys_invalidate` flushes the whole jump cache** (`tb-maint.c:896-904`,
   `tb_jmp_cache_inval_tb` → `tcg_flush_jmp_cache` for every CPU; the loop nulls all 16K entries).
   Every write to a page that holds TBs (code page-in into a recycled physical page, loader fix-ups,
   breakpoints) cold-starts the cache. The count is in `info jit` ("TB invalidate count"; via
   `sydb qemu_hmp 'info jit'`) — read it once during a boot before choosing the geometry lever.
3. [verified] Our patch adds flushes: a DACR drop of a code domain flushes the cache
   (`0005` line 292), a page invalidation clears 128 entries in each of the 16 parked caches
   (`asid_ctx_jmp_clear_page`, 1 % on both hosts), and an evicted ASID context (17th process) comes
   back cold.

### 1.4 Why there are so many lookups (what ends a block with a lookup or an exit)

[verified in `target/arm/tcg/translate.c`]:
- Every return: `bx lr`, `pop {.., pc}`, `ldm {.., pc}`, `mov pc, lr` → `gen_bx` / `store_reg(15)` →
  `DISAS_JUMP` → `gen_goto_ptr` → `tcg_gen_lookup_and_goto_ptr` → `helper_lookup_tb_ptr` + `goto_ptr`
  (`br x0` / `jmp *%rax`, an indirect host branch that mispredicts whenever the return target varies).
- Every direct branch or BL to **another 4 KB page**: `gen_goto_tb` only chains when
  `translator_use_goto_tb` says the target is on the same page as the block start
  (`translator.c:106-120`). Symbian calls between DLLs and across the ROM are mostly cross-page, so
  the call side is a lookup too, not just the return.
- Every indirect call (`blx rX`, `ldr pc, [..]` import stubs, virtual calls): lookup.
- Every `msr cpsr_*` / `msr spsr_*` / `cps` (interrupt enable/disable, mode switches in the EKA2
  dispatchers): `gen_set_psr` → `gen_lookup_tb` → `DISAS_EXIT` → `tcg_gen_exit_tb(NULL, 0)`: a full
  **main-loop round trip** (the locked `xchg` above, interrupt check, main-loop `tb_lookup`), and
  `HELPER(cpsr_write)` calls `arm_rebuild_hflags` unconditionally ("TODO: not all cpsr bits are
  relevant") on top of `cpsr_write`'s own conditional rebuild for M/E/IL changes
  (`op_helper.c:789-794`, `helper.c:8327`).

### 1.5 Is anything in our patch making lookups or misses more expensive?

- Not the hit path: parking swaps the `cpu->tb_jmp_cache` pointer; the helper reads it once per call
  (`mov -0x3e40(%rbp),%rbp`, 0.26 %) [verified]. ASID parking is a net win for hits (warm per-process
  caches instead of a flush per switch).
- Yes for invalidations: `asid_ctx_jmp_clear_page` writes 2 × 128 entries × 16 parked caches per
  `tlb_flush_page` (1.1 % desktop, 0.9 % phone) [verified]; on the phone `tlb_flush_domains` 1.4 % and
  `tlb_reset_dirty` 1.2 % (the parked tables are walked on every dirty reset) are the patch's too.
  `TB_JMP_CACHE_BITS` 14 made every full flush 4× longer (16K stores), but flushes are not visible in
  either profile, so the rate is modest [inferred].
- Parked caches keep entries of TBs invalidated meanwhile; they are caught by the `tb_cflags(tb) ==
  s.cflags` compare (CF_INVALID) on the hit path — correct, but it is the reason the TB deref cannot
  simply be dropped without a generation check (section 3B).

## 2. Levers, ranked

Gains are of vCPU time, boot phase; "desktop" assumes the codegen fix is done first (it changes the
base). Effort for someone who knows the tree.

| # | lever | desktop | phone | effort | risk |
|---|---|---|---|---|---|
| 0 | **Fix the helper's struct copy** (pass `TCGTBCPUState` by pointer to `tb_lookup`/`tb_htable_lookup`, or restructure so no 16-byte reload overlaps the narrow stores; also fixes the same pattern in `cpu_exec_loop`'s inlined `tb_lookup`) | up to 25-30 % [inferred; measured by the `shr` line vanishing] | 0 | 0.5-1 d incl. measurement | none (pure codegen) |
| 1 | **`msr`/`cps` block ends and hflags**: end with `DISAS_UPDATE_NOCHAIN` (lookup, no main-loop exit) when the immediate/`cps` cannot clear I/F or change mode (`cpsid`, `msr cpsr_c, #0xd3`-style, any SPSR write, flag-only writes); in `HELPER(cpsr_write)` rebuild hflags only when `mask & (CPSR_M|CPSR_E|CPSR_IL|CPSR_T)` and the bits actually changed | 3-6 % [inferred: main loop 3.95 % + rebuild chain 3.5 %, a good part from these] | 2-4 % | 1 d | low: the exit must stay whenever I/F may be cleared (pending IRQ delivery); hflags: keep the rebuild for mode/T/E/IL |
| 2 | **Return stack with key copy** (section 3B; prior art: Dynarmic's RSB, Box64 `DYNAREC_CALLRET` — both "push on call, compare on return, fall back on mismatch" [inferred, from memory]; nothing upstream in QEMU, FEX has no RSB [inferred]) | 5-10 % | 8-15 % [inferred: returns are ~half the lookups; a hit avoids the hash, both far misses and most of the helper] | 3-5 d | medium: invalidation (TBs, pages, ASIDs); switchable |
| 3 | **PGO** (`-Db_pgo=generate`, train on a boot, `use`) + `-march=native`/`-mtune=znver5` on the desktop; PGO on the phone needs an instrumented .so writing `.profraw` to app storage (`LLVM_PROFILE_FILE`), `adb pull`, `llvm-profdata merge`, `-fprofile-use` | 5-10 % | 3-6 % | 1 d desktop, 2 d phone | low; PGO will not remove lever 0's stall by itself (it may change the copy by luck; measure, don't rely on it) |
| 4 | **Jump-cache geometry and hash**: fold pc bits 24-31 into the hash (e.g. `tmp = pc ^ (pc >> 5) ^ (pc >> 19)`), then sweep 12/14/16 bits; 2-way only if conflict misses dominate the counter (step 0 of section 3); **key copy** in the entry (tc_ptr + cs_base + flags + cflags, no TB deref: removes the 20 % line on the phone) | 2-4 % | 4-8 % [inferred: hash path 11.5 % × part, TB line 4 %] | 1-2 d | key copy needs the generation check for parked caches (3B) |
| 5 | **Patch clean-ups**: make `asid_ctx_jmp_clear_page` lazy (record ≤ 8 flushed pages per context, apply on restore, full flush on overflow); skip `tlb_reset_dirty`/`tlb_flush_domains` work on contexts without entries | 1-2 % | 2-3 % | 1-2 d | low |
| 6 | **Android link flags**: `-fno-plt -fvisibility=hidden -Wl,-Bsymbolic` (0.94 % `@plt` is intra-.so calls that LTO could not bind because symbols are exported); `-ftls-model=initial-exec` for the `__thread` variables (`tlsdesc_resolver_dynamic` 1.1 % + `get_ptr_rcu_reader` 0.5 %); `--disable-stack-protector` (`-fstack-protector-strong` is on in both builds: canary load + compare in every helper) | <1 % | 2-3 % | 0.5 d | initial-exec TLS in a `dlopen`ed library needs bionic's static-TLS surplus to fit QEMU's few `__thread` objects (rcu reader, tcg_ctx, current_cpu; small) [inferred]; if `dlopen` refuses, fall back to `-fvisibility=hidden` only |
| 7 | `arm_get_tb_cpu_state` (3 % phone): an ARM-specific inline of the A32 path in the helper (pc, hflags, thumb, condexec, VFP enable) instead of the generic indirect call | 1 % | 1-2 % | 0.5 d | low |

Not levers (checked):
- `one-insn-per-tb` is off by default [verified: `one_insn_per_tb` is tested in the helper and is 0];
  `tb-size`/`code_gen_buffer`: translation (`tcg_gen_code` 1.15 % phone, below 0.4 % desktop) is cheap,
  so the buffer is not being flushed repeatedly [inferred]; confirm with `info jit` "TB flush count".
- Cross-page `goto_tb` chaining: upstream requires same-page because a chained target on another page
  bypasses the TLB; with ROM demand paging (the same VA maps different physical pages over time) this
  is exactly the hazard. Unlinking chains on `tlb_flush_page` has no per-VA index to work from. No.
- `TB_JMP_CACHE_BITS` alone: more bits do not fix the 24-bit hash or the per-invalidation flushes.

## 3. Design sketches

### 3A. The helper's struct copy (desktop)

Files: `accel/tcg/cpu-exec.c` (`tb_lookup`, `tb_htable_lookup`, `HELPER(lookup_tb_ptr)`,
`cpu_exec_loop`), `accel/tcg/internal-common.h` (prototype). Change `tb_htable_lookup(CPUState *,
TCGTBCPUState)` and `tb_lookup(CPUState *, TCGTBCPUState)` to take `const TCGTBCPUState *`; in the
helper keep `s` as a local and pass `&s` (the x86-64 ABI then passes a pointer; GCC no longer
hoists a vector copy). If GCC still emits a wide reload, split the key into scalars before the probe
(`vaddr pc = s.pc; uint32_t flags = s.flags; ...`) and build the struct only on the miss path with
`__attribute__((noinline))` on `tb_htable_lookup`. Measure: `perf annotate` (the `shr` line gone),
`perf stat -e ls_bad_status2.stli_other` before/after, boot phase-3 time. Switch: none needed, it is
a pure codegen change; keep it as `patches/qemu-modern/0006-...`. Also check `cpu_exec_loop`'s
hot `xchg` neighbour after the change (its own 89 % is the barrier, not this).

### 3B. Return stack with key copy (both hosts), switchable `SYBORG_RET_STACK=0`

Idea: on BL/BLX record a *call-site record* for the return address; on a return-shaped
indirect branch (`bx lr`, `pop {pc}`, `ldm {pc}`) compare the stack top with the target and jump
to the recorded host code without the hash, without touching the jump cache or the TB.

Data (new header `accel/tcg/ret-stack.h`):
```
typedef struct RetSite {            /* one per translated BL/BLX, allocated at translation */
    vaddr pc;                       /* return address (lr value), with the Thumb bit stripped as regs[15] has it */
    uint64_t cs_base; uint32_t flags; uint32_t cflags;   /* key of the TB after the BL: the caller TB's, recorded at translation */
    const void *tc_ptr;             /* host entry of that TB, filled on first return; NULL = unresolved */
    uint32_t inval_gen, page_gen, asid;                  /* validity stamps (below) */
} RetSite;
typedef struct CPURetStack { uint32_t top; RetSite *e[32]; } CPURetStack;   /* ring, in CPUState */
```
Sites live in a simple arena (`g_malloc`ed slab, bump allocator, reset in `do_tb_flush` together
with the TBs; when full, BLs are translated without a push until the next flush).

Translation (`target/arm/tcg/translate.c`):
- `trans_BL`, `trans_BLX_i`, `trans_BLX_r` (Thumb BL decodes to the same `trans_BL`): after
  `gen_pc_plus_diff(cpu_R[14])`, allocate a `RetSite` with `pc = pc_next` (constant for the translator
  even with CF_PCREL: `s->base.pc_next`; store it with the Thumb bit cleared, since `gen_bx` writes
  `regs[15]` with bit 0 cleared), key = `s->base.tb->{cs_base,flags,cflags}`, `tc_ptr = NULL`; emit
  inline TCG: `ld_i32 top; st_ptr site -> env.ret_stack.e[top]; top = (top+1) & 31; st_i32 top`
  (~6 host instructions, 2 stores, no helper call).
- `gen_bx` callers that are returns: `trans_BX` with `rm == 14`, `do_ldm` with pc in the list,
  `store_reg(15, ..)` from `mov pc, lr`-type ALU ops: set a new `s->base.is_jmp = DISAS_JUMP_RET`
  (or a bool in `DisasContext` consulted in `arm_tr_tb_stop`) so the block ends with
  `gen_helper_lookup_tb_ptr_ret(ptr, tcg_env)` + `goto_ptr` instead of the generic helper. All other
  `DISAS_JUMP` blocks keep `helper_lookup_tb_ptr`.

Runtime (`accel/tcg/cpu-exec.c`): `HELPER(lookup_tb_ptr_ret)(env)`:
1. `s = get_tb_cpu_state(cpu)`, `s.cflags = curr_cflags(cpu)`, `check_for_breakpoints` as today.
2. Look at the top 4 ring entries (newest first): the first with `site->pc == s.pc` is the match;
   pop the ring down to it (handles frames skipped by `leave`/`longjmp`-style unwinding and
   Symbian's `TRAP`). No match: leave the ring untouched (an unmatched return from an interrupt
   handler or a thread switch must not eat frames) and fall through to `tb_lookup` as today.
3. On a match: hit if `site->tc_ptr && site->cs_base == s.cs_base && site->flags == s.flags &&
   site->cflags == s.cflags && site->inval_gen == tb_inval_gen && site->page_gen ==
   page_gen[hash(s.pc >> TARGET_PAGE_BITS) & 255] && site->asid == cur_asid_key`. Everything is in
   1-2 L1 lines. Else `tb = tb_lookup(cpu, &s)`; if found, fill `site` (tc_ptr, stamps) and return
   `tb->tc.ptr`.
4. Counters (`rs_push`, `rs_hit`, `rs_miss_nomatch`, `rs_miss_key`, `rs_miss_stale`), printed in
   `info jit` (`accel/tcg/monitor.c`) and at exit when `SYBORG_RET_STACK_STATS=1`.

Invalidation (the part to get right; all rules mirror what the jump cache already does):
- TB invalidated (`tb_phys_invalidate`, `tb-maint.c:934-949`): `tb_inval_gen++`. Upstream already
  flushes the whole jump cache there under CF_PCREL, so stale sites are no worse than today; one
  global counter, no scan. If `info jit` shows a high invalidation rate, the alternative is to record
  `tb` in the site and check `tb_cflags(tb) & CF_INVALID` on every hit, which brings the TB far load
  back; prefer the counter and measure the rate first.
- Code cache flush (`do_tb_flush`): reset the arena, clear every ring, `tb_inval_gen++`.
- Page invalidation (`tb_jmp_cache_clear_page`, called from `tlb_flush_page_*` for `addr` and
  `addr - PAGE`, and our `asid_ctx_jmp_clear_page`): `page_gen[hash(page) & 255]++`. Sites on other
  pages survive (1/256 collateral). This covers the mapping-change hazard the same way the jump
  cache's page clear does.
- Full TLB flush / `tcg_flush_jmp_cache` callers (`tlb_flush`, CPU reset, DACR code-domain drop in
  0005, snapshot load): `tb_inval_gen++` and clear the ring.
- ASID switch (0005 parks jump caches): sites are shared by all address spaces (TBs are physically
  keyed; a site's `tc_ptr` is only known to be right for the VA in the ASID that filled it), so the
  site carries the `(secure, ASID)` key 0005 uses and a hit requires equality; a kernel call site
  reached from several processes refills once per switch (one ordinary lookup), not per return. The
  ring is per address space: park/restore it with the jump cache (32 pointers) or simply clear it on
  a switch (cheaper, costs one miss per live frame after the switch).
- Mode changes keyed in `flags` (Thumb bit, CPSR mode, condexec, VFP enable) and `cs_base` are
  covered by the key compare; `cflags` covers CF_PARALLEL/CF_NOIRQ/icount variants.
- gdb: `check_for_breakpoints` stays in front; a breakpoint on the return site sets CF_BP_PAGE in
  `s.cflags`, which mismatches the recorded key, so the fast path is skipped [verified: cflags is
  part of the key].

Measurement: boot phase-3 time (docs/perf.md) with `SYBORG_RET_STACK=0/1`, `perf report` helper
share (`helper_lookup_tb_ptr` + `helper_lookup_tb_ptr_ret`), the counters (target: hit rate ≥ 80 %
of returns, stale misses ≪ hits), and the same on the phone (`simpleperf`). Risks: a wrong stamp =
executing stale code (symptom: random guest crashes after code paging) — run the smoke test with
`SYBORG_RET_STACK=1` on the N8 (ROM paging) and the 808, and a `sydb catch` session on code page-in
heavy phases.

Variant for later: inline the top-of-ring compare into generated code (Dynarmic style: compare the
site pointer's `pc` and stamps in TCG ops, `goto_ptr` to `site->tc_ptr`), saving the helper call
(~15-20 host cycles) on hits. Do it only after the helper version shows the hit rate; it adds TCG
code at every return and complicates the key compare (flags/cs_base become loads in generated code).

### 3C. Measurement patch to do first (both hosts, half a day)

Counters in `tb_lookup`: hits, misses with `tb == NULL` (cold), misses with `pc != s.pc`
(conflict), misses with key mismatch (mode/cflags); jump-cache flushes and page clears
(by caller); print in `info jit`. Plus `info jit`'s existing "TB invalidate count" and "TB flush
count". These decide lever 4's shape (hash vs ways vs size) and whether 3B's global
`tb_inval_gen` is acceptable.

## 4. Android build notes

- `@plt` 0.94 %: QEMU is built without `-fvisibility=hidden` (only glib has it) so every exported
  function called inside `libqemu-syborg.so` goes through the PLT despite LTO. `-fvisibility=hidden`
  (+ an export list for the JNI entry points) or `-Wl,-Bsymbolic` + `-fno-plt`. [verified share,
  flags from `build/android/qemu-build/build.ninja`]
- `tlsdesc_resolver_dynamic` 1.1 % + `get_ptr_rcu_reader` 0.5 %: `__thread` accesses in a
  `dlopen`ed library use the dynamic TLS descriptor resolver. `-ftls-model=initial-exec` makes them a
  TPIDR-relative load; bionic honours it for `dlopen`ed libraries while the static TLS surplus has
  room (QEMU's TLS footprint is a few dozen bytes) [inferred]. If `System.loadLibrary` fails with a
  TLS error, keep local-dynamic (`-fvisibility=hidden` already lets clang use the cheaper
  local-dynamic sequence for hidden TLS symbols).
- `-fstack-protector-strong` is on (QEMU's meson default): a canary load+compare in every helper,
  including the lookup helper. `--disable-stack-protector` on both builds; well under 1 %.
- `-march=armv8.2-a` is right for LSE; nothing in the helper benefits from newer extensions.
- Thermal/clock: the X3 core share is 98.5 % vCPU; the same lookup rate costs ~2x more cycles than
  on the Ryzen because the far misses are DRAM-bound on the phone (L3 8 MB shared vs 32 MB). Levers
  that remove far loads (2, 4) pay more on the phone; lever 0 pays only on the desktop.

## 5. Claims

[verified]: all instruction-level shares and the disassembly facts above; `tb_lookup`/`tb_htable_lookup`
by-value signatures; the 24-bit jump-cache hash; CF_PCREL unconditional in system mode; full
jump-cache flush per `tb_phys_invalidate` under CF_PCREL; same-page rule for `goto_tb`; `gen_bx`
→ `DISAS_JUMP` → lookup helper; `msr`/`cps` → `gen_lookup_tb` → `DISAS_EXIT` → `exit_tb(NULL, 0)`;
unconditional `arm_rebuild_hflags` in `HELPER(cpsr_write)`; 0005's `asid_ctx_jmp_clear_page` walks
16 parked caches per page flush; Android compile flags; Zen 4 STLF-failure cost ~19 cycles
(chipsandcheese).
[inferred]: that the desktop `shr` pile-up is the STLF failure (+ memory-renaming flush); the
15-25 % miss rate; all gain percentages; the return share of lookups (~half); Dynarmic/Box64 details;
bionic initial-exec TLS acceptance; that the code buffer is not being flushed repeatedly.

Sources: [AMD's Zen 4, Part 2: Memory Subsystem and Conclusion (chipsandcheese)](https://chipsandcheese.com/p/amds-zen-4-part-2-memory-subsystem-and-conclusion),
[Load-store conflicts (zeux.io)](https://zeux.io/2025/05/03/load-store-conflicts/).
