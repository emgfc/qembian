# devices/

One directory per phone, named `<vendor>-<model>-<type-code>` (lower case, e.g.
`nokia-n95-8gb-rm320`). Each holds a declarative profile `device.toml` plus the small data files it
points at (keymap, ISI replies, ROM byte patches, ESTARTCOMP, GL glue inputs). Everything the host
tools and the guest build need to turn the shared QEMU binary, kernel and stand-ins into *that*
phone comes from here; nothing proprietary is ever committed (firmware stays in `firmware/<id>/`,
git-ignored, and its outputs in `build/devices/<id>/`). A variant (same hardware family, different
type code or market) is its own directory with `extends = "<base-id>"` and only the fields it
changes. The schema, layering and tool contract are in [docs/devices.md](../docs/devices.md); the
flow for a new phone is in [docs/adding-a-device.md](../docs/adding-a-device.md). Default device:
the `devices/default` symlink (or `SYBORG_DEVICE=<id>`).
