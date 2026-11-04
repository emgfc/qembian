# syborg-pg — EKA2 on the Syborg virtual platform

A reproducible environment that builds the open-source Symbian **EKA2 kernel** (Symbian
Foundation `kernelhwsrv`, branch `GCC_SURGE`, E32 build 3098) and the **Syborg** board
support package with GCC, packs a text-shell ROM, and boots it in the Symbian-modified
**QEMU 0.9.1** ("Symbian Virtual Platform").

The longer-term goal is running original Nokia N95 8GB (Symbian 9.2 / S60 3rd FP1) userspace.
The current plan is in [docs/roadmap.md](docs/roadmap.md): first our kernel with a 9.2 exec personality
running the N95 user side unmodified, then full OMAP2420 emulation.

## Status (2026-11-04)

| Milestone | Status | Evidence |
|---|---|---|
| M1 environment | done | `scripts/build-all.sh` rebuilds everything from pinned, checksummed inputs in about 3.5 min (input downloads not included): [clean-build.log](docs/evidence/clean-build.log) |
| M2 QEMU | done | Symbian QEMU builds; the prebuilt PDK 4.0a Syborg ROM boots to ESHELL: [screenshot](docs/evidence/pdk-fixture-eshell.png) |
| M3 own ROM | done (udeb) | A ROM of our own GCC-built kernel, BSP and file server boots to an interactive ESHELL ([screenshot](docs/evidence/clean-build-eshell-dir.png)) and starts processes ([btrace](docs/evidence/own-udeb-btrace.png)). The serial kernel trace is in [evidence](docs/evidence/own-udeb-trace0x80000003-serial-head.txt). |
| M4 N95 assessment | firmware inventoried | [docs/n95-firmware-inventory.md](docs/n95-firmware-inventory.md) (RM-320 core ROM extracted, 9.2 exec table recovered, XIP import census) and [docs/n95-compatibility.md](docs/n95-compatibility.md) (options; recommendation) |

| E1 option 1 (N95 user side on our kernel) | in progress | Nokia's file server, EStart, WSERV, FBSERV, ECOM, Starter and DosServer (21 processes) run unmodified on our kernel with the 9.2 exec personality and stand-ins for Nokia hardware drivers; next blocker: modem/system state for Starter ([build-log](docs/build-log.md#option-1-e1-our-kernel--92-exec-personality--n95-user-side)) |

The `urel` variant has not been built yet. SDL window mode (`run.sh --gui`) has not been
tried on a desktop session. All other features have been run as documented below.

## Requirements

- Linux x86_64 host with **podman** (rootless works; tested with 6.1.2). Docker should also
  work: run with `ENGINE=docker`, which has not been tested.
- `git`, `curl`, `sha256sum`, about 8 GB of free disk (the downloads are about 2 GB).
- Optional: a multi-arch `gdb` on the host, for kernel debugging.

All legacy tooling runs in a **Debian 6 "squeeze" i386** container built from
`container/Containerfile`. The host needs no 32-bit libraries.

## Quick start

```sh
scripts/build-all.sh          # fetch -> container -> QEMU -> host tools -> kernel -> BSP+ROM -> boot test
```

Results:
- ROM: `build/epocroot/epoc32/rom/syborg_tshell_ARMV5_udeb.img` (+ `.symbol`, ROMBUILD log in `epoc32/rom/syborg/`)
- Boot test: `build/boot-test/syborg_tshell_ARMV5_udeb/{serial0.log,screen-*.png}`

## Individual steps

| Script | What it does |
|---|---|
| `scripts/fetch.sh` | Clones the pinned git revisions into `external/` and downloads the pinned archives into `downloads/`. Every pin is in `sources.lock` and every download is sha256-verified. |
| `scripts/setup-epocroot.sh` | Assembles `build/epocroot`: the PDK epoc32 headers and tools, the LinuxBuild package, and copies of the kernel and baseport sources. Unpacks the GCC toolchain, patches its libgcc ([why](docs/build-log.md#5-libgcc-lacks-the-arm-eabi-integer-helpers)) and extracts the PDK fixture ROMs. |
| `scripts/build-qemu.sh` | Builds QEMU 0.9.1-symbian, its Python device plugins and `libnvmemmory.so` into `build/qemu/install`. |
| `scripts/build-hosttools.sh` | Builds Raptor (sbs 2.15.2) and the Linux host tools (elf2e32, rcomp, rombuild, ...). |
| `scripts/build-kernel.sh [udeb\|urel]` | Builds `ksrt_gcce` and then the whole `kernelhwsrv` package (`arm.v5.<var>.gcce4_4_1.surge`). |
| `scripts/build-syborg.sh [udeb\|urel]` | Builds the Syborg BSP and, through its ROM extension, the text-shell ROM. |
| `scripts/build-rom-trace.sh [var] [mask]` | Rebuilds the ROM with a different `kerneltrace` mask, for example `0x80000003` (KBOOT and KHARDWARE). |
| `scripts/hybrid-boot.sh [--no-build] [--secs N] [--trace MASK]` | Option 1 loop: rebuild our kernel side, compose it with the N95 user side and ROFS (`tools/romcompose.py`, 9.2 exec personality on), boot headless on a 240×320 board (`SYBORG_DTB`) and summarise the serial log. |
| `scripts/build-n95compat.sh` | Builds `src/n95compat` (stand-ins for N95 kernel drivers: ISC, Custom, DisplayDriver, ROFS media, `lcd_n95`) and the hybrid's kernel-side ROM. |
| `N95_FW_DIR=… scripts/n95-inventory.sh` | Read-only analysis of an N95 8GB firmware set: container split, core-ROM unpaging, file list, EUSER SWI table, import census. Output goes to `build/n95/`. No firmware bytes are committed. |

## Running

```sh
scripts/run.sh --fixture --timeout 30          # prebuilt PDK ROM; serial0 on stdout, headless
scripts/run.sh                                 # our ROM, headless (the shell is on the framebuffer, so use boot-test or --gui)
scripts/boot-test.sh --secs 60 --type 'dir z:\sys\bin'   # Xvfb + screenshots + typed commands
scripts/run.sh --gui                           # SDL window via host X11 (not yet tried)
scripts/run.sh --gdb                           # gdbstub on 127.0.0.1:1234, CPU halted at reset
scripts/run.sh --gdb-nowait IMG                # gdbstub without halting (attach/detach to sample the PC)
```

- **serial0** is the kernel debug port (`Kern::Printf`, `RDebug::Print`). ESHELL draws on the
  framebuffer through EWSRV, so the shell shows up in screenshots or the SDL window, not on serial.
- The Syborg keymap is UK layout. `boot-test.sh` sends `\` as the ISO 102nd key.
- Debugging: `gdb -ex 'set architecture arm' -ex 'target remote 127.0.0.1:1234'`. The ROM
  symbol file gives addresses, for example `hbreak *0x80003130` for EKERN `_E32Startup`.
  This has been tried and works.

## Layout

```
sources.lock          pinned git revisions + download URLs/sha256
container/            Containerfile (squeeze i386) + uname shim
scripts/              everything above; libgcc-shim/ for the toolchain fixup
tools/                analysis helpers (DSO/DEF ordinal diff, exec-table numbering, BB5 FIASCO split,
                      paged-ROM unpacker, XIP ROM reader, SWI-table recovery, XIP import census,
                      XIP disassembler, ROM composer, DTB editor); tools/exec92/ = 9.2 exec map
src/n95compat/        kernel-side stand-ins for N95 hardware drivers (option 1)
rom/hybrid/           board configuration overrides for the hybrid ROM
patches/              source patches applied by setup-epocroot.sh (kernel, Syborg baseport)
docs/                 research, build log, architecture, N95 compatibility, evidence/
external/ downloads/ cache/ build/    generated, git-ignored
```

## Documentation

- [docs/research.md](docs/research.md): sources, branches, toolchains, dependency graph, what is and isn't available
- [docs/build-log.md](docs/build-log.md): every build problem hit, its cause and its fix
- [docs/architecture.md](docs/architecture.md): virtual board, ROM, boot sequence, build pipeline
- [docs/roadmap.md](docs/roadmap.md): decided plan, option 1 (9.2 exec personality) then option A (OMAP2420 emulation), with step exit criteria
- [docs/n95-firmware-inventory.md](docs/n95-firmware-inventory.md): what is in the RM-320 firmware, measured
- [docs/n95-compatibility.md](docs/n95-compatibility.md): feasibility of running N95 userspace, approach comparison, next experiment

## Licensing notes

Symbian Foundation sources are EPL-1.0. QEMU is GPL-2.0. The GCC helper sources are
GPL-3.0 with the GCC Runtime Library Exception. CodeSourcery Lite binaries are GPL and fetched
from the Wayback Machine. The PDK is EPL-1.0 apart from some binary-only parts, and is fetched
from archive.org. Nothing large or proprietary is committed here.
