# qembian on Android

A phone (first the Nokia 808 PureView) full screen on an Android phone: QEMU with our syborg board runs
inside the app (`libqemu-syborg.so`), draws straight into the app's window (syborg-gl `present=window` on
its GL thread: no frame copies), and gets touches, the front key strip (Call, Menu, End, drawn after the
808's own strip), the volume keys and a floating button (side keys, Exit). Plan and decisions:
`docs/research/android-port.md`.

Everything Android-specific is in this directory, so the release export can leave it out with one
exclude (`android/`). Outside it there are only generic hooks: syborg-gl's `present` property and its
Android EGL display (`#ifdef __ANDROID__`), and builds without FFmpeg (`SYBORG_NO_FFMPEG`).

## Build and install (PC, phone on USB with USB debugging)

```
android/native/build.sh                       # QEMU + board for arm64 (NDK r28c from ~/Android/Sdk)
(cd android && ./gradlew assembleDebug)       # the app
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
python3 android/tools/make_bundle.py nokia-808-rm807   # the phone's ROM, drives, QEMU arguments
android/tools/push_bundle.sh nokia-808-rm807           # into the app's own storage (about 10 s over USB 3)
```
Then start "qembian" on the phone, unlocked and in front: the first start copies the C: drive (a few
seconds) and boots. `adb logcat -s qembian` shows QEMU's output; the phone's serial log is
`adb shell run-as org.qembian.phone cat files/devices/nokia-808-rm807/serial.log`.

## Layout

| Path | What |
|---|---|
| `native/build.sh` | glib, pixman (static), libsyvg, QEMU 11.1.3 + board, relinked as a shared library |
| `native/syborg_android.c` | JNI: start QEMU on a thread, stderr to logcat, touch/keys/exit as main-loop bottom halves, the window |
| `native/gen_epoxy_shim.py` | `<epoxy/*.h>` for the NDK: core GL from libGLESv1_CM/v3, extensions through eglGetProcAddress |
| `native/patches/` | patches for the Android QEMU tree only |
| `app/` | Kotlin, one Activity (`MainActivity`), the key strip (`KeyStrip`), the JNI bindings (`Native`) |
| `tools/make_bundle.py` | a device bundle from the device model (`tools/device.py`): files + `qemu-args.txt` + `bundle.json` |
| `tools/push_bundle.sh` | the bundle into the app's storage |
