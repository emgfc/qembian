# Context framework / sensor probes (phone-side diagnostics, run with `sydb rerun`)

- `cfprobe.cpp`: prints the CFServer's values of the auto-rotation rule's contexts and the sensor server's channel
  list (type, quantity, data item size). Build: `bash tools/probes/cf/build-cfprobe.sh` (S60 3rd FP1 SDK + the
  Symbian^3 sensrv libraries) -> `build/n95apps/cfprobe.exe`; run `sydb rerun build/n95apps/cfprobe.exe --wait 15`
  and read `CFPROBE:` lines in the serial log.
- `chprobe.cpp`: opens the orientation and XYZ channels with CSensrvChannel and listens 5 s. Build with
  `S60SDK=~/work/Nokia_Symbian3_SDK_v1.0/epoc32 bash tools/probes/cf/build-chprobe.sh`. A leave panics it
  USER-EXEC 3 (the linked libsupc++ unwinder does not match Symbian's); read the code with a breakpoint on
  User::LeaveIfError instead.
Written 2026-11-11 (cfprobe started by another agent); used to find the accelerometer registration failure
(docs/devices/nokia-n8-rm596/accelerometer.md section 7).
