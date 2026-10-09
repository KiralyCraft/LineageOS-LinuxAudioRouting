#!/usr/bin/env bash
set -Eeuo pipefail
AUDIO_BUNDLE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
AUDIO_VERSION=$(cat "$AUDIO_BUNDLE/linux/VERSION")
[[ $AUDIO_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 1
AUDIO_PREFIX=$HOME/.local/lib/linux-audio-routing/$AUDIO_VERSION
[[ $(id -u) != 0 ]] || { printf 'Run as the desktop user, not root.\n' >&2; exit 1; }
if [[ -f $AUDIO_BUNDLE/SHA256SUMS ]]; then (cd "$AUDIO_BUNDLE" && sha256sum -c SHA256SUMS >/dev/null); fi
for dependency in pipewire pipewire-pulse wireplumber pw-cli wpctl pactl pavucontrol python3; do
    command -v "$dependency" >/dev/null || { printf 'Missing %s; install pipewire pipewire-pulse wireplumber libpulse pavucontrol first.\n' "$dependency" >&2; exit 1; }
done
mkdir -p "$AUDIO_PREFIX" "$HOME/.local/bin" "$HOME/.local/share/applications"
python3 "$AUDIO_BUNDLE/linux/install-files.py" "$AUDIO_BUNDLE/linux" "$AUDIO_PREFIX"
for command in linux-audio with-linux-audio linux-audioctl; do
    AUDIO_LINK=$HOME/.local/bin/$command
    if [[ -e $AUDIO_LINK || -L $AUDIO_LINK ]]; then
        [[ -L $AUDIO_LINK && $(readlink "$AUDIO_LINK") == "$HOME/.local/lib/linux-audio-routing/"* ]] || { printf 'Preserving unrelated command %s\n' "$AUDIO_LINK" >&2; exit 1; }
    fi
    ln -sfn "$AUDIO_PREFIX/bin/$command" "$AUDIO_LINK"
done
cat > "$HOME/.local/share/applications/linux-audio-mixer.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Linux Audio Devices
Comment=Select Android devices and adjust Linux application volume
Exec=$AUDIO_PREFIX/bin/linux-audio mixer
Icon=audio-headphones
Categories=AudioVideo;Mixer;
Terminal=false
DESKTOP
printf 'Installed. Start the Android helper, then launch apps with:\n  %s/bin/with-linux-audio APPLICATION\nMixer:\n  %s/bin/linux-audio mixer\n' "$AUDIO_PREFIX" "$AUDIO_PREFIX"
if [[ ${1:-} == --activate-default ]]; then
    # Only explicit migration changes the user's shell/desktop configuration.
    python3 "$AUDIO_PREFIX/configure-desktop.py" "$AUDIO_PREFIX"
fi
