# Linux Audio Routing

Android owns the phone's audio hardware, Bluetooth stack and call routing. A visible Android foreground helper supplies playback/capture endpoints to a separate Linux PipeWire server. Linux applications use its native PipeWire or PulseAudio API; pavucontrol provides device selection and application volume. The HDMI broker and GPU stack are independent.

This is a first hardware-test candidate, not a claim that every phone route is validated. Native sanitizer tests and isolated graph tests are recorded in the release manifest. Real Bluetooth/wired playback, microphone, calls, unplug, doze and long-running clock stability require testing on the phone after installation.

## Transport

A root broker authenticates Linux UID 4000 and the exact Android helper package UID using SO_PEERCRED. It creates a Unix socketpair and passes its endpoints via SCM_RIGHTS. After handoff, the peers negotiate a shared PCM mapping or the legacy socket protocol; the broker handles control only and sleeps until control events or outstanding request deadlines.

The transport copies **uncompressed, unchanged PCM bytes**. Media endpoints use float32 little-endian; Bluetooth SCO uses signed 16-bit little-endian mono. There is no codec, gain adjustment, DSP or sample conversion in the broker, framing, socket worker or FIFO. Byte-level tests include arbitrary float bit patterns and wrapped buffers. With matching 0.1.6 peers, a sealed-size memfd ring carries PCM and eventfds carry wakeups; the socket carries descriptor setup and lifetime only. The native C backend calls AAudio directly on mapped sample spans, avoiding socket payloads and Java PCM arrays. The legacy path uses 40-byte packet headers and at most 10 ms of samples. Native consumption counters or legacy AudioTrack acknowledgements provide buffer credits; playback position independently controls drain; there is no audio pacing sleep in the streaming path.

PipeWire handles ordinary mixing, Linux volume and device-clock rate adaptation. Android AAudio/AudioTrack/AudioRecord, its HAL and Bluetooth may also convert formats or use a Bluetooth codec. Therefore “lossless” refers to the Android/Linux PCM transport, not a Bluetooth radio link or every mixer. Phone capture requests Android's unprocessed source where supported, otherwise voice recognition. Headset microphone mode uses Android's communication source and may include platform processing; the bridge enables no additional audio effects.

Queues are bounded: 20 ms playback credit, an 80 ms playback FIFO, a capture FIFO with 80 ms of burst capacity plus its prefill, and an 80 ms pending queue for hardware-driven endpoints. Older helpers retain a 1500 ms compatibility queue, which can add startup latency. The matched 0.1.5 pair waits for route readiness before requesting graph audio. Queue overruns, malformed packets, sequence/epoch mismatch and lost routes stop the stream and report an error. Capture begins after 20 ms of queued data (40 ms for SCO, covering two observed 20 ms HAL blocks). A temporary empty capture FIFO keeps its samples and recorder, reports no data to PipeWire, and re-primes; PipeWire may expose a gap while actual samples arrive. The bridge does not insert silence or discard captured bytes. Normal playback idle drains its tail; call, unplug and stop intentionally retire the old epoch. A hardware route may have additional buffering, especially Bluetooth.

## Install and start

1. Install `LinuxAudioRouting-0.1.1.zip` through Magisk, then reboot. This is a separate module; it does not replace the HDMI module or any driver. Its boot service verifies an already matching APK and otherwise installs/updates the ordinary signed `dev.kiraly.linuxaudio` app. `LinuxAudio.apk` is also provided for manual installation. An APK installation failure is logged and does not prevent the broker from starting.
2. Open **Linux Audio**, grant the requested microphone, nearby-device, phone-state and notification permissions, and tap **Start Linux audio**. Start once after each reboot, or use its Quick Settings tile. The service is deliberately not automatically started or granted permissions by root. Capture opens only when a Linux recorder uses a source. A partial wake lock exists only while streams are active. The microphone foreground service continues capturing with the Android activity in the background; Android Settings can still show the grant as "Allow while using the app". Stopping the helper also stops capture.
3. In the chroot, install required packages if missing: `pipewire pipewire-pulse wireplumber libpulse pavucontrol`. Desktop integration also uses `pasystray` and `pipewire-alsa`. Run the bundle's `./install-linux.sh` as the desktop user. It installs a versioned private runtime and commands under `~/.local/bin`.
4. Test without changing session defaults:

   ```sh
   ~/.local/bin/linux-audio status
   ~/.local/bin/linux-audio mixer
   ~/.local/bin/with-linux-audio firefox
   ```

   A previously running application retains its previous audio connection. Use a new application/process for the initial test.
5. After hardware validation, `./install-linux.sh --activate-default` enables the private endpoint for future bash/Openbox/LXDE sessions, starts the volume/device tray in LXDE, and adds missing volume-key bindings to both Openbox profiles (`lxde-rc.xml` and `rc.xml`). It backs up every changed file and preserves existing key bindings. PulseAudio, native PipeWire and ALSA client defaults also connect to the bridge, including applications without inherited shell exports. Restart the desktop when convenient. LXSession's native `Environment_variable` section overrides stale inherited audio variables before launching the panel or applications; the tray updates only audio variables in the session's DBus activation environment. DISPLAY, graphics settings, XDG_RUNTIME_DIR and the DBus address are preserved.

   If Termux's old PulseAudio startup has already been removed, `./install-linux.sh --activate-default --remove-legacy-termux` also removes its known obsolete Pulse exports from the user's shell configuration. Other earlier audio exports are retained as comments, and all changed files are backed up. This installer never edits Termux-native files or stops a daemon automatically.

The private server uses `/tmp/linux-audio-<UID>/linux-audio` and `/tmp/linux-audio-<UID>/pulse/native`. It preserves the desktop's XDG_RUNTIME_DIR and DBus environment. Its WirePlumber profile runs policy only, without ALSA/BlueZ/video hardware monitors. Startup probes the PipeWire and Pulse protocols rather than trusting existing socket files. Diagnostics and server state remain on tmpfs/zram; no per-frame SD-card logging occurs.

## Select devices, volume and Bluetooth profiles

Use the **Sound Settings** desktop menu entry or `linux-audio mixer`. pavucontrol's Output Devices/Input Devices tabs choose the default endpoint; Playback/Recording move individual applications. Device choices are separate virtual nodes rather than ALSA cards. Alternative phone microphones share one raw capture path; a conflicting selection is marked busy rather than opening a silent second recorder. Multiple Linux applications can use the same source concurrently. Phone raw capture and headset communication capture use separate paths. Stereo/headset routes are visibly named.

The LXDE tray icon opens a device/application menu. Scroll over it for volume, middle-click to mute, and Ctrl-click for Sound Settings. `linux-audio tray` starts it manually in an existing desktop. The tray and mixer are standard PulseAudio clients of PipeWire; no LXDE fork or duplicate audio transport is needed. Starting them also starts the private server if necessary. The Android helper must still be started visibly once per reboot.

`linux-audio volume up`, `down` and `mute` adjust the private Linux default sink, capped at 100%. No Android media/call volume API is called. Use pavucontrol for per-application volume.

`linux-audioctl list` shows available endpoint keys. `linux-audio profile ENDPOINT headset` or `stereo` selects a Bluetooth group's mode explicitly. Opening its headset microphone/playback requests Android's communication device and suspends stereo for that group. Explicit Stereo selection closes headset streams and prevents an application from reopening the headset microphone until Headset is selected again. The selected profile is held for the running helper; it is not persisted across helper restarts. For automatic headset use, Android communication ownership is released when the last headset stream closes; an explicitly selected Headset profile stays selected until Stereo is chosen or the device disconnects. A Linux application must select the corresponding virtual source/sink; pavucontrol does not expose these as an ALSA Configuration-tab profile.

On headphone disconnect, the selected node stays present but unavailable, so it does not automatically become the speaker node. The helper stops its track when its verified route changes. Android can reroute already-buffered hardware audio before an app callback; preventing that system-level transition absolutely would require a lower-level Android policy change.

Android calls/focus priority close Linux streams and release this helper's communication request. Fresh epochs are opened after priority ends. No call recording is performed. Grant phone-state permission for ringing-call detection; audio focus/mode changes also provide protection.

## Stop and rollback

`linux-audio stop` stops only this private server and its children; the Android app's Stop button releases its audio streams. Disable/remove this separate Magisk module and reboot to remove the broker. The app may be uninstalled normally.

Default migration backs up shell/desktop and audio-client files under `~/.local/state/linux-audio-routing/backup-<timestamp>/`. Restore the files listed in `files.txt` to undo the migration; remove newly created files only if listed and absent in the backup. For an unmodified session with a still-running Termux server, launch with the legacy `PULSE_SERVER=unix:/hostMounts/chrootBind/pulseAudio.socket`. This project does not modify Termux's PulseAudio startup files or stop its daemon automatically.

## Build

All release builds for this device run at `root@192.168.104.201`, with the Android SDK36 build tools, Android NDK29 (`aarch64-linux-android35-clang`), Java17 and the existing `mesa-arm-builder` AArch64 container. A manual aapt2/javac/d8/zipalign/apksigner build avoids a network-dependent Gradle bootstrap; conventional Android Gradle files are included too.

```sh
build-support/build.sh SOURCE OUTPUT
# Run isolated tests with tests/integration.py in the AArch64 builder.
build-support/package.py SOURCE OUTPUT RELEASE_DIRECTORY
```

The private signing key stays on the build server and is never packaged. The manifest records source/artifact hashes, compiler versions, certificate identity and test status. Project native code is C99 and uses `.clang-format`; cJSON 1.7.19 is vendored with its upstream license.

### 0.1.5 latency candidate

This release coordinates Android-driven graph demand, adaptive effective
AudioTrack buffers, and native presentation timestamps propagated into PipeWire
latency reporting. It does not promise to eliminate Bluetooth codec/transport
delay; correct reporting lets applications account for that delay when
synchronizing video. The PCM transport remains unchanged and uncompressed.

Use the matched APK and Linux bundle. The existing broker passes PCM descriptors
and negotiated metadata unchanged, so this candidate does not require a Magisk
update for live testing. After installing the APK and tapping Start, restart the
Linux bridge to pick up the new endpoint capability. Runtime pacing is enabled
for capable helpers, with `LINUX_AUDIO_CLOCK_DRIVER=0` as the compatibility
opt-out. `LINUX_AUDIO_TIMING=1` enables bounded counter/latency diagnostics.

Validation separates isolated graph tests, root Android policy probes, and
ordinary-helper live tests. See `diagnostics/android-routing/LATENCY-20261009.md`;
YouTube synchronization, simultaneous physical microphones, calls, unplug and
long-running stability still require validation with the matched installed pair.

### 0.1.6 shared-memory candidate

The matched APK and bridge negotiate shared PCM by default. Set
`LINUX_AUDIO_SHARED_PCM=0` in the bridge environment to retain the legacy socket
and Java audio path. An older helper or bridge also retains the old path. A
failed native open is reported rather than silently changing the selected device.

This removes interprocess sample serialization and Java staging, not every copy
in the audio stack. PipeWire still transfers samples to/from the shared ring;
AAudio shared mode may mix/copy internally, and the HAL/codec remains Android's.
AAudio requests shared low-latency mode, with exact application format validation,
explicit device selection, and a two-burst effective buffer. Actual MMAP use is
logged by the helper; it is not assumed from a successful open.

Hardware I/O and event readiness drive production. Bounded native waits permit
cancellation; a 5 ms observation interval only retires an idle playback tail.
Microphone pause immediately quiesces the producer before delayed manager cleanup.
No sample dropping, gain adjustment or conversion is added to the bridge.

Native Bluetooth presentation timestamps are withheld until their relationship
to codec/headset delay is validated. A low MMAP timestamp does not prove low
acoustic latency. The user's SBC selection is preserved; Android's negotiated
codec must be recorded for latency comparisons. Ordinary-app routing, capture,
call recovery, headset latency and long-running stability remain live-test gates.

### 0.1.7 allocation ownership correction

The first ordinary-app 0.1.6 test opened AAudio MMAP successfully, but enforcing
SELinux rejected the Linux-created memfd (`tmpfs` label). Version 0.1.7 creates
and exports the ring in the Android helper, using the existing app-domain tmpfs
transition. Linux validates and imports it. No SELinux rules or enforcement mode
are changed. Allocation ownership is negotiated explicitly, so mixed 0.1.6 and
0.1.7 pairs choose the legacy socket path instead of disagreeing about which peer
sends descriptors. Installed-helper tests passed for playback, all four microphones, simultaneous
speaker/stereo streams and return from headset mode. See
`diagnostics/android-routing/live-017-20261009.json`; audible quality, game latency,
calls/unplug and long-duration behavior remain separate acceptance checks.
