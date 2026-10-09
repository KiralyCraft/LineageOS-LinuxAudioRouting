# Linux Audio Routing

Android owns the phone's audio hardware, Bluetooth stack and call routing. A visible Android foreground helper supplies playback/capture endpoints to a separate Linux PipeWire server. Linux applications use its native PipeWire or PulseAudio API; pavucontrol provides device selection and application volume. The HDMI broker and GPU stack are independent.

This is a first hardware-test candidate, not a claim that every phone route is validated. Native sanitizer tests and isolated graph tests are recorded in the release manifest. Real Bluetooth/wired playback, microphone, calls, unplug, doze and long-running clock stability require testing on the phone after installation.

## Transport

A root broker authenticates Linux UID 4000 and the exact Android helper package UID using SO_PEERCRED. It creates a Unix socketpair and passes its endpoints via SCM_RIGHTS. After handoff, audio goes directly between the helper and Linux; the broker handles control only.

The transport copies **uncompressed, unchanged PCM bytes**. Media endpoints use float32 little-endian; Bluetooth SCO uses signed 16-bit little-endian mono. There is no codec, gain adjustment, DSP or sample conversion in the broker, framing, socket worker or FIFO. Byte-level tests include arbitrary float bit patterns and wrapped buffers. The only packet overhead is a 40-byte header; packets contain at most 10 ms of samples. Android position notifications provide playback credits; there is no audio pacing sleep in the streaming path.

PipeWire handles ordinary mixing, Linux volume and device-clock rate adaptation. Android AudioTrack/AudioRecord, its HAL and Bluetooth may also convert formats or use a Bluetooth codec. Therefore “lossless” refers to the Android/Linux PCM transport, not a Bluetooth radio link or every mixer. Capture requests Android's unprocessed source where supported, otherwise voice recognition; the helper enables no audio effects.

Queues are bounded: 20 ms playback credit, an 80 ms Linux FIFO, and a separate 500 ms maximum cold-start queue (normally empty after startup). The steady-state path bypasses that queue. Queue overruns, malformed packets, sequence/epoch mismatch and lost routes stop the stream and report an error. Normal playback idle drains its tail; call, unplug and stop intentionally retire the old epoch. A hardware route may have additional buffering, especially Bluetooth.

## Install and start

1. Install `LinuxAudioRouting-0.1.0.zip` through Magisk, then reboot. This is a separate module; it does not replace the HDMI module or any driver. Its boot service installs/updates the ordinary signed `dev.kiraly.linuxaudio` app. `LinuxAudio.apk` is also provided for manual installation.
2. Open **Linux Audio**, grant the requested microphone, nearby-device, phone-state and notification permissions, and tap **Start Linux audio**. Start once after each reboot, or use its Quick Settings tile. The service is deliberately not automatically started or granted permissions by root. Capture opens only when a Linux recorder uses a source. A partial wake lock exists only while streams are active.
3. In the chroot, install required packages if missing: `pipewire pipewire-pulse wireplumber libpulse pavucontrol` (optional `pasystray`). Run the bundle's `./install-linux.sh` as the desktop user. It installs a versioned private runtime and commands under `~/.local/bin`.
4. Test without changing session defaults:

   ```sh
   ~/.local/bin/linux-audio status
   ~/.local/bin/linux-audio mixer
   ~/.local/bin/with-linux-audio firefox
   ```

   A previously running application retains its previous audio connection. Use a new application/process for the initial test.
5. After hardware validation, `./install-linux.sh --activate-default` enables the private endpoint for future bash/Openbox/LXDE sessions and adds missing Openbox volume-key bindings. It backs up every changed file and preserves existing key bindings. Restart the desktop when convenient. An HDMI launcher that honors an inherited PULSE_SERVER can be launched through `with-linux-audio`; no HDMI patch is required.

The private server uses `/tmp/linux-audio-<UID>/linux-audio` and `/tmp/linux-audio-<UID>/pulse/native`. It preserves the desktop's XDG_RUNTIME_DIR and DBus environment. Its WirePlumber profile runs policy only, without ALSA/BlueZ/video hardware monitors. Diagnostics and server state remain on tmpfs/zram; no per-frame SD-card logging occurs.

## Select devices, volume and Bluetooth profiles

Use the Linux Audio Devices desktop menu entry or `linux-audio mixer`. pavucontrol's Output Devices/Input Devices tabs choose the default endpoint; Playback/Recording move individual applications. Device choices are separate virtual nodes rather than ALSA cards. Stereo/headset routes are visibly named.

`linux-audio volume up`, `down` and `mute` adjust the private Linux default sink, capped at 100%. No Android media/call volume API is called. Use pavucontrol for per-application volume.

`linux-audioctl list` shows available endpoint keys. `linux-audio profile ENDPOINT headset` or `stereo` selects a Bluetooth group's mode explicitly. Opening its headset microphone/playback requests Android's communication device and suspends stereo for that group. Android restores normal communication ownership when the last headset stream closes. A Linux application must select the corresponding virtual source/sink; pavucontrol does not expose these as an ALSA Configuration-tab profile.

On headphone disconnect, the selected node stays present but unavailable, so it does not automatically become the speaker node. The helper stops its track when its verified route changes. Android can reroute already-buffered hardware audio before an app callback; preventing that system-level transition absolutely would require a lower-level Android policy change.

Android calls/focus priority close Linux streams and release this helper's communication request. Fresh epochs are opened after priority ends. No call recording is performed. Grant phone-state permission for ringing-call detection; audio focus/mode changes also provide protection.

## Stop and rollback

`linux-audio stop` stops only this private server and its children; the Android app's Stop button releases its audio streams. Disable/remove this separate Magisk module and reboot to remove the broker. The app may be uninstalled normally.

Default migration backs up shell/desktop files under `~/.local/state/linux-audio-routing/backup-<timestamp>/`. Restore the files listed in `files.txt` to undo the migration; remove newly created files only if listed and absent in the backup. For an unmodified session, launch with the legacy `PULSE_SERVER=unix:/hostMounts/chrootBind/pulseAudio.socket`. This project does not modify Termux's PulseAudio startup files or stop its daemon automatically.

## Build

All release builds for this device run at `root@192.168.104.201`, with the Android SDK36 build tools, Android NDK29 (`aarch64-linux-android35-clang`), Java17 and the existing `mesa-arm-builder` AArch64 container. A manual aapt2/javac/d8/zipalign/apksigner build avoids a network-dependent Gradle bootstrap; conventional Android Gradle files are included too.

```sh
build-support/build.sh SOURCE OUTPUT
# Run isolated tests with tests/integration.py in the AArch64 builder.
build-support/package.py SOURCE OUTPUT RELEASE_DIRECTORY
```

The private signing key stays on the build server and is never packaged. The manifest records source/artifact hashes, compiler versions, certificate identity and test status. Project native code is C99 and uses `.clang-format`; cJSON 1.7.19 is vendored with its upstream license.
