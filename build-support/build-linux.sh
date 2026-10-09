#!/usr/bin/env bash
set -Eeuo pipefail
[[ $(uname -m) == aarch64 ]] || { printf "Run this script in the AArch64 builder container\n" >&2; exit 2; }
AUDIO_SOURCE=${1:?source}
AUDIO_BUILD=${2:?build}
mkdir -p "$AUDIO_BUILD/native" "$AUDIO_BUILD/tests" "$AUDIO_BUILD/objects"
AUDIO_FLAGS=(-std=c99 -O2 -Wall -Wextra -Werror)
AUDIO_HEADERS=$(sha256sum "$AUDIO_SOURCE"/include/*.h "$AUDIO_SOURCE"/vendor/cJSON.h | sha256sum | cut -d ' ' -f 1)
compile_object() {
    local audio_file=$1 audio_extra=${2:-} audio_object=$AUDIO_BUILD/objects/${1//\//_}.o
    local audio_hash
    audio_hash=$(printf '%s\n' "$AUDIO_HEADERS" "${AUDIO_FLAGS[*]}" "$audio_extra" "$(sha256sum "$AUDIO_SOURCE/$audio_file")" | sha256sum | cut -d ' ' -f 1)
    if [[ ! -f $audio_object || ! -f $audio_object.sha256 || $(cat "$audio_object.sha256") != "$audio_hash" ]]; then
        # Extra flags originate only from pkg-config, never user-controlled input.
        gcc "${AUDIO_FLAGS[@]}" $audio_extra -c "$AUDIO_SOURCE/$audio_file" -o "$audio_object"
        printf '%s\n' "$audio_hash" > "$audio_object.sha256"
    fi
}
for audio_file in protocol.c control.c transport.c ring.c framing.c broker.c audioctl.c vendor/cJSON.c tests/transport_test.c; do compile_object "$audio_file"; done
compile_object pipewire.c "$(pkg-config --cflags libpipewire-0.3)"
AUDIO_OBJECT=$AUDIO_BUILD/objects
AUDIO_COMMON=("$AUDIO_OBJECT/protocol.c.o" "$AUDIO_OBJECT/vendor_cJSON.c.o")
AUDIO_CLIENT=("$AUDIO_OBJECT/control.c.o" "${AUDIO_COMMON[@]}")
gcc "$AUDIO_OBJECT/audioctl.c.o" "${AUDIO_CLIENT[@]}" -pthread -lm -o "$AUDIO_BUILD/native/linux-audioctl"
gcc "$AUDIO_OBJECT/pipewire.c.o" "$AUDIO_OBJECT/transport.c.o" "$AUDIO_OBJECT/ring.c.o" "${AUDIO_CLIENT[@]}" $(pkg-config --libs libpipewire-0.3) -pthread -lm -o "$AUDIO_BUILD/native/linux-audio-bridge"
gcc "$AUDIO_OBJECT/broker.c.o" "$AUDIO_OBJECT/framing.c.o" "${AUDIO_COMMON[@]}" -lm -o "$AUDIO_BUILD/tests/broker-arm"
gcc "$AUDIO_OBJECT/tests_transport_test.c.o" "$AUDIO_OBJECT/transport.c.o" "$AUDIO_OBJECT/ring.c.o" "${AUDIO_CLIENT[@]}" -pthread -lm -o "$AUDIO_BUILD/tests/transport-fixture"
if [[ ${AUDIO_SKIP_GRAPH:-0} != 1 ]]; then
    timeout 60 python3 "$AUDIO_SOURCE/tests/integration.py" "$AUDIO_SOURCE" "$AUDIO_BUILD" > "$AUDIO_BUILD/tests/integration.log" 2>&1
fi
pkg-config --modversion libpipewire-0.3 > "$AUDIO_BUILD/native/pipewire-build-version.txt"
