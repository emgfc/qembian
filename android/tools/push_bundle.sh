#!/usr/bin/env bash
# Puts a device bundle (android/tools/make_bundle.py) into the app's own storage on the phone over USB.
#   android/tools/push_bundle.sh DEVICE_ID            (the app must be installed; adb from the SDK)
# adb pushes to /data/local/tmp, then run-as copies it into the app's files/bundles/ (files the app owns;
# a push straight into Android/data is not readable by the app on One UI). Drives are left alone: they live
# in files/devices/DEVICE_ID/ and survive bundle updates.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ID="${1:?device id}"; PKG=org.qembian.phone
ADB="${ADB:-$(command -v adb || echo "$HOME/Android/Sdk/platform-tools/adb")}"
SRC="$ROOT/build/android/bundles/$ID"
[ -f "$SRC/qemu-args.txt" ] || { echo "no bundle: python3 android/tools/make_bundle.py $ID"; exit 1; }
"$ADB" shell am force-stop "$PKG"
"$ADB" shell "rm -rf /data/local/tmp/qembian-bundle && mkdir -p /data/local/tmp/qembian-bundle"
"$ADB" push "$SRC" /data/local/tmp/qembian-bundle/
"$ADB" shell "run-as $PKG sh -c 'mkdir -p files/bundles && rm -rf files/bundles/$ID && cp -r /data/local/tmp/qembian-bundle/$ID files/bundles/$ID'"
"$ADB" shell rm -rf /data/local/tmp/qembian-bundle
"$ADB" shell rm -rf "/sdcard/Android/data/$PKG/files/bundles/$ID"
echo "bundle $ID is on the phone"
