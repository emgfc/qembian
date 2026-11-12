# Hosted EKA2: options and the recommended design

## 1. What Symbian's Win32 emulator kernel replaced [verified: kernel/eka in build/epocroot]

| Part | Portable (kept) | ARM target (replaced) | Win32 host variant |
|---|---|---|---|
| nanokernel | `nkern/*.cpp` 4,052 lines (scheduler policy, fast mutexes, DFCs, timers) | `nkern/arm/*` 7,477 | `nkern/win32/*` 2,176 |
| kernel | `kernel/*.cpp` 40,402 (objects, threads, processes, IPC, code segments, properties, HAL, loader side) | `kernel/arm/*` 14,123 | `kernel/win32/*` 1,425 |
| memory model | - | multiple 18,919 / flexible 29,749 | `memmodel/emul/*` 4,340 |
| user library | `euser/*.cpp` | `euser/epoc/arm/*` 8,178 (17 `svc` stubs + generated exec stubs) | `euser/epoc/win32/*` 5,521 |

How the Win32 variant works (`nkern/win32/ncsched.cpp`, `ncthrd.cpp`, `memmodel/emul/win32/*`):
- Every Symbian thread is a Win32 thread created `CREATE_SUSPENDED`; exactly one is "scheduled" at a
  time. The EKA2 scheduler picks the next thread, `SetEvent`s its `iScheduleLock`, and the previous one
  `WaitForSingleObject`s on its own. Idle = wait on the event. The EKA2 scheduler keeps full authority.
- "Interrupts" are a separate Win32 thread (`Win32Interrupt`, `StartOfInterrupt`/`EndOfInterrupt`)
  that `SuspendThread`s the scheduled thread, runs the ISR/DFC queueing, and `ResumeThread`s.
- All Symbian processes live in one Win32 address space. A process switch calls `iProcessHandler`
  which swaps DLL writable static data in and out (`DWin32CodeSeg::CopyDataBss`, `iDataCopy`): the
  "moving model" done with `memcpy`. Chunks are `VirtualAlloc` reservations; code segments are Win32
  DLLs loaded with `LoadLibrary`.
- Exec calls are direct calls: `euser/epoc/win32/uc_exec.cpp` fetches the kernel's dispatcher through
  `GetProcAddress(kernel, 1)` and calls it with the exec number (no trap at all).
- Exceptions use SEH (`NThread::ExceptionFilter`) to deliver Symbian exceptions and panics.

So the hosted kernel is ~8,000 lines of host glue around ~44,000 lines of unchanged kernel. Our Linux
variant has the same shape, but three problems the Win32 emulator did not have (section 3).

## 2. Options

| | (a) hosted EKA2 in Linux user space | (b) EKA2 at EL1 under KVM | (c) native EKA2 port | (d) QEMU TCG full system (today) |
|---|---|---|---|---|
| ROM code execution | native AArch32 EL0 | native AArch32 EL1/EL0 in a VM | native | TCG JIT (1.7x desktop CPU per clock on an X3) |
| Pixel 3a | yes (needs AArch32 EL0: present) | **no** (no EL2, no `/dev/kvm`) | needs the firmware to enter an AArch32 EL1 kernel [inferred, unknown] + a full SDM670 BSP | yes (TCG; APK or Termux) |
| QEMU virt | yes | yes, but it is (d) with another board | n/a | yes |
| Reuse: personalities (exec maps, shims) | yes (numbering tables; the ARM shims in `cexecpers.cia` rewritten) | all | all | all |
| Reuse: stand-ins (`src/standins`) | kernel-side ones compile in (fake modem, security, lights, ROFS media); MMIO-backed ones (LCD, sound, GL) need host back ends | all | none of the board side | all |
| Reuse: device model, ROM composer | ROM composer yes (patches `svc` sites too), device model as data | all | partly | all |
| Reuse: sydb | new transport (ptrace/gdbserver per process, in-kernel trace socket); catches, strace by hooking the hosted kernel | all | serial only | all |
| Fidelity | no MMU protection of kernel data, no ASIDs/domains, host signal timing | full | full | full |
| Effort | 4-9 months to a phone home screen | blocked | 6-12+ months (symbian-hd2 is years in and has no audio/network) | done |
| Speed | native (exec call cost decides) | native | native | 2-3x slower than the S23 on an A75 [inferred] |

(b) note: on `virt`, QEMU already lets our existing kernel run at EL1 with `-cpu max` and AArch32
EL1; that is a second board port with no gain over `syborg`. On a different phone whose firmware boots
Linux at EL2 (no Qualcomm hypervisor: some MediaTek, Rockchip, the Pixel 6+ Tensor with pKVM [inferred])
(b) becomes an interesting middle road: our unmodified kernel + a small VMM. Not for the 3a.

## 3. Recommended design for (a)

### 3.1 Address spaces: one Linux process per Symbian process

Fixed addresses in the multiple model (`mmboot.h` [verified]): local data `0x00400000-`, shared
chunks `0x20000000-0x80000000`, ROM `0x80000000-0x90000000`, DLL/global data `0x90000000-0xA0000000`,
kernel data `0xC8000000-0xC9200000`. The flexible model (N8+): local `0x00400000-0x7F000000`, IPC alias
`0x7F000000`, ROM `0x80000000`, kernel section `0x90000000+`. All of it is below the compat limit
`0xFFFFF000` [verified], so a 32-bit process can `mmap(MAP_FIXED)` every one of them.

The ROM's XIP binaries put *process-local* data (EXE statics, ROM DLL data) at the same fixed address
in every process. In one Linux process that is a collision; the Win32 kernel solves it by copying
static data on every process switch (moving model). With real address spaces it is free:

- The "nucleus" starts one 32-bit static launcher per Symbian process (`ADDR_NO_RANDOMIZE`; its own
  text linked at a spare range, e.g. `0xA0000000` RAM-drive area, which no phone ROM uses [inferred]).
- Mapped identically everywhere: the ROM image (`MAP_PRIVATE`, `PROT_READ|EXEC`, from the composed
  hybrid image file, whose kernel-side files are ignored), the hosted kernel's text (same ELF, same
  address), the kernel's data + heap as one `memfd` at `0xC8000000` (shared, writable), global chunks
  (one `memfd` each, mapped at their fixed address in every process that has opened them).
- Per process: local chunks (`memfd` each; a chunk commit = `mmap` of a further range of the file,
  decommit = `MADV_REMOVE` + `PROT_NONE`), thread stacks (chunks as in EKA2), DLL data copies.
- Kernel code runs on the calling thread inside the calling process (as in EKA2 and Win32). Kernel
  state is shared memory, so a thread in process B sees what A's exec changed. Locks: the nanokernel's
  scheduler lock and fast mutexes become futexes in the shared kernel data (process-shared futexes work
  on shared mappings).
- Creating a thread in another process: each launcher has an agent thread parked on a futex; the
  kernel posts "create thread T", the agent `pthread_create`s it parked. Killing a thread in another
  process: `tgkill` with a signal whose handler runs the exit path (Win32 used `SetThreadContext`).

What a Linux process boundary gives that Win32 did not: real isolation between Symbian processes,
fixed addresses with no data swapping, `fork`-free start-up. What it costs: kernel data is writable by
every Symbian process (as in the Win32 emulator), and a kernel-side bug anywhere corrupts all.

### 3.2 Executive calls

The ROM's EUSER makes exec calls with `svc #n` (fast: bit 23 set) in `svc #n; bx lr` stubs [verified:
`tools/swi_map.py`, `u32exec.h`]; the ARM kernel decodes the immediate from `[lr, #-4]`
(`nkern/arm/vectors.cia:149`). On Linux a compat `svc` is a Linux syscall numbered by r7 (random here),
so every one must be intercepted before the host kernel acts on it. Three layers, cheapest wins:

1. **Patched stubs (the normal path).** The composer already patches ROM code (`code-patches.txt`);
   it rewrites every `svc #n` in EUSER (and the one in `uc_realx`) to `b trampoline_n`. A trampoline
   page mapped within +-32 MB of the stubs (just past the ROM, or in the ROM's spare) does
   `push {lr}; mov ip, #n; bl dispatcher; pop {lr}; b site+4` [inferred: the N8's 100+ MB ROM may need
   a second page]. The dispatcher is the hosted kernel's exec table (the personality's `exec.map`
   generated as today by `gen_exec_map.py`), called with r0-r3. Cost: a function call. This matters
   because `User::Heap()` is fast exec F01 [verified: exec.map], i.e. every allocation is an exec.
2. **seccomp `SIGSYS` (safety net).** A BPF filter: `arch == AUDIT_ARCH_ARM && instruction_pointer`
   inside any Symbian code range -> `SECCOMP_RET_TRAP`; the handler reads the `svc` immediate at pc-4,
   dispatches, writes r0 (and r1-r3 for execs returning more) into the `ucontext`, returns. ~2-4 us on
   a phone [inferred]; counts and reports unpatched sites so layer 1 can grow. The hosted kernel's own
   Linux syscalls (from the 64-bit? no: from 32-bit libc) come from non-Symbian code pages and pass.
3. **Never:** ptrace (two context switches per exec) or `SECCOMP_RET_USER_NOTIF` (a round trip to a
   supervisor process; the exec would run outside the caller's address space). A kernel module would
   avoid the signal but is pointless once layer 1 exists.

Fast execs (`Heap`, `TrapHandler`, `ActiveScheduler`, `LockPeriod`, ...) read the current thread's
fields with "interrupts disabled"; hosted they read `TheCurrentThread` (a libc `__thread` variable or
`TPIDRURO`-based TLS, which Linux owns) under the nanokernel lock. `swp` sites in EUSER (request
completion, `RFastLock`) run through Linux's SWP emulation (`abi.swp = 1`, an undef trap each) or are
patched to a trampoline that does `ldrex/strex` [inferred]. ARMv6K EUSERs read `TPIDRURW` for their
thread data (N8, `docs/devices/nokia-n8-rm596/bring-up.md` [verified]); it is user-writable and
per-task on Linux [inferred], so the hosted thread start writes it once with what our ARM1176 kernel
build writes per switch.

### 3.3 Threads, scheduling, interrupts, time

- One Linux thread per Symbian thread, parked on a futex in its `NThread`; the EKA2 scheduler
  (`nkern/*.cpp`, unchanged) decides who runs; `nkern/linux/ncsched.cpp` is `ncsched.cpp` of Win32
  with events replaced by futexes and `SuspendThread` by a signal whose handler parks the thread.
  One running Symbian thread at a time at first (exactly the Win32 model); SMP-style concurrency is a
  later option the nkernsmp sources would allow.
- Interrupt sources: a timer thread (`timerfd`, 1 ms tick as `NTimerQ::Init1 - period 1000 us`),
  the host input/display/audio threads, the fake modem's own timers (already DFC-based). Each raises
  "interrupt" through the Win32-style `StartOfInterrupt`/`EndOfInterrupt` pair; DFC threads are
  ordinary Symbian threads (unchanged `nkern` code).
- Exceptions: `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` handler -> the kernel's exception dispatch (user
  exception handler via the `ucontext`, else `KERN-EXEC 3` panic path). `User::Exit`/thread kill ->
  the kernel's exit path then `pthread_exit`.

### 3.4 Memory model (`memmodel/emul/linux`, from `memmodel/emul/win32`)

- Chunks: `memfd_create` + `mmap(MAP_FIXED)`; global chunks mapped at the same address in every opener
  (agent thread in each process); `RChunk::Commit`/`Decommit` on the file; `MAP_SHARED` between
  processes gives shared chunks and the `KIPCAlias` for free.
- IPC descriptor copies (`DThread::DesRead/DesWrite`, `Kern::ThreadRawRead`): map the target chunk's
  `memfd` at a scratch range in the current process (cached per chunk, the multiple model's IPC alias
  done with files) or `process_vm_readv` (needs ptrace permission: same uid, `PR_SET_PTRACER`).
- Code segments: ROM ones are XIP as today; RAM-loaded ones (ROFS, `C:`) get a `memfd` at the fixed
  global code address (multiple: `iUserCodeBase` computed at boot from ROM header fields [verified:
  `xmmu.cpp:804-814`]), written by the loader thread (F32's loader, user side, through the normal
  `E32Loader` execs), then mapped RX into every process that loads the DLL. Writable static data of
  RAM-loaded DLLs: per-process chunk at the fixed DLL data address [inferred]. This replaces Win32's
  `LoadLibrary` and is the largest new piece (the ARM memmodel's `mcodeseg.cpp` is the model).
- Demand paging: off (as in our ROMs today).

### 3.5 Drivers and host back ends

- Compile in unchanged: fake ISI modem, security, lights, ROFS media (reads the ROFS image with
  `pread`), ECOMM/ELOCD style LDDs. Local media: a hosted PDD over the drive image files (today's
  `nvmemory` images), so F32, formatting and data caging stay as they are.
- New host back ends: LCD stand-in -> DRM/KMS (phone) or SDL2 window (PC); keys/touch <- evdev;
  sound -> ALSA; host GPU path for N8/808 -> the guest-side marshalling stand-in calls host EGL/GLES in
  the same process (no copies; today's `syborg_gl.c` logic re-hosted; needs Mesa freedreno on the 3a).
- sydb: `log/events` from the kernel's trace socket; `catch` and `strace` as hooks in the hosted
  kernel (`sipc.cpp` trace points exist); `dbg` = gdbserver attached to a Symbian Linux process with the
  ROM symbols (`SYBCODE:` lines as today); `snap` = CRIU of the process group [inferred, late].

### 3.6 Which source survives

Unchanged: `nkern/*.cpp`, `kernel/*.cpp` (incl. `sexecpers.cpp`, the personalities' tables and
shims written in C++), `drivers/` we use, our stand-ins' logic, `euser` of the phone (the ROM).
Rewritten: `nkern/linux` (~2,500 lines), `kernel/linux` (~1,500: exec dispatch, exceptions, boot),
`memmodel/emul/linux` (~5,000-7,000: chunks, codeseg loader, processes, IPC alias), the ARM `.cia`
shims of the personalities (`cexecpers.cia`), host device back ends (~3,000-5,000), the nucleus
launcher and composer changes (~1,500). Build: `arm-linux-gnueabihf-g++` (AAPCS, same Itanium C++
ABI as GCCE; exceptions/RTTI off in the kernel anyway) [inferred: no toolchain test done].
