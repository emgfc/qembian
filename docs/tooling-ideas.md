# Tooling ideas

Missing tools that would have saved time, written down after long debugging loops. Reviewed
periodically by the project owner; implemented items move to "Done" with a pointer to the tool.

Format: one entry per idea: **what it is**, *which loop it would have shortened* (with a date), and
a rough cost.

## Open

- **Exec-call argument audit for the 9.2 personality**: list every 9.2 exec call whose argument
  types differ from ours (by value vs by reference, extra/missing args), by comparing the N95
  EUSER stubs' register set-up with our `execs.txt`. *2026-11-05: `TimerAt` passing `TTime&` cost
  the boot 140 s and was only found by stack-walking a spinning thread.* Cost: one day.
- **Undefined-instruction catcher**: report every UNDEF the guest takes with the instruction word
  and the module, so emulator decoding gaps (like the non-canonical `CLZ`) show up at once.
  *2026-11-05: the AknIconServer crash was first seen as a kernel fault with a confusing "far".*
  Cost: small (kernel trace in the undef handler or a QEMU TCG plugin).
- **DOS event log**: log every DosServer event raised (id, value, raising DSY) in the serial log, next
  to the `N95PROP:` lines, so the ISI reply → DSY → DOS event → sae key chain shows in one run.
  *2026-11-05: the "no SIM" fix needed reading NokiaDSY and sae by hand to see that our 0x1C meant
  "card present" and that SEC cause 0x16 is ignored.* Cost: small (a `syb_gdb brk` preset on
  DosSrv's raise function, or a kernel-side hook on the DosServer session).
- **KERN-EXEC call-site logger**: when the kernel panics a thread with KERN-EXEC (bad handle etc.),
  log the user PC/LR and the exec number (`NKern::ThreadGetUserContext` in `K::PanicCurrentThread`).
  *2026-11-05: MmRmServerThread's KERN-EXEC 0 after the DSP-bridge controls took a long static hunt
  for the call site.* Cost: small kernel patch + kernel package rebuild.
- **Fix `syb_gdb` kernel-symbol breakpoints**: a breakpoint on `K::PanicCurrentThread` resolved via
  `_syborg_ekern.exe.sym` (+0x80003000-0x8000) landed in SHA-256 code at run time, so the .sym file or
  the address formula does not match the ROM's kernel. Check which ELF the ROM's ekern comes from.
  *2026-11-05.* Cost: small.

## Done
- `tools/syb_run.py` + `tools/syb_report.py` (2026-11-05): one-command boot/screenshots/keys and a
  structured report incl. the "likely blocked" IPC wait list.
- `tools/syb_gdb.py` (2026-11-05): leave tracer with module filter, breakpoints with kernel
  expressions (kernel DWARF loaded), current-thread names, symbolised sampling.
- `tools/isi.py` + `src/n95compat/isi/isi-db.txt` (2026-11-05): ISI names, unanswered-request
  summary, reply table as data.

- `tools/syb_sym.py` (2026-11-05): address → module/export/function/literal, symbol search,
  callers of an export across all modules, string search with references, word dumps,
  per-function decompile.
