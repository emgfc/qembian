# Logging the N95 firmware can produce (short survey, 2026-11-05)

Question: does the release N95 firmware have logging that is switched off and could be switched
on to tell us more during boot? Short answer: **almost nothing worth having is switched off; it is
compiled out.** The one path that release binaries still use, `RDebug`, already reaches our serial
log.

| Channel | State in the N95 image | Switch | Worth it now? |
|---|---|---|---|
| `RDebug::Print` (user) → kernel debug port | **already on**: lines such as `MobileCrashData: E32Main()`, `CMCMobilePhoneInfo::…`, the camera list and `EDMHAnim…` appear in `serial.log` | none needed | already used |
| Kernel `__KTRACE_OPT` | our own kernel (udeb), masks via `--trace` / `scripts/hybrid-boot.sh --trace MASK` | trace mask | already used |
| `RFileLogger` / FLogger (`C:\Logs\<dir>\<file>`, written only if the directory exists) | linked by ~20 apps (licensing, MMS, the browser engine, the video-telephony logger, Smart2Go, …), **no boot or telephony component** | create `C:\Logs\<dir>` on the NVMemory drive | later, per app |
| Comms debug utility (`comsdbgutil.dll`, `commsdbg.ini`) | only `packetlogger.dll` links it; ETel/C32/ESock release builds have their logging compiled out | `commsdbg.ini` | no |
| `BTrace` (user → kernel trace buffer) | used only by Nokia MobileCrash (`mc_useragent.exe`, `mc_isiserver.exe`, `cmcdatastoreapi_usermode.dll`) | a kernel BTrace handler | no |
| Nokia trace stack: `sti.dll`, `STILDD.LDD`, `tracecore_sti_if.dll`, `stiwriter.dll` (System Trace Interface over the OMAP XTI port) | kernel side only (imported by `STILDD.LDD`, `KernelSampler.DLL`); we do not run Nokia's kernel side. Activation needed Nokia R&D tools (TraceViewer); release components hardly emit anything | Nokia R&D trace activation | no (stand-in would carry next to nothing) |
| WSERV log (`wsini.ini` LOG keyword + logging DLL) | logging DLL not in the ROM | wsini.ini + DLL | no |
| MobileCrash (Nokia crash reporter) | **broken here**: `mc_useragent.exe` dies with `KERN-EXEC 0` at ~3 s | fix the bad handle | **maybe**: would give Nokia-format crash reports for every panic |

Public references agree: Symbian release binaries carry no textual logging; one swaps in debug
binaries or rebuilds with tracing enabled (Comms/ULogger docs), and `RDebug` routing is an
emulator (`epoc.ini`) setting. Sources:
[ULogger with Comms](https://devlib.symbian.slions.net/s3/GUID-ABE77283-EED8-5A33-B574-3B771EF11086.html),
[S60 3rd general-purpose logging](https://docs.huihoo.com/symbian/s60-3rd-edition-cpp-developers-library-v1.1/GUID-35228542-8C95-4849-A73F-2B4F082F0C44/html/SDL_93/doc_source/ToolsAndUtilities93/GeneralPurposeLogging/generalpurposelogging.html),
[UTrace overview](https://docs.huihoo.com/symbian/s60-5th-edition-cpp-developers-library-v2.1/GUID-35228542-8C95-4849-A73F-2B4F082F0C44/sdk/doc_source/ToolsAndUtilities94/UTraceSolution/utrace.overview.html),
[TraceViewer thesis (Nokia/Carbide trace tooling)](https://trepo.tuni.fi/handle/10024/78164).

Our own instrumentation (`SYBPROP`, `ISCPEER`, `SYBSTUB`, IPC trace, `syb_gdb`) gives more than the
firmware could. The only cheap extra is MobileCrash, after fixing its `KERN-EXEC 0`.
