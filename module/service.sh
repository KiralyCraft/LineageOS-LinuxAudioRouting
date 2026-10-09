#!/system/bin/sh
MODDIR=${0%/*}
AUDIO_RUNTIME=/dev/linux-audio-routing
umask 077
mkdir -p "$AUDIO_RUNTIME"
chmod 0700 "$AUDIO_RUNTIME"
while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 1; done
AUDIO_APK=$MODDIR/app/LinuxAudio.apk
AUDIO_HASH=$(sha256sum "$AUDIO_APK" | cut -d ' ' -f 1)
AUDIO_OLD=$(cat "$MODDIR/installed-apk.sha256" 2>/dev/null)
if [ "$AUDIO_HASH" != "$AUDIO_OLD" ] || ! pm path dev.kiraly.linuxaudio >/dev/null 2>&1; then
    if pm install -r --user 0 "$AUDIO_APK" > "$AUDIO_RUNTIME/apk-install.log" 2>&1; then
        printf '%s\n' "$AUDIO_HASH" > "$MODDIR/installed-apk.sha256"
    else
        exit 1
    fi
fi
# The app is not started and permissions are not granted automatically.
while [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; do
    "$MODDIR/bin/linux-audiod" --linux-uid 4000 > "$AUDIO_RUNTIME/broker.log" 2>&1 &
    AUDIO_PID=$!
    trap 'kill "$AUDIO_PID" 2>/dev/null; wait "$AUDIO_PID" 2>/dev/null; exit 0' TERM INT
    wait "$AUDIO_PID"
    sleep 1
 done
