# models/ (device model v2)

One directory per phone model, named `<vendor>-<model>-<type-code>` (lower case, e.g.
`nokia-n95-8gb-rm320`): `model.toml` (the phone: board, hardware, kernel, stand-ins, modem, ROM edits,
status) plus the data files it points at (keymap, ISI replies, ESTARTCOMP, byte patches, GL and
composition inputs). Each firmware the model runs is an overlay in `firmware/<name>/firmware.toml`
(its files and the language/customer pack choice, identity, addresses, and whatever that release needs
differently, e.g. Belle's personality and patches), merged over the model by the same rules as
`extends`; the model names its default (`[firmware] default`). Files are looked up in the firmware's
directory first, then the model's. `registry.toml` lists the known type codes.

Ids: a model's id loads it with its default firmware (`nokia-n8-rm596`); an overlay with `[device] id`
keeps the profile name it had before (`nokia-n8-rm596-v13` = the N8 on 013.016); `<model>@<firmware>`
reaches any other overlay. `registry.toml` `default` is the device of a bare command. A type-code variant of the same phone (RM-321, RM-133) is a model that `extends`
another. The schema and tool contract: [docs/devices.md](../docs/devices.md); a new phone:
[docs/adding-a-device.md](../docs/adding-a-device.md); the design: [docs/device-model-v2.md](../docs/device-model-v2.md).
Nothing proprietary is committed: flash files are in the ROM store (`roms/`, tools/fw/store.py), what
is extracted from them in `build/firmware/`, each device's build dir (composed ROM, DTB, generated files,
the extraction linked in) is `build/roms/<id>/<firmware key>/` (`tools/device.py paths`). The user's
phones are device instances (`build/instances/<name>/`, `tools/instance.py`, docs/sydb.md).
