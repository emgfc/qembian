# Hosted EKA2: future designs (speed, processes, SMP, CPUs without AArch32)

State of the design today and why: `src/hosted/README.md`. Tags: [verified] = read in the code or
measured, [inferred] = reasoning or a guess not yet checked. Nothing here is implemented unless it says so.

## 1. Measure first

Start-up (EikSrvs starts) takes 5.6 s hosted vs 4.2 s on the ARM QEMU board; the process is on the CPU
only ~25% of the time [verified, roadmap.md]. Before any rewrite, split the other 75%:
- wake-up latency: timestamp in `TLxEvent::Set`, read when `Wait` returns, a histogram per boot;
- interrupts delivered: count `LxSuspend` calls, split tick thread / IRQ thread;
- process switches and bytes copied (`DoProcessSwitch`, `mutils.cpp`).
If latency dominates, section 2 recovers most of it cheaply. If idle dominates, the floor is the N95's own
timers and the ARM board has it too.

## 2. Cheap speed steps (hours to 2 days each) [inferred gains]

1. **One CPU for the whole Symbian machine.** Pin nanothreads, tick and IRQ threads to one core
   (`sched_setaffinity` in `LxThreadStart`); first experiment: `htest --smp 1`. A hand-over then becomes
   a same-CPU switch instead of a cross-CPU wake (IPI, and under MTTCG a blocked host thread to wake).
2. **Private futexes while single-process** (a flag in `hrt.h`). Undo it with the process split (section 4).
3. **Longer tick suppression / tickless idle.** Today a real tick interrupt comes at least every 8 ms
   (`KMaxSkip = 7`, nkhost.cpp) and idle sleeps in 1 ms steps; skip while the ready thread is alone,
   cap at 64 ms, sleep the whole idle time at once.
4. **Device interrupts on the calling thread.** When a nanothread's register write raises an interrupt
   while it holds the interrupt lock (device models take it now: hsnd/hmodem/hkeyb), run the ISR inline
   instead of waking the IRQ thread to suspend that same thread. Every ISR must be checked for
   `NKern::CurrentContext() == EInterrupt` assumptions; tests in the style of `kn_devlock`.
5. **Process-switch copies**: copy only DLL data that changed (dirty tracking), if the counter shows it.

## 3. All nanothreads on one host thread (5-10 days)

The ARM kernel's own model: a context switch saves registers and swaps stacks, no host scheduler
involved. `SwitchThreads` (ncsched.cpp) becomes a register/stack swap; `LxCreateThread` an mmap of a stack.
Interrupts: helper threads set a pending flag and `tgkill` the one thread; its handler switches only
when the interrupt lock is free, else marks pending and `Restore(0)` delivers (exactly "interrupts
disabled" on ARM). Removes all futex hand-overs (~27% of CPU samples) and suspend signals.
Costs: a signal must never run inside the hosted kernel's non-reentrant code (heap lock, hprintf):
defer it; diversions edit the saved frame instead of a parked ucontext; dumps and the watchdog walk
saved contexts; gdb sees one thread (sydb `dbg` needs its own thread list); a blocking host call on a
nanothread stalls the machine (none today). It conflicts with SMP (section 5), which wants host threads
per CPU: do it only if section 2 leaves hand-overs dominant.

## 4. One Linux process per Symbian process

The original design (`architecture.md` 3.1): each Symbian process is a 32-bit Linux process with its
own address space; the ROM, the hosted kernel's text, the kernel data and heap (one memfd at
0xC8000000) and global chunks are mapped at the same addresses in all of them.
- No static-data copying on a process switch (each process has its own 0x00400000 and DLL data).
- Real isolation between Symbian processes (a stray write in one app no longer corrupts another).
- Kernel code runs on the calling thread in the calling process, on shared kernel state.
- **This is why the futexes are process-shared today**: the scheduler lock, fast mutexes, thread
  schedule events and the interrupt lock live in the shared kernel data, and a wait in process A must be
  woken by process B. Linux keys a private futex by (mm, address), so a private wake from another
  process never reaches the waiter; a shared futex is keyed by the backing page and works across
  processes [verified: futex(2)]. Its price is the slower key lookup on every call.
- Thread creation in another process: an agent thread per process, parked on a futex, creates threads
  on request; kill = `tgkill` with a signal whose handler runs the exit path.
- IPC copies: the target chunk's memfd mapped at a scratch range (cached), or `process_vm_readv`.

## 5. SMP

Symbian has an SMP nanokernel (`nkernsmp`, used from Symbian^3 on multi-core phones). Hosted SMP means:
- `nkernsmp` sources instead of `nkern`, with a Linux platform layer like `nkern/linux` [inferred: the
  Win32 tree has no SMP variant to port; nkernsa's `nirqx.cpp` (SMP interrupt controller) is the
  test-side reference].
- One host thread per Symbian CPU ("vCPU") running nanothreads, or keep one host thread per nanothread
  and let up to N of them run at once. The second is close to today's design: the hand-over becomes
  per-CPU, and real concurrency means the kernel's spinlocks, atomics and memory barriers do real work.
- Shared futexes again: the per-CPU schedulers and the cross-CPU IPIs (generic IPI, reschedule IPI) are
  futex wakes on shared kernel data, also across processes once section 4 exists.
- Device models already take a lock (a spinlock with sched_yield, taken with interrupts disabled by
  nanothreads); on SMP they need per-device locks without the global interrupt lock, as QEMU's
  device locks [inferred].
- Interrupts routed per CPU (a GIC-like distributor in hboard), timers per CPU.
- Gain: the S60 5th/Symbian^3 phones were single core; SMP matters for Belle-era multi-core firmware
  and for using several host cores at once [inferred: little gain for the N95, which is single core].

## 6. CPUs without AArch32: translation (JIT and ahead-of-time)

Hosting needs the host CPU to execute 32-bit ARM. Phone cores from about 2022 (Cortex-X3/A715 on, Pixel
7+), Apple silicon and x86-64 cannot. For them the ROM's ARM/Thumb code must be translated. Design
sketch [inferred throughout]:

**Kernel.** The hosted kernel is compiled natively for the host (arm64 or x86-64) but keeps the 32-bit
data layouts the user side shares with it (descriptors, `TRequestStatus`, message structures, exec
argument blocks). Two options: compile it ILP32-style (pointers 32 bits wide; x86-64 has the x32 ABI,
arm64 ILP32 is deprecated) or keep the kernel 64-bit and reach guest memory through a base register
(`guest_base + addr`, every user pointer a 32-bit offset). The second is the portable one; the
`MM::CurrentAddress`/`RawRead`/`RawWrite` layer is where guest pointers are already resolved today.

**Guest address space.** Reserve 4 GB of host address space and map the Symbian layout inside it at the
same offsets as today (ROM at +0x80000000, EXE data at +0x00400000, ...). Translated code adds the base;
fixed Symbian addresses keep working without relocation.

**JIT** (dynarmic-style, per basic block, ARMv5/v6 + Thumb + VFPv2): the baseline that runs anything,
including code loaded at run time (ROFS/C: DLLs, installed SIS apps). Exec calls become direct calls into
the native kernel (the `svc` decoder sees the immediate at translation time: no trampolines, no trap).
Preemption: translated blocks check a pending-interrupt flag at block ends or loop back-edges, which
also makes the single-host-thread design (section 3) natural. Reference: EKA2L1 runs Symbian user code
through dynarmic [inferred]; dynarmic is 0BSD-licensed [inferred: check before use]; GPL code stays a
naming reference only (project rule).

**Ahead-of-time recompilation** of the ROM, the ROFS images and installed binaries, before boot: the
part that makes it fast on a phone.
- The ROM is XIP at fixed addresses and never changes for a firmware: translate every module once,
  cache the native code on disk keyed by the image hash (as Android's ART or Rosetta 2 do), load it with
  the ROM. RAM-loaded E32 images have relocation tables and a code/data split: their translation is
  position-independent and cached per image hash too.
- What the image formats give the translator: code section bounds, the export table (entry points),
  the import table (call targets in other modules), the exception descriptor; EUSER's exec stub block
  (`romscan.py` already finds it) becomes direct kernel calls.
- What static translation cannot know: indirect branch targets (vtables, function pointers, switch
  tables), ARM vs Thumb at a target (interworking via `bx`), data inside code (literal pools). Answer: a
  guest-pc -> native-code lookup for indirect branches, and fall back to the JIT for any target not
  translated ahead of time; record JIT-found entry points and add them to the next AOT pass.
- Services and the whole user side (the servers that start at every boot: EStart, the window server,
  FbServ, ECom, the S60 UI servers) are the first AOT targets: they are where boot time goes.
- Correctness reference: the same ROM on the ARM QEMU board and on the AArch32-capable hosted kernel
  (exec traces call for call, as kn_efile was checked).

**Order:** after the AArch32 hosted path is complete on the Pixel 3a (the current priority), first the
JIT (it runs everything), then AOT for the ROM and the boot-time services, measured against the JIT.

## 7. Other hosts and devices

- The Pixel 3a: the same process on mainline Linux (pmOS sdm670 tree), DRM/KMS display, evdev keys,
  ALSA sound (roadmap.md P6).
- GPU phones (N8, 701, 808): the host EGL/GLES path of the QEMU board (`syborg_gl.c`) re-hosted in the
  process, Mesa freedreno on the 3a.
- Snapshots of a hosted machine: CRIU of the process (group), with the shared-memory display
  re-attached [inferred, late].
