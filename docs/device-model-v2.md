# Device model v2: models, firmware, device instances

Design document (2026-11-08) for the rework the owner asked for: separate *what a phone model is*
from *which firmware build is loaded* from *the user's own virtual phone*. Written before the work:
§6 says what was implemented (2026-11-10) and where it differs; docs/devices.md, docs/sydb.md and
models/README.md describe the result. Claims are marked [verified in source] (read in the repository or in
the firmware packages under `roms/`) or [inferred] (Nokia/Phoenix conventions from general knowledge,
or a proposal). Related: `docs/devices.md` (the current schema), `docs/adding-a-device.md`,
`docs/sydb.md`, `docs/device-matrix.md`, `docs/cleanup-review.md`.

## 1. Problem statement, goals, non-goals

### 1.1 What is wrong today [verified in source]

`devices/<id>/device.toml` mixes two things with different lifetimes:

| In one profile today | Belongs to | Changes when |
|---|---|---|
| `[device]`, `[platform]`, `[board]`, `[hardware]`, `[kernel]`, `[[standins]]`, `[modem]`, `keymap.toml`, `ESTARTCOMP.TXT`, `[gl]` identity strings, `[gl.host]`, `[milestones]`, `[props]`, `[[rom.rofs_patches]]`, `[[rom.rofs_replace]]`, `[[rom.file_edits]]`, `[rom] files` | the phone model (type code) and how we emulate it | a new stand-in, a new hack, a new phone |
| `[firmware] version`, `[[firmware.files]] glob`, `[firmware.identity]` (sw.txt values, image sha256), `firmware.sha256`, `[firmware.rom] user_window_start`, `code-patches.txt` (absolute ROM addresses), `gl/gles-patches.txt`, `composition/gcebe-patches.txt`, `[composition] blob_addr`, `[gl] blob_addr`, `symindex.json` | one firmware build (type code + software version + language pack + customer variant) | the user loads another version of the same phone |

Consequences seen in the repository:
- A second firmware of the same phone is a whole second device (`devices/nokia-n8-rm596-v13` with
  `extends = "nokia-n8-rm596"`), its own `build/devices/<id>/`, its own `firmware.sha256`, its own
  session directory, and it appears in `tools/device.py list` and in the coverage matrix as if it
  were another phone. The owner's note in that profile says only "files, identity, addresses"
  changed; the matrix says the same ("only addresses changed: EDISP user window,
  fmradioaudiocontrol -0x4390, gcebe imports").
- The profile pins one exact build through `[firmware.identity.sha256]` and `device.py build`
  refuses anything else. That is right for the address-bearing patches and wrong for everything
  else in the profile.
- `roms/` is a hand-named tree (`roms/RM-596_Nokia-N8/011.012/`, `roms/RM-348_Nokia-N79/10.046/`,
  `roms/_archives/RM-348 v.10.04 N-79.rar`); no tool reads it. The profile's `obtain` text tells a
  human to copy files somewhere and point `FW_DIR` at them; `tools/fw/inventory.py` is run by hand
  with `--fw-dir`, and the link between a ROM directory and a device is in nobody's head but the
  owner's. The `firmware/<id>/` directory the docs mention does not exist.
- `[[firmware.files]] glob` encodes the language pack choice in a file-name pattern
  (`RM-596_*_Euro_1_*_prd.rofs2.fpsx`) although the package's own `.vpl` names it.
- A session (`build/sydb-<id>/`) is the only thing that holds a user's drives; it is keyed by the
  profile id, so there is one phone per profile, and the owner's "real" phone is a special case
  (`sydb --device`, `run-gui.sh` defaults, `hybrid-exp.img`).

### 1.2 Goals (the owner's terms)

1. A table of known Nokia type codes (RM-xxx) that grows with our knowledge; one profile per phone
   model holding hardware facts, QEMU settings, stand-ins, patch sets. No ROM hashes in it.
2. ROM files or an archive are read the way Phoenix reads a data package: the files say what they
   are for (type code, version, language pack, product). We do not invent a naming scheme.
3. The user thinks in three things: a model, a firmware, and *their* device (model + firmware +
   disks + settings), created from a GUI ("create device"), then started, reset, explored.
4. Device instances are like VMs in Proxmox: several of the same model (three N95s) with names the
   user chooses, each with its own isolated drives, settings and firmware choice, and, when wanted,
   small overrides of the model profile (a GPU setting, a stand-in option). Instances are
   independent of each other; they can be created, cloned, renamed and deleted. Nothing more:
   no template hierarchy, no multi-user, no networking between instances.

### 1.3 Non-goals

- Flashing real phones, Phoenix/Navifirm compatibility beyond reading their files.
- Full FIASCO reverse-engineering (only what identifies a file and extracts partitions).
- Replacing sydb: the daemon, agent, CLI and MCP tools stay; the manager sits above them.
- Rewriting the ROM composer: `romcompose.py` keeps its format; a resolver feeds it.
- Supporting every language pack: one combination per firmware is composed at a time (more can be
  imported, each is a firmware of its own).

## 2. Entities

```
registry (repo)            models/<id>/ (repo)                 roms/ store (user data)
  type code -> model id     model.toml  hardware + emulation      <RM>/<sw>/<files> + rom.toml
                            keymap, ESTARTCOMP, isi-replies,      identity read from the files
                            patches/ (module-relative)            extracted images in build cache
                                   \                                  /
                                    \                                /
                              instances/<name>/ (user data) instance.toml
                              model + firmware + drives + settings + snaps + logs
                                           |
                              build/roms/<model>/<firmware>/hybrid.img  (cache, shared)
```

### 2.1 Model profile (`models/<id>/model.toml`)

One per phone model, keyed by type code; the directory keeps today's id form
(`nokia-n8-rm596`). A type-code variant with the same user side (RM-321 China N95 8GB, RM-133 N73
ME) is a model that `extends` the base and changes only what differs (today's rule, kept).
Everything in today's `device.toml` except the `[firmware]` table and the address-bearing files
moves here unchanged, so the schema of `docs/devices.md` §2.1 stays valid for these tables.

What changes:

| Today | v2 | Why |
|---|---|---|
| `[firmware] version`, `[[firmware.files]] glob`, `[firmware.identity]`, `checksums` | gone; replaced by `[firmware.accept]` (below) | identity is read from the ROM, not declared |
| `[firmware.rom] user_window_start` | derived at build time: the lowest address of the firmware's user-side files in `core.json` (today it is read off EDISP.DLL's header by hand: `probe.py header`, N8 profile comments) | per build, computable |
| `[firmware.rom] base/kernel_data/memory_model/paged`, `compression`, `partitions` | stay in the model: `[firmware.accept]` | facts of the model's firmware generation, same for every version |
| `code-patches.txt` absolute addresses | `patches/*.txt` module-relative (§2.2.3) | survive versions |
| `[gl] blob_addr`, `[composition] blob_addr` | `blob_place = "below-user-window"` + alignment; the address is computed from the derived user window | per build |
| `gl/gles-patches.txt`, `composition/gcebe-patches.txt` | not committed: generated into the build cache from `ordinals`/`imports` + the firmware's symbol index (already how they are made [verified in source: headers of both files]) | generated |
| `[status]`, `[milestones]` | stay; `[status]` grows a `firmware_tested = ["011.012", "013.016"]` list that tools update | coverage is per firmware |

New table `[firmware.accept]`: what a ROM must be to run on this model (replaces the glob/identity
pair):

```toml
[firmware.accept]
type_codes = ["RM-596"]                      # sw.txt / .vpl TypeDesignator; variants list their own
container = "fiasco-bb5"
platform = ["symbian3", "anna", "belle"]     # informational until a release-specific hack needs it
versions = ">= 011.012"                      # optional; refuse older builds we know do not work
roles = ["core", "rofs2", "rofs3"]           # required file roles (uda optional)
core.partitions = { core = "SOS+CORE", rofs1 = "SOS+ROFS1" }
rofs2.partitions = { rofs2 = "SOS+ROFS2" }
rofs3.partitions = { rofs3 = "SOS+ROFS3" }
rom = { base = 0x80000000, memory_model = "flexible", paged = true }
```

For the N73 the `core` entry carries `compression = "sos-deflate"` and `partitions = { core =
"SOS*CORE", rofs1-after-core = "SOS*CORE" }`, the `rofx` role `partitions = { rofs2 = "_pre" }`
(today's values [verified in source]).

Per-version overrides live in the model too, but only when needed:

```toml
# models/nokia-n8-rm596/firmware/013.016.toml   (optional; most versions need none)
[patches]
omit = []                                    # hack ids that do not apply to this version
extra = ["patches/013.016-only.txt"]
[status]
level = "L1"
notes = "same as 011.012"
```

Example `models/nokia-n8-rm596/model.toml` (abridged; the unchanged tables are today's):

```toml
schema = 2
family = "bb5-s60"
[device]
id = "nokia-n8-rm596"
vendor = "Nokia"
model = "N8"
type_code = "RM-596"
aliases = ["n8"]
[platform]
os_version = "symbian3"
personality = "symbian3"
soc = "rapido"
[firmware.accept]                            # as above
[board] ... [board.display] ... [[board.drives]] ... [hardware] ... [kernel] ... [[standins]] ... [modem]
[rom]
files = [{ rom = "Sys/Data/ESTARTCOMP.TXT", local = "ESTARTCOMP.TXT" }]
code_patches = ["patches/fmradio.txt", "patches/haptics.txt"]   # module-relative (section 2.2.3)
[[rom.rofs_patches]] ...                     # path + old/new: already version-tolerant
[[rom.rofs_replace]] ...
[[rom.file_edits]] ...
[composition]
impl = "gcebe"
imports = "composition/imports.txt"          # names only; addresses resolved per firmware
blob_place = "below-user-window"
[gl.host]
msaa = 8
[status]
level = "L1"
firmware_tested = ["011.012", "013.016"]
```

**Registry** `models/registry.toml`: the growing table of type codes. A row needs no profile; it
lets ROM detection say "Nokia E52 (RM-469): known phone, no emulation profile yet" instead of
"unknown". Columns: type code, vendor, marketing name, platform release, SoC family, `model`
(profile id or none), `status` (`supported`, `profile-draft`, `known`). Seed it from
`docs/device-matrix.md` sections 2 and 3 and the ROMs held in `roms/` (E52, E72, E5-00, 701, N73 ME).

```toml
[[types]]
code = "RM-596"
vendor = "Nokia"
name = "N8"
platform = "symbian3"
soc = "rapido-bcm2727"
model = "nokia-n8-rm596"
status = "supported"

[[types]]
code = "RM-469"
vendor = "Nokia"
name = "E52"
platform = "s60-3.2"
soc = "rapido"
status = "known"                             # firmware held, no profile
```

Families (`platform/families/`) and personalities are unchanged.

### 2.2 Firmware (a ROM build)

#### 2.2.1 Identity, read from the files

| Fact | Source, in order of trust | Status |
|---|---|---|
| type code, software version, product code, variant description/group, variant version | `.vpl` `VariantPackingList/Variant/VariantIdentification/{TypeDesignator,ProductCode,Description,VariantGroup}`, `SwVersion`, `VariantVersion` | [verified in source: `roms/RM-596_Nokia-N8/111.040.1511/*.vpl`, `roms/RM-320_Nokia-N95-8GB/35.0.001/*.vpl`] |
| file roles | `.vpl` `FileList/File/{Name,FileType,FileSubType,FlashImage/RofsIndex,SwVersionInfo}`: `Mcu` = core (RofsIndex 1), `Ppm` = ROFS2 language pack (index 2/3) and ROFS3 customer variant (index 3/4), `Content` = UDA, `MemoryCardContent` = eMMC/card image, `Dcp` = the data package zip, plus type-label pictures and templates | [verified in source: the same .vpl files; the N95's .vpl marks `.C0R` Mcu and `.v13` Ppm] |
| language pack / customer variant ids and dates | `.vpl` `SwVersionInfo` `id` ("02", "C14") and `PartSwVersionInfo type="RofsVersionString"` (`111.040.1511.02.01\n2012-07-28\nRM-596\n(c) Nokia`) | [verified in source] |
| software version, date, type code, product name (without a .vpl) | `Z:\Resource\Versions\sw.txt` in ROFS1 (UTF-16: `V 35.0.001\n11-11-09\nRM-320\nN95(c)NMP`), `langsw.txt` in ROFS2 | [verified in source: `tools/device.py _version_file`, `firmware_facts`] |
| customer variant id (without a .vpl) | `customersw.txt` in ROFS3 (same layout) and `Resource/Versions/model.txt`, `platform.txt` | [inferred: the `*#0000#` about screen reads these; not read by our tools yet] |
| product configuration | `.dcp` (a zip: `ProductConfiguration/*.xml`, `VariantConfiguration/<product>.xml`, `Refurbish/*productprofile.xml`) | [verified in source: `unzip -l` of two .dcp files]; useful only to display, not needed for emulation |
| core ROM header | `TRomHeader`: `iRomBase` at +0x8c, `iVersion`, `iTime`; image size | [verified in source for `iRomBase` (`inventory.py sos_inflate`); the rest inferred from EKA2 headers] |
| ROFS header | magic `ROFS`/`ROFx`, `iImageSize` at +0x24; also format version, time and image version | [verified in source for magic and +0x24 (`inventory.py rofs_slice`); the rest inferred] |
| FIASCO (`B2`) container header | tool versions and ASIC names only ("Elf2flash 10.21.000", "CMT"/"APE", "OMAP2420 UNISTORE-II-1.5.1 ALG"), no type code and no software version as text | [verified in source: strings of five headers under `roms/`]; so the container header identifies the *kind* of file (BB5 CMT/APE, OMAP1710/2420 ALG), never the firmware |
| FIASCO partitions | records tag 0x27/0x28 with partition name (`SOS+CORE`, `SOS+ROFS1`, `SOS+ROFS2`, `SOS+ROFS3`, `SOS*CORE`, `SOS_ROFX`), size and location; data chunks tag 0x17; UDA tag 0x2E | [verified in source: `tools/fiasco.py`] |
| file name | `RM-596_111.040.1511_02.01_Euro_2_79u_prd.rofs2.fpsx`, `rm465_*_prdapps.rofs2.v??`, `RM-320_35.0.001_PR.C0R`, `N73_rofx_3.0717.1.4.1_PRD.V16` | [verified in source]; last resort, for display and for pre-sorting an archive before anything is opened |

So the **firmware identity** is the tuple `(type_code, sw_version, rofs2_id, rofs3_id)`, written as
`RM-596/111.040.1511/02.01+C14.01` (core/language/customer), with `product_code` (0599762) and the
variant description recorded when a `.vpl` gave them. `sw.txt` alone (no `.vpl`, no ROFS2) still
gives a usable `RM-320/35.0.001`. Hashes (sha256 of every input file and of every extracted image)
are recorded in the manifest for *verification* (is this the file we imported) and for the build
cache key; nothing in a model refers to them.

#### 2.2.2 ROM store

Imported ROMs live in a store outside the repository tree that git sees (today `roms/` is
git-ignored [verified in source: `.gitignore` `/roms/`]; keep that path as the default store so
nothing moves for the owner, with `QEMBIAN_ROMS` to relocate it). Layout by identity, not by
hand-made name:

```
roms/
  index.toml                                  generated: every firmware the store holds (one line each)
  inbox/                                      archives dropped here are imported on the next scan
  RM-596/
    111.040.1511/
      rom.toml                                manifest (below)
      RM-596_111.040.1511_79u_prd.core.fpsx   original files, original names
      RM-596_111.040.1511_02.01_Euro_2_79u_prd.rofs2.fpsx
      RM-596_111.040.1511_C14.01_Germany_79u_prd.rofs3.fpsx
      RM-596_111.040.1511_U01.01_79u.uda.fpsx
      package/                                 .vpl, .dcp, readme.txt, .xml, .pcx: kept, never read again
  RM-320/
    35.0.001/ ...
```

`rom.toml` (generated by the importer, never edited by hand; rewriting it from the files must give
the same result):

```toml
schema = 1
type_code = "RM-596"
sw_version = "111.040.1511"
sw_date = "2012-07-28"
platform = "belle-refresh"                   # from the registry's version table when known, else ""
source = { archive = "Nokia_N8_RM-596_v111.040.1511_MIRA.zip", imported = "2026-11-08" }

[[products]]                                 # one per .vpl: which files form which product
code = "0599762"
description = "euro2_dg_germany"
group = "euro"
variant_version = "029"
files = ["core", "rofs2:02.01", "rofs3:C14.01", "uda:U01.01", "emmc:M004.56"]

[[files]]
role = "core"
name = "RM-596_111.040.1511_79u_prd.core.fpsx"
sha256 = "..."
container = "fiasco-bb5"
partitions = ["SOS+CORE", "SOS+ROFS1"]       # as read by tools/fiasco.py
sw = { version = "111.040.1511", date = "2012-07-28", type_code = "RM-596" }   # Resource/Versions/sw.txt

[[files]]
role = "rofs2"
id = "02.01"
label = "Euro_2"                              # from the .vpl / file name
name = "RM-596_111.040.1511_02.01_Euro_2_79u_prd.rofs2.fpsx"
sha256 = "..."
partitions = ["SOS+ROFS2"]
langsw = { version = "111.040.1511.02.01", date = "2012-07-28", type_code = "RM-596" }

[[files]]
role = "rofs3"
id = "C14.01"
label = "Germany"
...
[[files]]
role = "uda"
id = "U01.01"
...
```

A "firmware" as the user picks it is one *combination*: core + one rofs2 + one rofs3 (+ uda when the
model wants it). The store may hold ten rofs2 files for one version (the N8 011.012 directory holds
nine [verified in source]); the combination is chosen at device creation, defaulting to the first
product of the `.vpl`s or, without one, to the only/first rofs2 and rofs3 present.

Archives: `.zip` and `.rar` (`bsdtar`/`unar`, both read rar; `7z` as fallback) are expanded into a
temporary directory, identified, and the firmware files plus the package metadata are moved into
`roms/<RM>/<version>/`; the archive itself is left where it was (or deleted with `--consume`). Nested
folders inside archives are flattened by scanning for `.vpl`/`.fpsx`/`.C0R`/`.v1?`/`.V??`/`.c00`.
Today's `roms/_archives/*.rar|zip` are exactly such inputs.

Extracted and derived artifacts (not inputs): `build/firmware/<RM>/<version>/<combo>/` holds
`core-flat.img`, `core.json`, `rofs1..3.img`, `symindex.json`, `abi/`, `kernel-drivers.txt`
(today's `build/devices/<id>/` content from `inventory.py` [verified in source]). Composed ROMs go
to `build/roms/<model>/<firmware>[/<variant>]/hybrid.img(.rofs)` + `hybrid-manifest.txt` +
`syborg-device.dtb` + `gen/`: the build cache, keyed by (model, firmware combination, variant),
shared by every device instance that uses the pair.

#### 2.2.3 Firmware-specific emulation data, keyed so it survives versions

What is address-bearing today and how each item becomes version-independent:

| Item | Today | v2 |
|---|---|---|
| byte patches (`code-patches.txt`: `ADDR ORIG NEW # [H-id]`) | absolute ROM addresses of one build; romcompose refuses when ORIG mismatches [verified in source] | `module + offset`: `fmradioaudiocontrol.dll+0x2c  fff7e4ef  c046c046  # ConstructL: no leave [H-fmradio-nosession]`. The resolver takes the module's load address from the firmware's `core.json` (every XIP file has its address there [verified in source: `romimg.py --json` output is what `syb_sym` and `xip_imports` read]) and adds the offset; ORIG is still verified. A module whose code did not change between versions keeps the same offset (the N8 case: fmradioaudiocontrol moved by -0x4390 as a whole [verified in source: device-matrix 2.3]). When ORIG mismatches, the tool searches the module for the ORIG bytes (optionally a longer `pattern = "..."` with `?? ` wildcards and `nth = 1`) and *proposes* the new offset in a `firmware/<version>.toml` override; it never applies a guess silently. |
| GL entry patches (`gl/gles-patches.txt`, 175 lines) | generated from `libgles_cm.ordinals` + symindex by `tools/gen-gles-glue.py` [verified in source] | generated into the build cache per firmware; only `ordinals` (names) and `imports` (names) stay in the model |
| composition patches (`composition/gcebe-patches.txt`) | generated by `tools/blob_glue.py rebind` against `symindex.json` [verified in source: file header] | same: generated per firmware; `rebind` lines (names) stay in the model |
| `imports.txt` (sygl, gcebe) | names only, resolved at build [verified in source] | unchanged, in the model |
| blob addresses (`gl.blob_addr`, `composition.blob_addr`) | literal, "gap below the user window" [verified in source] | computed: `user_window_start` from `core.json` minus blob size, aligned; a model may pin `blob_addr` when the gap rule does not hold |
| `user_window_start` | literal per firmware (N8 v11 0x80424B40, v13 0x8041F810) [verified in source] | computed from `core.json` (lowest user-side file address); the composer already checks the overlap |
| `kernel.place` (`after-firmware`) | model | model (a property of the firmware generation's size, true for every N8 build we hold) |
| ROFS text patches (`rofs_patches`: path + old/new), `rofs_replace` (path), `file_edits` (path + anchor line) | model, already version-tolerant (old text verified) | model |
| `ESTARTCOMP.TXT` | model | model |
| ISI replies | model + family | model + family |
| symbol index, ABI reports | `build/devices/<id>/` | `build/firmware/<RM>/<version>/<combo>/` |

Proposed patch-file format v2 (one file per hack family, in `models/<id>/patches/`):

```
# H-fmradio-nosession: ... (why; what the original code does; what the patch does)
module = fmradioaudiocontrol.dll
+0002c  fff7e4ef  c046c046   # ConstructL: no leave
+00096  10b54068  00207047   # routing method 1
versions = *                               # optional: "011.*", "!= 013.016"
```

The converter (`tools/fw/patches.py convert`) rewrites today's absolute files using the
build's `core.json`, and the check `tools/fw/patches.py resolve MODEL FIRMWARE` prints the
absolute addresses for romcompose, so `romcompose.py --patches` needs no change.

### 2.3 Device instance (the user's phone: a VM)

An instance is one virtual phone, the way a VM is one machine in Proxmox: a user-chosen name, a
model, a firmware, its own drives, its own settings, and optionally a few overrides of the model
profile. Three N95s with different names are three instances of `nokia-n95-8gb-rm320`; they may run
different firmwares, have different C:/E: images and different GPU settings, and none of them sees
the others. Operations: create, clone (copy of config + drives, new name; snapshots not copied),
rename (directory and `name`), delete.

It is a directory in user data holding the configuration and *all* mutable state that today a sydb
session holds (drives, snapshots, catches, logs). The instance directory is the session directory:
`sydb --session` keeps working on it, and nothing in `daemon.py` changes for this (it takes a ROM
path and a drives directory [verified in source: `serve(sdir, rom, drives, ...)`]).

```
~/.local/share/qembian/instances/        (default; QEMBIAN_HOME overrides; repo default build/instances/)
  n8-germany/
    instance.toml
    drives/                               nvmemory.img (C:), nvmemory2.img (E:), rtc-offset
    snaps/  catches/  shots/  serial.log  daemon.log  ...   (today's session files)
```

```toml
# instance.toml
schema = 1
name = "n8-germany"
created = "2026-11-08"
model = "nokia-n8-rm596"
firmware = "RM-596/111.040.1511/02.01+C14.01"     # identity in the ROM store; the hash check is the store's job
rom_variant = ""                                  # model [rom.variants.<n>] name, "" = default; "audio-exp" on the N95
rom_options = { omit_hacks = [] }                 # per-instance hack toggles (what variants do today)

[drives]
c = { size = "64M" }                              # created sparse on first start, formatted by the phone
e = { size = "1G" }                               # or: e = { image = "/path/to/card.img" } (a host file used in place)
e_removable = true
sd = { mount = "" }                               # later: a host directory synced in through sydb push/pull
uda = false                                       # apply the firmware's UDA image to C: at creation (when the model supports it)

[settings]
gl = "host"                                       # host | soft
gl_host = { msaa = 8, filter = "auto", compose = "host" }   # today <session>/gl.json
audio = "wav"                                     # wav | host
scale = 1.0
ram_mb = 0                                        # 0 = model default
trace_mask = 0

[overrides]                                       # optional: small changes to the model profile, this instance only
# Same tables and merge rules as a model's `extends` (deep-merge, arrays of tables by name):
# the resolved profile is model (+ family) with this table merged on top. Meant for a handful of
# values: a GPU setting, a stand-in config key, a modem knob, RAM. Not for new stand-ins or patches.
board.ram_mb = 256
modem.sim = false
[[overrides.standins]]
name = "exyin"
config = { rotate = 90 }

[state]                                           # written by tools, not by the user
last_start = "2026-11-08T10:11:12"
boots = 14
agent_installed = true
pinned_snapshot = ""
```

Overrides and the build cache: overrides that only change run-time values (`[board]` arguments,
`[gl.host]`, `[modem]` knobs, `[[standins]] config` keys, all of which reach QEMU as arguments or
board-config entries [verified in source: `docs/devices.md` §3.1, `[[standins]] config` ->
`BoardConfig`]) share the model's composed ROM. An override that changes what is composed (a
`[rom]` value, `omit_hacks`) makes the cache key `(model, firmware, variant, overrides-hash)`, so
the instance gets its own ROM in `build/roms/`. `device.py check` warns when an override touches a
table it does not know how to apply at run time.

Multiple devices of one model are directories with different names; the same `(model, firmware,
variant)` triple shares one composed ROM from the build cache. The owner's "real" phone is an
ordinary instance (`n95` with its old drives, `rom_variant = "audio-exp"`, `audio = "host"`), and
`sydb --device`, `run-gui.sh`'s defaults and `DEVICE_ROM` become its instance settings
(`docs/sydb.md` "Phone window" describes the special case [verified in source]).

Where things live, summarised:

| Thing | Repo (versioned) | User data (git-ignored) |
|---|---|---|
| registry, model profiles, keymaps, patches (module-relative), ISI replies, families, personalities | `models/`, `platform/`, `personalities/` | |
| ROM store (original files + `rom.toml`), archive inbox | | `roms/` (default) |
| extracted images, symbol index, ABI reports | | `build/firmware/<RM>/<version>/<combo>/` |
| composed ROMs, DTB, generated patches | | `build/roms/<model>/<firmware>/[<variant>/]` |
| device instances (config, drives, snapshots, logs) | | `instances/<name>/` (default `build/instances/`, or `$QEMBIAN_HOME`) |
| per-device long notes, hacks | `docs/devices/<id>/`, `models/<id>/hacks.md` | |

## 3. ROM detection

### 3.1 Algorithm (`tools/fw/identify.py`, new; `tools/fw/import.py` calls it)

Input: a directory, an archive, or a list of files.

1. **Expand**: archives into a temporary directory (zip: `zipfile`; rar: `bsdtar`/`unar`;
   nested archives one level deep). Collect candidate files by extension and by content: any file
   whose first byte is `0xB2` with a plausible header length is a FIASCO container
   [verified in source: `fiasco.py blocks`, five headers checked]; `.vpl` are XML
   `VariantPackingList`; `.dcp` are zips.
2. **Package metadata first**: every `.vpl` gives type code, software version, product code and a
   file list with roles and ids. Files named in a `.vpl` get their role from it; files not named get
   step 3.
3. **Container scan**: `fiasco.py` partition names decide the role when there is no `.vpl` or the
   name is missing: `SOS+CORE`/`SOS*CORE`/`SOS_CORE` = core (with ROFS1 inside), `SOS+ROFS2`/`SOS_ROFX`
   = rofs2, `SOS+ROFS3` = rofs3, tag 0x2E = uda; a bare chunk stream whose data begins with `ROFx`
   is an N73-style language ROFS [verified in source: N73 profile `partitions = { rofs = "_pre" }`].
   The header's ALG string (`OMAP1710`, `OMAP2420`, `CMT`/`APE`) is kept as a hint for the family.
4. **Version files**: extract (to the cache, the extraction is needed anyway) and read
   `Resource/Versions/sw.txt` from ROFS1 and `langsw.txt` from ROFS2 (today's `firmware_facts`
   [verified in source]); read `customersw.txt` from ROFS3 and `model.txt` when present [inferred].
   `sw.txt` is the authority for `(type_code, sw_version, date)`; the `.vpl` must agree, else the
   package is reported inconsistent (mixed versions in one folder are common in collectors' dumps).
5. **File-name fallback**: `(RM-?\d{3})` and a version `\d+\.\d+(\.\d+)*` from the name, only when
   neither a `.vpl` nor an extractable ROFS1 is available (a lone rofs2 file, a truncated core). The
   result is marked `confidence = "name"` and the GUI says so.
6. **Group and store**: files with the same `(type_code, sw_version)` form one store directory;
   products from the `.vpl`s become `[[products]]`; everything else goes to `package/`.
7. **Pick a model**: registry lookup by type code. Exactly one model per type code (variants have
   their own code). If the registry row has `status = "known"` (no profile), the import succeeds
   (the ROM is stored) and the GUI offers "create a draft profile from <nearest model>" (today's
   `device.py new ID --from BASE` intent). If the type code is not in the registry at all: stored
   under its code, shown as "unknown Nokia type code RM-xxx (software 022.009, 2009-xx-xx)" with a
   link to add a registry row.
8. **Check against the model** (when the user chose the model first, or after auto-pick):
   `[firmware.accept]`: type code in `type_codes`, required roles present, container kind, version
   range; after extraction `kernel-drivers.txt` versus `[hardware]` (today's `drivers`/`no_drivers`
   check [verified in source: `identity_problems`]). Mismatches are listed; the user can force.

What the existing tools already do [verified in source]: `fiasco.py` splits containers and names
partitions; `inventory.py` finds files by glob, checks `firmware.sha256`, extracts partitions
(including N73 deflate and RAPIDO 0xC00 prefixes), unpages the core, writes `core.json`, the driver
list and the ABI reports, and calls `identify`; `device.py identify DIR` reads `sw.txt`/`langsw.txt`
from extracted images and scores every profile by `identity_problems`. Not done today: reading
`.vpl`/`.dcp`, archives, file-role detection from partitions without a profile, the registry.

### 3.2 Reporting

Every detection returns a record `{confidence: vpl|sw.txt|name, type_code, sw_version, date,
products[], files[{path, role, id, sha256, partitions}], model: id|none, registry: row|none,
problems[]}`. The GUI shows it as a card: "Nokia N8 (RM-596), software 111.040.1511 (2012-07-28),
Belle Refresh. Products: 0599762 euro2_dg_germany (Euro_2 + Germany). Model profile:
nokia-n8-rm596 (supported, tested on 011.012 and 013.016; this version untested)". Unsupported:
"Nokia E52 (RM-469), 022.009: known phone, no emulation profile. Stored. [Create draft profile]".

## 4. User flows

Pieces: `manager` = the new PyQt6 window (`tools/qembian/manager.py`, sibling of `phone.py`);
`lib` = a Python package `tools/qembian/` (store, registry, models, instances, build) that the CLI,
the manager and MCP share; sydb daemon/CLI/agent unchanged unless noted.

| Flow | Manager window | Behind it | CLI |
|---|---|---|---|
| Import ROM | "ROM library" tab, button "Import..." (file/folder/archive picker, or drop onto the window); progress (expand, identify, extract); result card (§3.2) | `fw/import.py`: §3.1 steps 1-7; extraction = today's `inventory.py` steps 3-6 into `build/firmware/...` | `qembian rom import PATH [--consume]`, `qembian rom list`, `qembian rom show ID` |
| Create device, model first | "Create device" wizard: page 1 model (table from the registry: name, type code, platform, support level); page 2 firmware (store entries whose type code the model accepts; "Import..." inline); page 3 product/combination (core + language + variant, from `[[products]]`); page 4 disks (C: size, E: size or existing image, removable, UDA); page 5 settings (name, GL backend, audio, scale; "Advanced" = optional profile overrides, empty by default); Finish = build if the cache lacks the ROM | `instance create` writes `instance.toml`; `build.ensure(model, firmware, variant)` = today's `device.py build` with the firmware's cache dir instead of `build/devices/<id>` | `qembian device create NAME --model ID --firmware FWID [--product CODE] [--c 64M] [--e 1G|--e-image F] [--gl soft]` |
| Create device, ROM first | wizard page 1 "From ROM files...": picker, identify, auto-picked model shown with its status; continue at page 3 | same, model from the registry | `qembian device create NAME --rom PATH` |
| Edit disks / SD | instance "Settings" dialog (same pages 4-5) while stopped; "Replace E: image", "Resize" (new sparse image, old kept), "Mount folder as card" (push/pull through the agent, not a block device, until a FAT image tool is wired) | rewrite `instance.toml`; drive files in `drives/` | `qembian device set NAME e.size=2G`, `qembian device disk NAME --e-image F` |
| Start / Stop / Reset / Pause | device row buttons; Start opens the phone window (`phone.py` reused with `--instance`); Reset = QMP system_reset (phone.py's `reset` [verified in source]); "Factory reset" = delete drives | `sydb start --session <instance> --rom <cache rom>` with the instance's settings (`--gl`, `--audio`, `gl.json`); `sydb stop` | `qembian device start NAME [--fresh]`, `stop`, `reset`, `phone NAME` (= today's `SYBORG_DEVICE=ID sydb phone`) |
| Snapshots | "Snapshots" panel per instance: list (name, time, size), Save, Restore, Fork (new instance from a snapshot), Delete; N8 restore warns about the host GPU limitation [verified in source: sydb.md] | `sydb snap save/list/rm`, `restore`, `fork --into <new instance dir>` + write its `instance.toml` | `qembian device snap NAME ...` (thin wrappers over `sydb snap`) |
| Explore files | "Files" panel: tree of C:/E:/Z: (running: agent `ls`; stopped: `mtools` on the FAT images for C:/E:, `rofs.py`/`core.json` for Z:), push/pull, delete, open with host viewer | agent `fs_*` (running), `sydb.py drive_read/drive_write` (stopped) [verified in source] | `sydb ls/push/pull/rm` |
| Several devices of one model | just more rows ("n95-daily", "n95-test", "n95-fw10"): each has its own drives, settings, firmware and overrides; the ROM is built once per (model, firmware, variant[, overrides]) | cache key; the instance directory is the isolation boundary | `qembian device create n95-test --model nokia-n95-8gb-rm320 --firmware RM-320/35.0.001` |
| Profile overrides | instance "Settings" dialog, page "Advanced": a small editable TOML text of `[overrides]` with the resolved value next to each key and a "reset to model" button; applies on the next start | merge into the resolved profile before `qemu-args`/`board_config`; rebuild only if a composed value changed | `qembian device set NAME overrides.board.ram_mb=256`, `qembian device show NAME --resolved` |
| Clone | row "Clone..." asks for the new name; copies `instance.toml` (new name, state cleared) and `drives/` (reflink on btrfs, as snapshots do today [verified in source: sydb.md]); snapshots, catches and logs are not copied; the stopped instance is required | `instance clone SRC DST` | `qembian device clone SRC DST` |
| Rename | row "Rename..."; stopped instance only; directory and `name` change, snapshot metadata paths rewritten | `instance rename` | `qembian device rename OLD NEW` |
| Delete | row "Delete..." with the size (drives, snaps) and a checkbox "also remove the composed ROM if no other device uses it" | `rmtree` instance; cache GC by reference count | `qembian device rm NAME` |
| Status | row shows model, firmware id + platform, state (stopped/booting/idle/paused), uptime, last boot time, level of the model | `sydb status` per instance (poll) | `qembian device list` |

The manager's main window: left, the device list (name, model, firmware, state), like a VM list;
right, the selected device's summary (with its overrides, if any), drives, snapshots, files tabs;
row actions Start/Stop/Reset, Clone, Rename, Delete; top, "Create device", "Import ROM", "Models"
(registry table, read-only, with support status and a link to the model's docs). It never embeds
the screen: Start opens the existing phone window.

MCP: `mcp.py` gains `instance_list`, `instance_start`, `rom_list` and `rom_import` so agents can
make and boot a test instance without shell commands (today they use `session_start` with a
session path [verified in source]).

## 5. Migration plan

Rules for every step: the six working profiles (N73, N95 8GB, 5730, N79, 5800, N8 011.012) and
the N8 013.016 boot after the step; `tests/device-smoke.sh ID` on at least the N95 and the N8 and
`tests/device_py_test.py` pass; old sessions in `build/sydb*` keep starting; nothing proprietary is
committed; one commit per step. Effort is for one person with an agent, in half-days (hd).

| # | Step | What changes | Safety check | Effort |
|---|---|---|---|---|
| 0 | Spec and registry | This document; `models/registry.toml` seeded from the matrix and `roms/`; `tools/device.py registry` prints it. No profile changes. | `device_py_test.py` | 0.5 hd |
| 1 | Firmware identification tool | `tools/fw/identify.py`: `.vpl` parser, FIASCO role detection, `sw.txt`/`langsw.txt` readers moved out of `device.py` into `tools/fw/identity.py` (device.py imports them), file-name fallback, JSON output. Run it over every directory in `roms/` and over `roms/_archives/*` (into a temp dir) and compare with what the profiles declare today. | identification of the six held firmwares equals `[firmware.identity]`; archives identify the same as their folders | 1.5 hd |
| 2 | ROM store and importer | `tools/fw/import.py` writes `rom.toml` into `roms/<RM>/<version>/` and `roms/index.toml`. Migrate the existing tree with a script that *moves* `roms/RM-596_Nokia-N8/011.012` to `roms/RM-596/011.012` etc. (same files, new parent names), imports the archives, and leaves `roms/_archives/` as the inbox. `inventory.py --fw-dir` keeps working (store directories are plain directories). | `rom.toml` re-generated from the moved files is identical; `device.py build` still finds the files through the (temporarily rewritten) globs | 1 hd |
| 3 | Build cache keyed by firmware | `device.py paths(prof, firmware)`; extraction goes to `build/firmware/<RM>/<version>/<combo>/`, composition to `build/roms/<model>/<firmware>/`; `build/devices/<id>` becomes a symlink to the model's default firmware build so every script (`run-gui.sh`, `sydb.py DEVPATHS`, `syb_sym`, smoke test) keeps working. Firmware chosen by `--firmware ID` or the model's `[status] default_firmware` (new, temporary). | smoke N95 + N8; `cmp` of the new `hybrid.img` against the old one (byte-identical, because only paths changed) | 1.5 hd |
| 4 | Module-relative patches | `tools/fw/patches.py` (convert, resolve, rebase); `device.py build` resolves patches to absolute lines for romcompose; `[gl] blob_addr`, `[composition] blob_addr`, `user_window_start` derived from `core.json` (profiles keep the literals as *checks* for one step: the tool compares and warns). Convert the N95, N8 and 5800 patch files (the others have none [verified in source: directory listings]). | `cmp hybrid.img` byte-identical for all six; resolving the N8 011.012 patches against 013.016's `core.json` gives the v13 file's addresses (the -0x4390 shift) | 2 hd |
| 5 | Split the profile | `models/<id>/model.toml` = `device.toml` minus `[firmware]` (replaced by `[firmware.accept]`), minus the literals of step 4; `devices/` becomes a symlink to `models/` for one release so `SYBORG_DEVICE`, docs and agents keep working; `device.py load` accepts both schemas (1 and 2) and warns on 1. `nokia-n8-rm596-v13` is folded into `models/nokia-n8-rm596/firmware/013.016.toml` (empty or near-empty) and deleted; `nokia-n95-8gb-rm321` stays a variant model. | smoke N95, N8 011.012, N8 013.016 (`--firmware`); `device.py check` on all; the `cmp` from step 4 again | 1.5 hd |
| 6 | Instances | `tools/qembian/instance.py` (create, clone, rename, delete, list, resolved profile = model + `[overrides]`); `instance.toml` written into every existing session dir (`build/sydb` -> model from its `device` mark file [verified in source: `cmd_start` writes it], firmware = the model's default, settings from `gl.json`); `sydb start --instance NAME` (`--session` stays as the low-level form); `sydb phone NAME`; `run-gui.sh` reads the owner's instance; `tests/device-smoke.sh` creates a throw-away instance. The owner's phone = instance `n95` on the old `build/devices/.../drive-c.img` files (linked, as `use_device` does today). Overrides: run-time ones first (board/gl.host/modem/standins config); composed ones (cache key with overrides hash) in the same step or deferred. | existing sessions start unchanged; smoke; `sydb phone n95` shows the owner's phone; a cloned `n95-test` boots on its own drives while `n95` is untouched | 2 hd |
| 7 | Manager GUI | `tools/qembian/manager.py`: device list, create wizard (both orders), import, disks, start/stop/reset, snapshots, files (agent when running, mtools when stopped), delete. Reuses `phone.py` for the screen. | manual walk-through of every flow in §4 on a test instance; nothing in the owner's instance touched | 3-4 hd |
| 8 | Cleanup | remove schema 1 support, the `devices/` symlink, `build/devices` symlinks, `firmware.sha256` files, `FW_DIR`; update `docs/devices.md`, `adding-a-device.md`, `device-matrix.md` (coverage per firmware from `[status].firmware_tested`), `sydb.md`, `CLAUDE.md`. | smoke all six; docs reviewed | 1 hd |

Total about 14-16 half-days. Steps 1-2 and 4 are independent of each other and can run in
parallel sessions; 3 before 5; 5 before 6; 6 before 7.

What happens to existing sessions: nothing in steps 0-5 (they hold a ROM path and drives); step 6
writes an `instance.toml` beside them and the daemon is untouched; `build/sydb-smoke` and fork
sessions (`build/sydb-fork-*`) become unnamed instances with `ephemeral = true` that `sydb gc`
removes as today.

Risks:
- Byte-identical `hybrid.img` after step 4 depends on patch ordering and the generated GL/gcebe
  files landing at the same addresses; a diff that is only reordering is acceptable, any other
  difference is a bug in the resolver. The check is cheap and decisive, so keep it.
- `core.json` module addresses are the resolver's anchor; the moving-model N73 and `after-firmware`
  N8 layouts must be tested explicitly (the N73 has no byte patches today, so only the window
  derivation matters there).
- Old packages without `.vpl` (N73, E52 as held [verified in source: no .vpl in those directories])
  rely on `sw.txt`; a lone language file cannot be placed without its core and is reported as such.
- rar extraction needs `bsdtar` or `unar` on the host and in `Containerfile.modern`.
- The language/variant combination is part of the firmware identity: a device created on
  `02.01+C14.01` does not silently switch to `04.01+00.01` when another pack is imported.
- Snapshots record the ROM path (`meta['rom']` [verified in source: `daemon.py snapshot`]); moving
  the composed ROM into `build/roms/` invalidates old snapshot metadata unless step 3 rewrites the
  paths or keeps the `build/devices/<id>` symlink until step 8.
- `symindex.json` depends on the proprietary SDK import libraries for names [verified in source:
  `syb_sym.py` docstring]; the patch resolver must use only `core.json` (module base) so a user
  without the SDK can still build.
- The N95's `hybrid-exp.img` variant and `sydb --device` are the owner's daily path; step 6 must be
  tested on a copy of those drives before the owner's instance is created (memory rule: never boot
  the owner's drives for a test).

## 6. Decisions (owner, 2026-11-08)

Migration 2026-11-10: steps 0 (registry), 1 (identify.py, layout read from the files), 2 (ROM store,
held firmware migrated), 3 (build cache keyed by firmware), 4 (module-relative patches; blob addresses
stay pinned in the profiles for now), 5 (models/<id>/model.toml + firmware overlays; old ids kept),
6 (instances) and 8 (cleanup) done; 7 (manager GUI) not started (owner: not needed for now). See the
commits. What was built differs from the plan above in these points:
- Instances (step 6): `tools/instance.py` (not tools/qembian/), `build/instances/<name>/`
  (`$QEMBIAN_HOME/instances`). `instance.toml` has `model`, `firmware` (the model's overlay name, not a
  store id), `rom_variant`, `primary`, `ephemeral`, `[settings]` gl/audio/scale and `[overrides]`
  (run-time tables only: board, gl.host, modem, standins config; `tools/device.py` applies them under
  `$SYBORG_INSTANCE` and refuses anything else). No `[drives]`/`[state]` tables yet: drives are made
  at the first boot as before. The owner's sessions were adopted as primary instances (`build/sydb` ->
  `n95`, `build/sydb-<id>` -> short names), the run-gui drives moved into `n95-gui` and `n8-gui`; sydb
  takes `--instance NAME` and `phone NAME`; a bare `SYBORG_DEVICE=ID sydb` opens that device's primary
  instance. `sydb --device` and `scripts/run-gui.sh`'s drives in the build tree are gone (run-gui boots
  an instance). Overrides that change the composed ROM are refused rather than given their own ROM.
- Cleanup (step 8): no `devices/` (the default device is `models/registry.toml` `default`), no
  `build/devices/<id>` links (`tools/device.py paths dir` is `build/roms/<id>/<firmware key>/`), no
  `firmware.sha256` (inventory checks the flash files against the store's `rom.toml` hashes), no
  `FW_DIR` (`inventory.py --fw-dir` for files outside the store). Device ids from before v2
  (`nokia-n8-rm596-v13`) stay as the overlays' `[device] id`: they name build directories, sessions
  and snapshots.

1. **Locations**: ROMs and instances stay inside the repository tree for now (`roms/`, `build/instances/`,
   git-ignored). The future application gets a config file with a list of directories to search for ROMs.
2. **Model ids**: keep `nokia-n8-rm596` (vendor-model-typecode) as the directory name, the registry maps
   the bare type code to it. Pure type-code ids (`RM-596`) may come later.
3. **Type-code variants with the same user side** (RM-321, RM-133): one model listing both codes in
   `[firmware.accept].type_codes`, with a `[variants]` table for the hardware differences.
4. **Language/variant combinations**: extract and symbol-index a combination on first use.
5. **Flash-file layout is read, not declared** (owner, 2026-11-10): today each profile lists the FIASCO
   partition names per firmware file (`[[firmware.files]] partitions`), so a newer firmware of the same
   phone needs its own overrides (N73 ME 4.0839: ROFS1 in its own `SOS_ROFS`, a `SOS+ROFX` record,
   where 3.0717 had ROFS1 after the compressed core and a bare chunk stream). That belongs to the files,
   not the phone: `tools/fw` should find the core (SOS*CORE / SOS+CORE / SOS_CORE, compressed or not)
   and every ROFS/ROFx image itself (record names, then the ROFS/ROFx magic and iImageSize), and order
   them by the ROFS header's own fields. Profiles then keep only the file globs and the identity.
   Done 2026-11-10 (`tools/fw/layout.py`, inventory): every held firmware re-extracts byte-identically.
6. **Archives**: extract on import, leave the archive where it is; never delete or move one unless the
   owner asks.
7. **GUI**: quick PyQt6 launcher, separate from the phone window (`phone.py`); the GUI is the owner's
   way, the CLI is the agents' way. A Rust rewrite with a 3D phone view may replace it later.
