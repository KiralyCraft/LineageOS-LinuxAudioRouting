#!/usr/bin/env bash
set -Eeuo pipefail
AUDIO_SOURCE=${1:?source checkout}
AUDIO_BUILD=${2:?build output}
AUDIO_SDK=${AUDIO_SDK:-/bigdata/android-sdk}
AUDIO_NDK=$AUDIO_SDK/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin
AUDIO_TOOLS=$AUDIO_SDK/build-tools/36.0.0
AUDIO_JAR=$AUDIO_SDK/platforms/android-36/android.jar
AUDIO_SIGNING=${AUDIO_SIGNING:-/bigdata/linux-audio-routing/signing}
mkdir -p "$AUDIO_BUILD/native" "$AUDIO_BUILD/android/generated" "$AUDIO_BUILD/android/classes" "$AUDIO_BUILD/android/dex" "$AUDIO_BUILD/tests"
CFLAGS=(-std=c99 -O2 -Wall -Wextra -Werror)
COMMON=("$AUDIO_SOURCE/protocol.c" "$AUDIO_SOURCE/vendor/cJSON.c")
"$AUDIO_NDK/aarch64-linux-android35-clang" "${CFLAGS[@]}" -fPIE -pie "$AUDIO_SOURCE/broker.c" "$AUDIO_SOURCE/framing.c" "${COMMON[@]}" -lm -o "$AUDIO_BUILD/native/linux-audiod"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer "$AUDIO_SOURCE/broker.c" "$AUDIO_SOURCE/framing.c" "${COMMON[@]}" -lm -o "$AUDIO_BUILD/tests/broker-host"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer "$AUDIO_SOURCE/tests/unit.c" "$AUDIO_SOURCE/framing.c" "$AUDIO_SOURCE/ring.c" "${COMMON[@]}" -pthread -lm -o "$AUDIO_BUILD/tests/unit"
"$AUDIO_BUILD/tests/unit" > "$AUDIO_BUILD/tests/unit.log" 2>&1
python3 "$AUDIO_SOURCE/tests/broker.py" "$AUDIO_BUILD/tests/broker-host" > "$AUDIO_BUILD/tests/broker.log" 2>&1
python3 "$AUDIO_SOURCE/tests/desktop.py" "$AUDIO_SOURCE" > "$AUDIO_BUILD/tests/desktop.log" 2>&1
# The persistent AArch64 builder mounts the package-combined parent at /build.
case "$AUDIO_SOURCE" in
    /bigdata/mesa-sync-build/package-combined/*) ;;
    *) printf 'Source must be under the AArch64 builder mounted directory\n' >&2; exit 2 ;;
esac
AUDIO_ARM_SOURCE=/build/${AUDIO_SOURCE#/bigdata/mesa-sync-build/package-combined/}
AUDIO_ARM_BUILD=/build/${AUDIO_BUILD#/bigdata/mesa-sync-build/package-combined/}
docker exec -e AUDIO_SKIP_GRAPH="${AUDIO_SKIP_GRAPH:-0}" mesa-arm-builder bash "$AUDIO_ARM_SOURCE/build-support/build-linux.sh" "$AUDIO_ARM_SOURCE" "$AUDIO_ARM_BUILD"

"$AUDIO_TOOLS/aapt2" compile --dir "$AUDIO_SOURCE/android/app/src/main/res" -o "$AUDIO_BUILD/android/resources.zip"
"$AUDIO_TOOLS/aapt2" link -o "$AUDIO_BUILD/android/base.apk" -I "$AUDIO_JAR" --manifest "$AUDIO_SOURCE/android/app/src/main/AndroidManifest.xml" --java "$AUDIO_BUILD/android/generated" --version-code 1 --version-name 0.1.0 "$AUDIO_BUILD/android/resources.zip"
mapfile -t AUDIO_JAVA < <(find "$AUDIO_SOURCE/android/app/src/main/java" "$AUDIO_BUILD/android/generated" -name '*.java' -type f)
javac --release 17 -cp "$AUDIO_JAR" -d "$AUDIO_BUILD/android/classes" "${AUDIO_JAVA[@]}"
mapfile -t AUDIO_CLASSES < <(find "$AUDIO_BUILD/android/classes" -name '*.class' -type f)
"$AUDIO_TOOLS/d8" --min-api 35 --lib "$AUDIO_JAR" --output "$AUDIO_BUILD/android/dex" "${AUDIO_CLASSES[@]}"
cp "$AUDIO_BUILD/android/base.apk" "$AUDIO_BUILD/android/unsigned.apk"
(cd "$AUDIO_BUILD/android/dex" && zip -q "$AUDIO_BUILD/android/unsigned.apk" classes*.dex)
"$AUDIO_TOOLS/zipalign" -P 16 -f 4 "$AUDIO_BUILD/android/unsigned.apk" "$AUDIO_BUILD/android/aligned.apk"
if [[ ! -e "$AUDIO_SIGNING/linux-audio.keystore" ]]; then
    install -d -m 0700 "$AUDIO_SIGNING"
    umask 077
    openssl rand -hex 24 > "$AUDIO_SIGNING/password"
    keytool -genkeypair -keystore "$AUDIO_SIGNING/linux-audio.keystore" -storepass:file "$AUDIO_SIGNING/password" -keypass:file "$AUDIO_SIGNING/password" -alias linux-audio -dname 'CN=KiralyCraft Linux Audio' -keyalg RSA -keysize 4096 -validity 10950 > "$AUDIO_BUILD/android/signing.log" 2>&1
fi
"$AUDIO_TOOLS/apksigner" sign --ks "$AUDIO_SIGNING/linux-audio.keystore" --ks-pass "file:$AUDIO_SIGNING/password" --out "$AUDIO_BUILD/android/LinuxAudio.apk" "$AUDIO_BUILD/android/aligned.apk"
"$AUDIO_TOOLS/apksigner" verify --verbose --print-certs "$AUDIO_BUILD/android/LinuxAudio.apk" > "$AUDIO_BUILD/android/verification.txt"
python3 "$AUDIO_SOURCE/build-support/manifest.py" "$AUDIO_SOURCE" "$AUDIO_BUILD"
