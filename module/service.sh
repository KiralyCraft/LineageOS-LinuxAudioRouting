#!/system/bin/sh
MODDIR=${0%/*}
AUDIO_RUNTIME=/dev/linux-audio-routing
umask 077
mkdir -p "$AUDIO_RUNTIME"
chmod 0700 "$AUDIO_RUNTIME"
audio_start_broker() {
    "$MODDIR/bin/linux-audiod" --linux-uid 4000 > "$AUDIO_RUNTIME/broker.log" 2>&1 &
    AUDIO_PID=$!
}
# Match the HDMI module: the root broker starts independently of package setup.
audio_start_broker
trap 'kill "$AUDIO_PID" 2>/dev/null; wait "$AUDIO_PID" 2>/dev/null; exit 0' TERM INT
until [ "$(getprop sys.boot_completed)" = 1 ]; do
    kill -0 "$AUDIO_PID" 2>/dev/null || exit 1
    sleep 1
done
. "$MODDIR/apk.sh"
# Installation failure must not disable a manually installed helper.
audio_install_apk "$MODDIR/app/LinuxAudio.apk" "$MODDIR/installed-apk.sha256" "$AUDIO_RUNTIME/apk-install.log"
# The app is not started and permissions are not granted automatically.
while [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; do
    wait "$AUDIO_PID"
    [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || break
    sleep 1
    audio_start_broker
done
