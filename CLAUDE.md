# syborg-pg: notes for Claude sessions and subagents

Nokia N95 8GB (S60 3rd FP1, Symbian 9.2) user side, unmodified, on our EKA2 kernel with stand-in
drivers and a fake modem, on our own Syborg board in QEMU 11.1.3. Start with `docs/tools.md`
(debugging tools), `docs/hacks.md` (every workaround), `docs/roadmap.md` (status and queue).

## Live phone: use sydb first

For anything on a running phone (install/run/kill apps, files, processes, window groups, keys,
screenshots, keyguard, P&S, IPC strace, gdb), use the sydb bridge (`docs/sydb.md`), not
hand-made QEMU/gdb command lines:
- MCP tools `mcp__sydb__*` (server in `.mcp.json`), or the CLI `tools/sydb/sydb.py` (`--help`).
- `sydb start` boots a session from `build/sydb/drives` (`--seed build/runs/mtc-drives` = clean
  phone; `--gui` shows the window); `sydb stop` ends it.
- `sydb dbg ...` is the persistent gdb: hw breakpoints (`dbg cmd 'condition N $r0 == -4'`),
  bt/regs/mem/eval, app ELF symbols from `N95CODE:` lines. `sydb strace MATCH` traces IPC.
- `sydb log --grep RE` / `events` read the session's serial log; logs live in `build/sydb/`.
- `tools/syb_run.py` (scripted boots with timed keys and a report) and `tools/syb_gdb.py` are still
  right for one-shot boot experiments and ROM composition (`docs/tools.md`). They kill only their
  own `sybtool` QEMUs, so they don't stop a sydb session.

## Subagent research rules (owner's instruction)

Research agents deliver **just enough API/protocol knowledge to mock a driver or system**, never a
full reverse-engineering of it.
- Deliverable: per control/message, give the number, the argument layout, what must come back
  (values and handle kinds) and the accepted results. Add proposed stand-in code, plus a short doc
  that marks each claim [verified] or [inferred].
- Budget: about 30–40 tool calls. When it is reached, stop and report what you have.
- If something is unclear, mark it [inferred], give a best guess and move on. Don't dig.
- If the subsystem needs heavy emulation, stop early. Report in 3–4 lines with the cheapest
  alternative.
- Out of scope: internals not needed for the mock, naming everything, neighbouring subsystems.
- Don't run QEMU (`tools/syb_run.py`, `tools/syb_gdb.py`, `scripts/run-modern.sh`, `sydb start`)
  unless the brief says so. `syb_gdb` kills every running qemu, so runs collide with the main session.

## Project rules

- Don't commit proprietary binaries, firmware bytes or firmware screenshots
  (keep them in `build/evidence`).
- oFono/phonet and other GPL sources are naming references only; copy no code.
- Local commits only, no push.
- Log missing tools in `docs/tooling-ideas.md` after long debugging loops.
