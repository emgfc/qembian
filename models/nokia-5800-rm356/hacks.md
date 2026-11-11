# Nokia 5800 XpressMusic (RM-356): device hacks

The workarounds that belong to this device. Platform-wide ones are in [docs/hacks.md](../../docs/hacks.md).

| Hack | What | Why | Real implementation |
|---|---|---|---|
| **H-startuptone-none** | `[[rom.rofs_patches]]`: CenRep 101F8763 key 3 (start-up tone path, `cap_wr=alwaysfail`) in our copy of ROFS2 points to `z:\data\Startup_none.aac`, a file that does not exist, so the start-up animation plays no tone | Startup waits for the tone to end; RAPIDO audio (DSP shared memory) is not emulated, so the player never ends: the audio server's EapDsp thread writes past the MdaifDriver stand-in's buffer (KERN-EXEC 3), its F3 0x34 data requests now get "all taken" (iscpeer) | a RAPIDO DSP model (DSP memory map in the F3 0x31 reply, shared-buffer protocol); docs/devices/nokia-5730-rm465/rapido-audio.md |
| touch: `exyin` stand-in | digitiser on the Syborg pointer device, default calibration for the panel size, `rotate = 270` (the UI is the panel turned by the screen mode) | Nokia's exyin.dll drives the real touch controller | none needed beyond calibration; the S60 calibration application writes a new matrix through the HAL |
