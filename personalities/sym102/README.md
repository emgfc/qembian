# Personality sym102: Symbian OS 10.1 FP2 (Nokia Belle FP2) user sides on our EPL kernel

Used by devices with `platform.personality = "sym102"`: `models/nokia-701-rm774` (113.010.1506) and
`models/nokia-808-rm807` (113.010.1508). Registry number 7 (`personalities/registry.toml`). Kernel side:
`patches/0022-sym102-personality.patch`.
113.010.x is Belle Feature Pack 2, not FP1 [V: "Nokia Belle Feature Pack 2" in ROFS1 of both firmwares; renamed
2026-11-10]. The kernel's boot line reads "10.1 FP2" from the next kernel build on.

| part | what | evidence |
|---|---|---|
| exec numbering | sym101 (Belle Refresh) with S11, S12 and Sd8 inserted: Belle S11..Sd5 = FP2 S13..Sd7, Belle Sd8..S114 = FP2 Sd9..S115; fast calls unchanged | [V] every EUser export that reaches one call in both firmwares (249, none ambiguous), `tools/swi_map.py` reports in `build/roms/<id>/<fw>/abi` |
| S11, S12, Sd8 | new in FP2; no export reaches them: unmapped, logged (`EXEC92: unmapped ... slow exec`), KErrNotSupported | [V] numbers, [I] that nothing at boot needs them |
| Belle Sd6, Sd7 | ThreadGetHeap, ThreadAsProcess: no FP2 slot left, no export reaches them in Belle either | [V] |
| everything else | as sym101 (Ok101 hooks now S10c..S10e, TLocalThreadData layout, nested library attach) | [I] the same EUser line |
| loader structures | TCodeSegCreateInfo 476 bytes of data (sizeof 480; Belle 472): the extra word sits before TProcessCreateInfo's own fields, which start at 476 in the base's tail padding (ProcessCreate copies 0x210 = 476 + 48 rounded) | [V] probe exec-sizes (CodeSegCreate 0x1e0 vs 0x1d8), KDLL trace (process fields one word late at 480) |
| iUseCodePaging | FP2's loader passes 2 for RAM-loaded code it does not page (Belle: 0); our kernel would page any non-zero value and failed with KErrArgument. Read as "not paged", 2 written back (the loader checks it) | [V] raw dumps of the same cfrtp.dll create on Belle and FP2; meaning [I] |

Status (2026-11-10). Fixed so far (all committed): exec numbering, loader structures, paging mode 2,
kernel-side SVCs through the native table (patches/kernelhwsrv/platform/0023: exyin's Kern::HalFunction ran
SessionSendSync in fbserv's FP2 table), VideoCore IV chip version in P&S 2726beef/0 = 0x04000100 (sgresource:
0 -> khclient_vc3, >= 0x04000100 -> khclient_vc4, between -> XSgDriver -39), and the composition backend's
interface UID 0x2002E6D5 in gcebe (src/gcebe/gcebe.c "UID 0x2002E6D5").

UID 0x2002E6D5 is not MDisplayMapping (2026-11-09's guess): [V] gcerenderstage resolves it on the next stage and
forwards slot 0 (const TSurfaceId &, 1, 0, TRequestStatus &) and slot 1 (const TSurfaceId &); the caller is an
alfcompositorrs active object (surface ID at +0x1C, iStatus at +4) whose RunL calls its observer once on
KErrNone. [I] a one-shot "surface ready" notification: gcebe completes it at once. The identity "coordinate"
copy had written the 16-byte surface ID over that object's iStatus and queue link, so the window server
faulted in TDblQueBase::DoAddPriority at the next CActiveScheduler::Add (the bad link was always word 2 of
the surface ID in the GCEBE log). Ruled out on the way: the TLocalThreadData layout (FP2 EUser's
RThread::Id, UserSvr::DllTls, TLocalThreadData::DllTls and CActiveScheduler::Add are instruction-identical
to Belle's: ID at +12, iTlsHeap +16, iTls +20, scheduler at +4).

SIM over PN_UICC (0x8C): FP2's TSY and SAServer use UICC, not PN_SIM; unanswered, start-up waited 40 s
for the SIM language (EF_LI read). Now answered as "server up, no card" (platform/isi/replies-bb5.txt 8C 00/03,
iscpeer.inl 8C 0C -> failed with the service echoed): the language is set at 13.5 s instead of 56 s. 8C 06
(application request) is still unanswered, with no effect seen.

FM radio / audio server (H-fmradio-none, models/<id>/code-patches.txt, 701 and 808). Chain [V]: !AudioServer
(saaudioserver -> Lac.dll -> fmradioaudiocontrol.dll factory, untrapped) starts radioadaptationserver.exe; its
radio HAL (radiohalserver.dll ConstructL) opens Bluetooth HCI direct access through ESock (fn 190 -> -1: no
Bluetooth stack here), leaves -2, the server reports -2 to its launcher and then faults in the HAL destructor
(NULL member at +0x30). fmradioaudiocontrol left -2, !AudioServer panicked -2, and Wserv's tactile click
plugin (tactilefeedbackresolver.dll+0xf16) waited for ever in User::WaitForRequest for the feedback player,
which needs the audio server: the window server and everything behind it stopped. The N8 (Belle) has the same
radio HAL failure, but its older radio server answers its launcher first. Patched in fmradioaudiocontrol.dll
(code identical on both FP2 phones): a failed connect is kept, every radio audio control returns
KErrNotSupported. Found with: the thread wait-state walk (all waiting on their request semaphores), a
hardware breakpoint in User::WaitForRequest conditioned on Wserv's NThread (TheScheduler.iCurrentThread at
0xC8000C28) to read its user stack, and catch --at User::Leave --thread AudioServer.

RAM: the 701 ran with the N8's 172 MB. After the first-boot wizard's page transition alf's render thread
(EwSrv::alfredserver) died: our libEGL.dll's OpenVG RI (Image::Image) memset a NULL pixel buffer, the new[]
of a 360x554 VGImage (~800 KB) having failed (about 11 MB of RAM free), and the screen went black. The phone
has 512 MB: ram_mb = 548 (512 + the unpaged core image, the 5730's convention); the same steps then reach the
home screen. Open in src/khr: the RI expects new[] to throw (src/khr/shim/ri_shim.h maps that to khr_oom),
but Symbian's new[] returns NULL, so an out-of-memory crashes on NULL instead of the planned panic or
VG_OUT_OF_MEMORY_ERROR.

Status (2026-11-10): the 808 boots to the home screen too (same fixes; ram_mb 548; its wizard defaults to
Russian, keeping it avoids the language restart). The 701 boots to the home screen (fresh drives: language, country, date in the
first-boot wizard, then the home screen with clock and calendar widgets, "Offline"). Open: PN_UICC 8C 06
unanswered; no FM radio (H-fmradio-none); gcebe does not compose off-screen targets (transition effects).

Method that worked: catch --kind panic --thread Wserv --action freeze, then `dbg mem` walks of the
active-object queue (CActiveScheduler + 8: TPriQue, links at CActive + 12) until the bad link, and the
owner's vtable (alfcompositorrs+0xd708) -> its RunL/DoCancel in the extracted DLL (tools/rofs.py --extract,
tools/e32load.py); gcerenderstage's forwarders read the same way. Also KIPC/KDLL/KEXEC trace builds
(device.py build --trace) and gdb with _syborgfx_ekern.exe.sym -o 0x81e93000 (701 ROM).
