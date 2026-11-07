# qembian: notes for Claude sessions and subagents

qembian: Nokia S60 phone user sides (first and most tested: N95 8GB, S60 3rd FP1, Symbian 9.2),
unmodified, on our EKA2 kernel with stand-in drivers and a fake modem, on our own Syborg board in
QEMU 11.1.3. Start with `docs/tools.md`
(debugging tools), `docs/hacks.md` (every workaround), `docs/roadmap.md` (status and queue).

## Live phone: use sydb first

For anything on a running phone (install/run/kill apps, files, processes, window groups, keys,
screenshots, keyguard, P&S, IPC strace, gdb), use the sydb bridge (`docs/sydb.md`), not
hand-made QEMU/gdb command lines:
- MCP tools `mcp__sydb__*` (server in `.mcp.json`), or the CLI `tools/sydb/sydb.py` (`--help`).
- `sydb start` boots a session from `build/sydb/drives` (`--seed build/runs/mtc-drives` = clean
  phone; `--gui` shows the window); `sydb stop` ends it.
- `sydb dbg ...` is the persistent gdb: hw breakpoints (`dbg cmd 'condition N $r0 == -4'`),
  bt/regs/mem/eval, app ELF symbols from `SYBCODE:` lines. `sydb strace MATCH` traces IPC.
- `sydb log --grep RE` / `events` read the session's serial log; logs live in `build/sydb/`.
- Crashes and races: `sydb catch add ...` (freeze/dump/snapshot when a thread dies as described,
  call stack included), `sydb snap save` / `sydb fork SNAP` (resume a saved machine as a separate
  session, again and again). Prefer this to breakpoints on hot paths, which change the timing.
- `tools/syb_run.py` (scripted boots with timed keys and a report) and `tools/syb_gdb.py` are still
  right for one-shot boot experiments and ROM composition (`docs/tools.md`). They kill only their
  own `sybtool` QEMUs, so they don't stop a sydb session.

## Devices (declarative device model)

One QEMU binary serves every phone; everything phone-specific is declared in
`devices/<id>/device.toml` (+ `keymap.toml`, `isi-db.txt`, `code-patches.txt`, `ESTARTCOMP.TXT`,
`gl/`). Read `docs/devices.md` (schema, layering) before touching `devices/`, `src/standins/`,
`personalities/` or the run scripts; `docs/adding-a-device.md` is the flow for a new phone.
- No new N95 literal in a script or tool: put it in the profile and read it through `tools/device.py`
  (`list`, `check`, `show`, `get`, `paths`, `qemu-args`, `identify`, `build`, `gen`). Default device:
  `devices/default` (symlink) or `$SYBORG_DEVICE`.
- Compose a device ROM with `tools/device.py build [ID] [--variant V] [--kernel]` (hybrid-boot.sh /
  compose-exp.sh are wrappers). It refuses firmware whose `[firmware.identity]` (sw.txt, image hashes,
  hardware drivers) does not match; `tools/device.py identify DIR` tells which profile a firmware is.
- Families (`family = "bb5-s60"` -> `platform/families/bb5-s60.toml`, merged under the profile) and
  `platform/isi/replies-bb5.txt` hold only what is true of every member for a reason (BB5 modem, S60
  keys, our board/kernel set-up). Never move a value there because the devices so far happen to
  agree (screen, hardware, drives, stand-ins). When a new device differs from a family value, move
  that value out of the family into each member's profile in the same change; do not override it
  in one device and keep a false "base". Rules: `docs/devices.md` "Families".
- A stand-in that must speak another phone's protocol variant gets it declared
  (`[[standins]] config = { key = value }` -> `BoardConfig("<name>.<key>")`), never guessed from
  the arguments a client passes.
- Stand-ins: `src/standins/<driver>/` (shared headers in `_shared/`); the profile's `[[standins]]` picks
  them (bld.inf and ROM order generated). Keys: `keymap.toml` -> EKEYB table, sydb aliases.
- Personalities (`personalities/sym92/`: exec map, kernel patches, `kabi.toml`) make our EPL kernel look
  like the phone's own; verify with `tools/personality/check.py --device ID`.
- Smoke test after every platform change: `tests/device-smoke.sh [ID]` (own session; idle, Carousel,
  Switch off), unit tests `tests/device_py_test.py`.
- Firmware and the user's own phone images stay in `build/devices/<id>/` (git-ignored). Never boot the
  user's personal drives (`run-gui.sh` defaults) for a test: use `sydb --session build/sydb-<name>` sessions.

## Subagent research rules

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
