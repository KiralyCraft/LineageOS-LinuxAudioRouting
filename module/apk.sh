#!/system/bin/sh
# Binder package commands must receive pipe-backed stdout/stderr, not a /dev log FD.
audio_install_apk() {
    AUDIO_APK=$1
    AUDIO_STATE=$2
    AUDIO_LOG=$3
    AUDIO_HASH=$(sha256sum "$AUDIO_APK" | cut -d ' ' -f 1)
    AUDIO_INSTALLED=$(pm path dev.kiraly.linuxaudio 2>/dev/null)
    AUDIO_PATH=${AUDIO_INSTALLED#package:}
    if [ -f "$AUDIO_PATH" ] && [ "$(sha256sum "$AUDIO_PATH" | cut -d ' ' -f 1)" = "$AUDIO_HASH" ]; then
        printf '%s\n' "$AUDIO_HASH" > "$AUDIO_STATE"
        return 0
    fi
    AUDIO_BYTES=$(stat -c %s "$AUDIO_APK")
    if AUDIO_RESULT=$(pm install -r --user 0 -S "$AUDIO_BYTES" - < "$AUDIO_APK" 2>&1); then
        printf '%s\n' "$AUDIO_RESULT" > "$AUDIO_LOG"
        if AUDIO_CHECK=$(pm path dev.kiraly.linuxaudio 2>/dev/null); then
            case "$AUDIO_CHECK" in
                package:*) printf '%s\n' "$AUDIO_HASH" > "$AUDIO_STATE"; return 0 ;;
            esac
        fi
        printf 'Package Manager reported success but the helper package is absent.\n' >> "$AUDIO_LOG"
        return 1
    fi
    printf '%s\nManual APK installation is available; the broker will still start.\n' "$AUDIO_RESULT" > "$AUDIO_LOG"
    return 1
}
