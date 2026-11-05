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

- **ISI reply-layout extractor**: for a client handler of a response, decode the ISIMSG field
  accessors it calls (`#165`/`#255`/... take a field id that maps to a fixed offset, `#303` finds a
  subblock) into "offset → accepted values", so a fake-modem reply can be written in one go.
  *2026-11-05: the RTC status reply `19 0B` took four boot-and-probe rounds because the field ids
  (0 → [10], 18 → sb+2) looked like raw offsets.* Cost: small (table of the ISIMSG accessor
  switch statements + a disassembly walk in `tools/isi.py`).

## Done
- `tools/syb_gdb.py` fixes (2026-11-05): stack reads clipped at the end of the supervisor stack (they
  failed silently, so `--stack` and kernel-symbol probes looked broken; the kernel .sym mapping was
  fine), user call site + 9.2 exec name at kernel-mode stops (replaces the "KERN-EXEC call-site
  logger" idea), probe address confirmation and gdb output on "no hits".
- `tools/syb_run.py` + `tools/syb_report.py` (2026-11-05): one-command boot/screenshots/keys and a
  structured report incl. the "likely blocked" IPC wait list.
- `tools/syb_gdb.py` (2026-11-05): leave tracer with module filter, breakpoints with kernel
  expressions (kernel DWARF loaded), current-thread names, symbolised sampling.
- `tools/isi.py` + `src/n95compat/isi/isi-db.txt` (2026-11-05): ISI names, unanswered-request
  summary, reply table as data.

- `tools/syb_sym.py` (2026-11-05): address → module/export/function/literal, symbol search,
  callers of an export across all modules, string search with references, word dumps,
  per-function decompile.
