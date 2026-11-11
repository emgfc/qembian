# qembian

qembian runs the original user side of Nokia S60 phones (Symbian 9.x) on an open-source EKA2
kernel inside QEMU. The phone's own ROM supplies the applications, servers and libraries. qembian
supplies the kernel, the board, and replacements for the phone's hardware drivers. The goal is
to develop and debug Symbian applications in the real firmware environment of a given phone,
with a debugger, symbols and tracing, instead of on a device or in a high-level emulator.

## Read this first

This is an early, unpolished research project, and it is written mainly for AI coding agents.
Building it, adding a phone and debugging on it are designed to be done by an agent (an LLM
with shell access) that reads the documents in `docs/` and `CLAUDE.md`. On-device debugging is
exposed as an MCP server (`sydb`) for that purpose. Setting it up by hand is possible, but the
instructions are not written for people following them step by step. Some human-oriented notes
are given below on a best-effort basis.

If you want to play old Symbian games or run applications, this is not the right tool. Use
[EKA2L1](https://github.com/EKA2L1/EKA2L1), which is a ready-to-use emulator. qembian is for
developers who need the phone's real ROM.

No firmware is included. You need your own copy of the phone's flash files (see "Firmware").

## How it works

**Hybrid ROM.** A phone ROM holds a kernel side (the EKA2 kernel, memory model, hardware drivers)
and a user side (file server, window server, every DLL and application). qembian builds its own
kernel side from the Symbian Foundation's open-source kernel (`kernelhwsrv`) and the Syborg
board support package, and composes it with the phone's user side into one ROM image. The phone's
user side stays byte-for-byte at its original addresses, so every import and DLL reference in it
remains valid (`tools/romcompose.py`).

**Personalities.** The open-source kernel is not the one the phone shipped with. Symbian 9.1 and
9.2 numbered their kernel calls differently and laid out some structures differently. A
personality (`personalities/sym91`, `personalities/sym92`) maps the phone's calls onto the kernel
and adapts what differs. The ROM header selects one at boot.

**Stand-in drivers.** The phone's kernel-side drivers drive real hardware (OMAP display, DSP,
modem link) that the emulated board does not have. They are replaced by stand-ins
(`src/standins/`) that answer the same interfaces as far as the user side needs: display, DSP
bridge for audio, a fake cellular modem (ISI protocol, canned replies), security, lights, and
ROFS media. A stand-in is written from the calls the phone's user-side clients make, at the level
needed to satisfy them.

**Debugging.** Since the kernel is ours, the platform can observe everything: gdb with symbols
for both our kernel and the phone's ROM, IPC tracing, catching a thread the moment it panics,
machine snapshots that can be resumed again and again, screenshots and key input. `sydb`
(`tools/sydb/`, `docs/sydb.md`) bundles this as a command-line tool and as an MCP server.

**Patches.** Everything we change in other projects is kept as patch files and applied at build
time, never as edited copies:

| Patch set | Applies to | Why |
|---|---|---|
| `patches/kernelhwsrv/platform/` | the open-source EKA2 kernel | build with modern toolchains, tracing, kernel personality support |
| `patches/kernelhwsrv/policy/` | the kernel | optional run-time policies (e.g. survive a critical user thread that exits) |
| `personalities/*/patches/` | the kernel | each Symbian version's call numbering and ABI differences |
| `patches/qemu-baseport/` | the Syborg board support package | phone-like CPU, RAM, timers, RTC, display modes, keyboard |
| `patches/qemu-modern/` | QEMU | CPU model details the phone firmware relies on |
| `patches/elf2e32_next/` | elf2e32_next | post-linking GCC 14 output |
| `models/<id>/code-patches.txt` | the composed phone ROM | small byte patches where a stand-in cannot help (each one documented, original bytes verified before patching) |

Every workaround is listed in `docs/hacks.md` and `models/<id>/hacks.md`.

**Devices.** Everything specific to a phone is declared in `models/<id>/`: `model.toml` (the
phone), one `firmware/<version>/firmware.toml` per firmware it runs, keymap, modem replies, drive
map, byte patches. One QEMU binary and one set of tools serve every phone. Your phones are device
instances (`tools/instance.py`: a name, a model and firmware, its own drives and snapshots), opened
with `tools/sydb/sydb.py phone NAME`. `docs/devices.md` describes the model, `docs/adding-a-device.md` the procedure.

## Status

- Nokia N95 8GB (RM-320, Symbian 9.2, S60 3rd Edition FP1) is the device that was actually
  tested: start-up, home screen, menus, built-in applications, installed applications, audio,
  OpenGL ES.
- Nokia N73 (RM-132, Symbian 9.1) reaches the home screen and its menu.
- Nokia 5730 XpressMusic (RM-465, Symbian 9.3) reaches the home screen and its menu.
- Nokia N79 (RM-348, Symbian 9.3) reaches the home screen and its menu.
- Nokia 5800 XpressMusic (RM-356, Symbian 9.4) reaches the home screen and its menu, with touch input.

Devices other than the N95 are first bring-up steps, used mainly to exercise the kernel
compatibility layer. Expect them to stop short of a usable phone.

**3D graphics.** OpenGL ES 1.1 is a first, simple implementation. It renders a limited set of
known applications correctly; correctness elsewhere is not guaranteed. There is a software
renderer and a host-GPU-accelerated one (`sydb start --gl soft|host`).

**Audio.** By default a session records the phone's audio to a WAV file. `sydb start --audio host`
plays it through PipeWire; `AUDIO=pa`, `alsa` or `sdl` selects another QEMU audio driver. This
works on a typical PipeWire desktop. Other audio setups are not supported.

**Host platform.** Linux on x86-64 (amd64) is the only tested host. Other systems and
architectures may work, but they are not tested or supported.

## Firmware

Put the flash files of your phone in a directory (for example `roms/<model>/`, which git ignores)
and point the inventory tool at it. The following original files (as distributed, before any
conversion) were used. Compare yours with `sha256sum`:

| Device | File | SHA-256 |
|---|---|---|
| N95 8GB RM-320, 35.0.001 | `RM-320_35.0.001_PR.C0R` | `018dc62b2378801d26aa86e76e019236c57ac756925dc00a99b354f5ecbccf95` |
| | `RM-320_35.0.001_PR.v13` | `9797555aa695f7424bd8fe5f1aaf918ea35da628f01bdaeef7f80f1073212a86` |
| N73 RM-132, 3.0717.1.4.1 | `N73_3.0717.1.4.1_western_C00_PRD_cc.fpsx` | `d708a376e57c0d42dbe83bec94a1c7119598ff13b0e630954314ab9712edad05` |
| | `N73_rofx_3.0717.1.4.1_PRD.V16` | `0efd5eb42072f8a00bf294c532777410fe64834b0adbf5bf602da15f23552fcf` |
| 5730 RM-465, 101.48.128 | `rm465_10148128_prd.c00` | `5f11fc66e5588a98bbf4fdf47f2b774ffaeea25137a0ee2a2f7fa20543e98967` |
| | `rm465_10148128_prd_prdapps.rofs2.v19` | `c2ff454a10b8d5675abbf1e57fa79fc511b509ad0e54b0d6ffd19b06ac493bb0` |
| | `rm465_10148128_prd_prdapps.rofs3.v19` | `2b5ea8f95f42cdb7b6e93e158c2e03c7eeb5add8f2e3378a57c194e3c1a723d8` |
| N79 RM-348, 30.019 | `RM-348_30.019_prd.core.fpsx` | `1445d909569751d0065001b903320ac303367053b89fc0fe5a5ff56dc2ce817b` |
| | `RM-348_30.019_005_prd.language.fpsx` | `df7efb7ba9c7505ac4eee7997cc2c098f6d313bf7daacec0bfd8e70116ef66a5` |
| | `RM-348_30.019_53.1_prd.customer.fpsx` | `b4d48439fa0934285f9f9031a7022c8de831bd895b8f2187b80127fe5daedaa7` |
| 5800 RM-356, V 31.0.101 | `RM-356_31.0.101_prd.core.C00` | `bffaa4b9fb0e727934aa1ffb55ed48f98403f767227f4c746dc27cfe5ea834fb` |
|  | `RM-356_31.0.101_prd.rofs2.V05` | `671054fe238ca5634b493f898ad1e415760c5b3b0fdc148c67af61c3fcb23812` |
|  | `RM-356_31.0.101_C01_prd.rofs3.fpsx` | `1ce5ca18b5f923b64d10318a52fc61999e338f525d781692ea5611fff7407c1e` |

`tools/fw/identify.py PATH` tells what a set of flash files (or an archive of them) is, and
`tools/fw/store.py import PATH` files them into the ROM store `roms/<type code>/<version>/` (git-ignored),
which records each file's sha256. `tools/device.py identify DIR` tells which profile a set of
extracted images belongs to.

## Building

Requirements: Linux amd64, podman (or Docker with `ENGINE=docker`), git, curl, Python 3.11 or
newer, about 15 GB of disk. All compilers run in two containers that the scripts build on first
use: `container/Containerfile` (Debian 6 i386: Symbian host tools, kernel, board support,
stand-ins) and `container/Containerfile.modern` (Debian 13: QEMU, GCC 14 for
`arm-none-symbianelf`, elf2e32_next).

```sh
scripts/fetch.sh                        # pinned sources and toolchains (sources.lock), checksummed
scripts/setup-epocroot.sh               # Symbian build tree, applies the kernel and board patches
scripts/build-hosttools.sh              # Symbian host tools (rombuild, elf2e32, ...)
scripts/build-kernel.sh                 # the EKA2 kernel
scripts/build-qemu-modern.sh --container   # QEMU with the Syborg board -> build/qemu-modern-ct/

# per phone (N95 8GB shown)
tools/fw/store.py import /path/to/RM-320-flash-files   # into roms/RM-320/35.0.001/
tools/fw/inventory.py --device nokia-n95-8gb-rm320     # extract (build/firmware/...)
tools/device.py build nokia-n95-8gb-rm320 --kernel     # board support, stand-ins, composed ROM

# your phone: an instance with its own drives, in a window
tools/instance.py create n95 nokia-n95-8gb-rm320
tools/sydb/sydb.py phone n95 --fresh
```

The container-built QEMU runs through `scripts/qemu-container.sh`, which the run scripts use
automatically when there is no host build. With the development packages listed in
`container/Containerfile.modern` installed on the host, `scripts/build-qemu-modern.sh` without
`--container` builds a native QEMU instead.

Guest code built with GCC 14 (the OpenGL ES library, the sydb on-phone agent) also needs the
Nokia S60 3rd Edition SDK, which cannot be redistributed. It is mounted at build time. Build these
before `tools/device.py build`, which puts the OpenGL ES library into the ROM when it exists:

```sh
S60SDK=/path/to/S60_3rd_FP1/Epoc32 CONTAINER=modern scripts/in-container.sh src/sygl/build.sh
S60SDK=/path/to/S60_3rd_FP1/Epoc32 CONTAINER=modern scripts/in-container.sh src/n95apps/devagent/build.sh
```

## For agents

Start with `CLAUDE.md`, then `docs/tools.md` (debugging tools), `docs/sydb.md` (the bridge and
its MCP server, configured in `.mcp.json`), `docs/devices.md` and `docs/adding-a-device.md`
(device model and how to add a phone), `docs/hacks.md` (every workaround) and `docs/roadmap.md`.

## Credits

- **Syborg**, the Symbian virtual platform: the QEMU board and devices were created by
  CodeSourcery, and the Syborg board support package by Nokia and its contributors, both released
  through the Symbian Foundation. qembian's board is a reimplementation of Syborg for current QEMU,
  and its kernel side is built on the Syborg baseport. This project would not exist without that
  work.
- **Symbian Foundation and Nokia** for releasing the EKA2 kernel and platform sources under the
  Eclipse Public License.
- **QEMU** and its contributors.
- **fedor4ever** for [elf2e32_next](https://github.com/fedor4ever/elf2e32_next), used as the
  post-linker for GCC 14 output, and for [GCC4Symbian](https://github.com/fedor4ever/GCC4Symbian),
  whose recipe showed how to build a current GCC for `arm-none-symbianelf`. The container applies
  equivalent source changes written independently.
- **EKA2L1** and its contributors, whose documentation of Symbian internals was a useful reference.

## License

qembian's own code is MIT licensed (`LICENSE`), except where a file or directory says otherwise:
changes to and code built into Eclipse Public License projects (the kernel and board patches,
personalities, stand-in drivers, the elf2e32_next patch) are under the EPL-1.0, and the QEMU board
code and QEMU patches are under the GPL-2.0-or-later, like QEMU. Third-party sources are fetched
at build time and keep their own licenses. Nokia firmware and SDKs are not part of this project.
