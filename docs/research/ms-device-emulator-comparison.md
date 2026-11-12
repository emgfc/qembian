# Microsoft Device Emulator (WinCE ARM JIT) vs qembian: what to borrow

Static read of the Microsoft Device Emulator source release (2006 MS-PL source; reference only, no code copied): ARM->x86
JIT (`cpus/arm/armcpu.cpp`), software MMU with 64-entry I/D TLBs (`cpus/arm/mmu.cpp`), one translation cache + red-black
entrypoint tree (`cpus/tc.cpp`, `cpus/arm/entrypt.cpp`), peripherals by physical address (`boards/mappedio.cpp`).

## 1. MMU / TLB / code cache: we are ahead; one cheap idea (batching cache-op loops) worth a look
- Translation: every jitted load/store `CALL`s `MMU::MapGuestVirtualToHost<Access>()` (`armcpu.cpp:2407-2411`,
  `mmu.cpp:808-870`): a linear scan of 64 `TLBRegister`s from a per-instruction slot hint stored in the code.
  No inline fast path; QEMU's inline softmmu probe is far cheaper.
- Context switch: WinCE uses FCSE. The PID is OR-ed into every VA below 32 MB (`mmu.h:145`, `mmu.cpp:194,820`), so
  TLB entries and entrypoints are keyed by the relocated VA and a PID write flushes nothing (`mmu.cpp:722-737`).
  Symbian 9.2/ARMv6 uses ASIDs, so this does not transfer; our parked per-ASID contexts
  (`patches/qemu-modern/0005-arm-tlb-contexts-domains-4k-pages.patch`, `docs/perf.md`) are the ASID equivalent.
- Domains: hard-wired `DACR == 1` (`mmu.cpp:122-124,443-447`); any PTE with domain != 0 faults, DACR writes are
  stored and ignored. Symbian's alias-domain lock would break here. Our selective drop (0005/0009) is more general.
  TLB ops: full flush per unit for `c8`, linear `FlushOneTLB` (`mmu.cpp:636-690,188-215`): QEMU semantics, worse constants.
- Code cache: no self-modifying-code detection. The whole translation cache + entrypoint tree is dropped on every
  guest I-cache invalidate, MMU enable, or cache-full (`mmu.cpp:485-563`, `tc.cpp:387-426`; range flushes only skip
  ranges with no entrypoints, `entrypt.cpp:375`). Clever bit: the `ArmFlushICacheLines` loop is pattern-matched and
  collapsed into one flush (`armcpu.cpp:6626-6690`). QEMU's per-page dirty-code protection is finer and survives
  remaps. Not present there: per-process caches, lazy invalidation, physical-keyed blocks.

## 2. Guest RAM / sharing: we are ahead

RAM is a static host array (`boards/smdk2410/board.cpp:64-69,239-246`, range check + offset). The LCD thread polls
the framebuffer by physical address at ~12 fps, copies it word by word into a DIB and diffs to decide on repaint
(`devices.cpp:4116-4147`); audio DMA hands raw host pointers into the guest ring to the IIS thread (`:4320-4340`).
No dirty tracking, nothing shared with the GPU or between devices. Our dirty-page `syborg_fb`, host-GPU `syborg_gl`
and the bounds-checked ring in `syborg_audio.c:180-183` are better.

## 3. Host calls: same MMIO pattern as ours, with worse safety; nothing cheaper to learn
- All host services are MMIO register devices, no SWI/coprocessor trap: FolderSharing (parameter-block physical
  address, opcode, pending, result; `boards/FolderSharing.cpp:627-740`), EmulServ (host->guest event bits + IRQ,
  `boards/EmulServ.cpp:1-30`), DMATransport (Visual Studio link: packets written word by word through MMIO into
  device buffers, `boards/cominterface.cpp:720-850`).
- Pointer safety: the parameter block is mapped by physical address and checked only for `fStructureSize`
  (`FolderSharing.cpp:749-770`); `AsyncServerRead/Write` map `fDTAPtr` and call `ReadFile(hFile, lpvBuffer,
  pServerPB->fSize, ...)` with no check of `fSize` against RAM end (`FolderSharing.cpp:1367-1379`; the 64K max is
  only reported to the guest, `:97,1746`): a guest can overflow host memory. Our `gmem_rw`/`user_rw`
  (`src/qemu-syborg/hw/arm/syborg_gl.c:1020-1110`) walk page by page through the TLB with the caller's MMU index and
  refuse device pages; they never attempt guest-virtual access (WinCE pins buffers for them).
- Their completion poll parks the CPU thread in `WaitForMultipleObjects` (`FolderSharing.cpp:785-815`): IRQ + WFI for us.

## 4. Other ideas; what is worse

Borrowable: (a) idle-loop detection (`LDR/CMP/Bcc` spin on a flag -> block on an event, `armcpu.cpp:6165-6192,3469`);
QEMU parks on WFI, but if Symbian code spins on memory a TCG-level detector would help. (b) Call-return shadow stack
and per-site last-target cache for indirect branches (`docs/JIT.html` "Indirect call/jump", `ShadowStack[256]`):
TCG has only the jump cache; a per-site cache is a known gap but a deep change. (c) Collapsing cache-maintenance
loops into one op, like our `0009-arm-code-domains-at-fetch` does for domain flushes.
Clearly worse: function call + linear TLB per access; whole-cache flush per I-cache op; DACR ignored; no
guest-virtual host access; unbounded guest sizes; polled 12 fps framebuffer copy; no FIQ.

## Ranked list

1. Count Symbian's per-line `c7` cache ops and spin loops during boot (perf.md method): low effort; batch the
   CP15 helper path (0009 style) if the count is high. Expected: small boot-time win.
2. Spin-loop detector in TCG: low-medium effort, impact only if step 1 shows spinning; otherwise skip.
3. Per-site indirect-branch target cache in TCG: high effort, maybe 5-10% in IPC-heavy code; defer.
4. Nothing to take for ASID/TLB, RAM sharing or host calls; keep 0005-0009 and the page-walked `user_rw`.