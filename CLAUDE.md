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
- The user's phones are device instances (`tools/instance.py list`; `build/instances/<name>/`:
  instance.toml + drives, snapshots, logs; docs/sydb.md "Which phone"). For the user:
  `python3 tools/sydb/sydb.py phone NAME [--fresh]` is ONE command (boots, opens the window, ends the
  session when the window goes away); never hand over start + phone.
- Agents never use the user's instances: `sydb --session build/sydb-<name> start` (`--seed
  build/seeds/<id>` = a phone past first boot, `--fresh` = factory-fresh; `--gui` shows the window) or
  an ephemeral instance (`tools/instance.py create NAME DEVICE --ephemeral --replace`). A bare `sydb`
  (no `--instance`/`--session`) is the primary instance of `$SYBORG_DEVICE`: the user's phone. A
  session refuses another phone's drives; `sydb stop` ends it.
- `sydb dbg ...` is the persistent gdb: hw breakpoints (`dbg cmd 'condition N $r0 == -4'`),
  bt/regs/mem/eval, app ELF symbols from `SYBCODE:` lines. `sydb strace MATCH` traces IPC.
- `sydb log --grep RE` / `events` read the session's serial log (`<session>/serial.log`).
- Crashes and races: `sydb catch add ...` (freeze/dump/snapshot when a thread dies as described,
  call stack included), `sydb snap save` / `sydb fork SNAP` (resume a saved machine as a separate
  session, again and again). Prefer this to breakpoints on hot paths, which change the timing.
- `tools/syb_run.py` (scripted boots with timed keys and a report) and `tools/syb_gdb.py` are still
  right for one-shot boot experiments and ROM composition (`docs/tools.md`). They kill only their
  own `sybtool` QEMUs, so they don't stop a sydb session.

## Debugging method

Before investigating a crash, hang, restart, black screen or a new firmware, load the project skill
`symbian-debugging` (`.claude/skills/symbian-debugging/SKILL.md`): triage order (deaths, unanswered
modem requests, who is blocked on whom), catches, trace builds, gdb with kernel symbols, reading ROM
and ROFS code, and the traps that gave wrong conclusions before.

## Devices (declarative device model)

One QEMU binary serves every phone; everything phone-specific is declared in
`models/<model>/model.toml` + `models/<model>/firmware/<fw>/firmware.toml` (device model v2,
`models/README.md`) (+ `keymap.toml`, `isi-replies.txt`, `code-patches.txt`, `ESTARTCOMP.TXT`, `gl/`).
Read `docs/devices.md` (schema, layering) before touching `models/`, `src/standins/`,
`personalities/` or the run scripts; `docs/adding-a-device.md` is the flow for a new phone or firmware
(`tools/fw/identify.py`, `tools/fw/store.py import`, `tools/device.py firmware-add`).
- No new N95 literal in a script or tool: put it in the profile and read it through `tools/device.py`
  (`list`, `firmwares`, `check`, `show`, `get`, `file`, `paths`, `qemu-args`, `identify`, `build`, `gen`).
  Device: `$SYBORG_DEVICE`, else the instance's (`$SYBORG_INSTANCE`), else `models/registry.toml`
  `default`; ids are a model, `MODEL@FW` or a firmware's own id (`nokia-n8-rm596-v13`).
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
- Smoke test after every platform change: `tests/device-smoke.sh [ID]` (throw-away `smoke` instance;
  idle, Carousel, Switch off), unit tests `tests/device_py_test.py`.
- Flash files are in the ROM store `roms/` (git-ignored), extractions in `build/firmware/`, each device's
  build dir in `build/roms/<id>/<fw key>/` (`tools/device.py paths`: never spell these paths in a
  script), the user's phones in `build/instances/`. Never boot the user's own drives for a test (an
  instance in `tools/instance.py list`, or `run-gui.sh`, which boots one): clone it
  (`tools/instance.py clone NAME NAME-test`) or use a `--session build/sydb-<name>` session.

## N-Gage QD (EKA1): a separate board

The original N-Gage firmware (Symbian 6.1, EKA1 kernel) runs on its own QEMU board `ngage`
(`src/qemu-ngage/`, built by `scripts/build-qemu-ngage.sh` into `build/qemu-ngage/`), outside
the device model and sydb. Start with `docs/eka1-ngage.md` (state, running, debugging, next);
evidence is in `docs/eka1-ngage-questions.md`. Window: `python3 eka1/tools/ngage_phone.py`;
debug sessions: `eka1/tools/ngage_sess.py` (own sessions only, never `phone-*` unasked).

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
