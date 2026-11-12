# re3-symbian (GTA III port): why there is no audio traffic on the N8

Source: https://gitlab.com/shinovon/re3-symbian, branch `master`, commit 653d3134 (2026-05-15).
Static read only (a shallow clone of the repository above).
Line numbers refer to that commit.

## 1. Audio backend: there is none yet

- The Symbian build compiles `src/audio/sampman_symbian.cpp`, guarded by
  `#if defined(__SYMBIAN32__) && !defined(NO_AUDIO)` (line 1). `NO_AUDIO` is defined nowhere
  (`group/*.mm?`, `src/core/config.h`), so this file, not `sampman_null.cpp`, is the backend. [verified]
- Every method of `cSampleManager` in it is a stub: `Initialise` returns TRUE (84-88),
  `InitialiseSampleBanks` TRUE (370-374), `LoadSampleBank` FALSE (137-142), `IsSampleBankLoaded`
  `LOADING_STATUS_NOT_LOADED` (150-156), `StartChannel` empty (320-323), `StartStreamedFile` FALSE
  (325-330), `IsStreamPlaying` FALSE (362-367), `GetStreamedFileLength` 1 (355-360), `Terminate` empty
  (90-94). It includes `<MdaAudioOutputStream.h>` (line 3) and links `mediaclientaudiostream.lib`,
  `mediaclientaudio.lib` (`group/re3.mmh` 16-17) but never instantiates a stream, DevSound, file or
  buffer: no `CMdaAudioOutputStream`, no `MaoscOpenComplete`, no `sfx.raw`/`sfx.sdt` open. [verified]
- OpenAL is off: `config.h:10-12` `#undef AUDIO_OAL`; `sampman_oal.cpp:3` is `#if defined AUDIO_OAL && 0`;
  `EXTERNAL_3D_SOUND` is commented out (`config.h:183,442`), so `MAXCHANNELS` is 0 (`sampman.h:108`). [verified]
- `README.md` ("400 MB free on E:, which will increase once audio support arrives") and `TODO.md`
  ("- [ ] Audio", unchecked) confirm audio is unimplemented. [verified]
- Game-side audio logic still runs: `CGame::Initialise` -> `DMAudio.Initialise()` (`src/core/Game.cpp:334`)
  -> `cAudioManager::Initialise` (`AudioManager.cpp:60-80`) sets `m_bIsInitialised = TRUE` because the
  stub returns TRUE, then `Service()` every frame (`main.cpp:1600,1617`) calls the stubs. [verified]

Conclusion: 75 s with zero DevSound/MMF/driver traffic is exactly what this binary does on a real N8.
Nothing on the emulator side is missing or wrong for audio.

## 2. Does audio init depend on anything that can fail here? No.

- No DevSound capability, rate (engine constants `DIGITALRATE 32000`, 16-bit stereo, `sampman.h:115-117`,
  are unused by the stub), buffer size, thread priority or `Open` completion is touched. [verified]
- The game source contains no `RProperty`, `UserSvr`, `TRawEvent`, `RThread::SetPriority` or
  `HAL::Set` (grep over `src/`); only `User::ResetInactivityTime()` (`symbian.cpp:110,495`), which
  needs no capability. The exe has `CAPABILITY NetworkServices ReadUserData WriteUserData` only
  (`re3.mmh:36`). The P&S Define/Set/Get and SwEvent denials therefore come from system DLLs loaded
  in the process (avkon skin/orientation via `BaseConstructL(EAknEnableSkin)` `symbian.cpp:838`,
  `SetOrientationL` 515, `CFeatureDiscovery`, EGL/GLES driver) and are harmless, self-signed-app
  behaviour that happens on hardware too. [inferred]

## 3. Can the crash at RE3.EXE+0x1ABDBC be audio? Very unlikely.

- The backend owns no buffers or pointers, so a "stream that failed to open" cannot exist. The engine's
  audio layer only sees "no bank loaded / no stream playing" and returns early. [verified]
- A NULL write 75 s in, after the frontend (`GS_FRONTEND` -> `GS_INIT_PLAYING_GAME`, `symbian.cpp:425-447`)
  is more plausibly streaming/texture/model code; the port's own TODO lists an open
  `Texture::destroy` crash. Memory budget: `psInitialize` clamps `_dwMemAvailPhys` to
  `HAL EMemoryRAMFree`, 10 MB floor, 60 MB cap (`symbian.cpp:241-251`); README wants >= 50 MB free.
  Check what `EMemoryRAMFree` reports on our board (a low value changes texture/streaming limits). [inferred]
- To symbolise: build offset 0x1ABDBC against the owner's `re3.exe` (`arm-none-symbianelf-objdump`
  on the E32 image is not enough; needs the `.sym`/ELF from the same Carbide build). [inferred]

## 4. Files and paths on the phone

- Data root is hard-coded: `E:\Data\gta3\` (`symbian.cpp:105`, `chdir` 597/605; `crossplatform.cpp:30`
  `E:/data/gta3/%s`). The SIS installs only fonts/GXT/PARTICLE.CFG there (`sis/re3.pkg`); the user
  copies the original PC `models/`, `data/`, `TEXT/`, `anim/` etc. [verified]
- Core SFX (`audio/sfx.raw` + `audio/sfx.sdt`, names only in dead OAL code `sampman_oal.cpp:86-87`)
  and streams (`AUDIO\MUSIC\*.VB` / `AUDIO\*.wav|mp3`, `sampman.h:272ff`) are NOT read by this build.
  Providing them changes nothing until the author implements `sampman_symbian.cpp`. [verified]

## What to check / provide

- Phone: nothing for audio. Only confirm `E:\Data\gta3\` has the game data, and >= 50 MB free RAM.
- Emulator: nothing for audio now. When the port gains a backend it will almost surely be
  `CMdaAudioOutputStream` (already linked), i.e. the same MMF path our WAV/MP3 playback already uses;
  expect 32000 Hz 16-bit stereo (`DIGITALRATE`) which DevSound must accept (`TMdaAudioDataSettings`
  ESampleRate32000Hz). The P&S denials are not an emulator defect.
