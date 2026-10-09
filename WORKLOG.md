# Audio implementation checkpoint

Repository: KiralyCraft/LineageOS-LinuxAudioRouting, first 0.1.0 hardware-test candidate.

Native project code is C99, Allman/tabs, filename function prefixes and requested underscore naming; headers are in include/. Android public APIs require a Java helper. cJSON 1.7.19 is vendored with its license. No existing HDMI code or drivers were modified.

Transport authenticates Linux UID 4000 and the exact ordinary Android app UID. The broker hands off socketpair FDs once via SCM_RIGHTS, then stays out of the PCM path. float32 LE media and S16 LE SCO bytes are unchanged; no codec, DSP, gain or resampling in the transport. PipeWire provides mixing, per-application routing/volume and clock adaptation. Android keeps hardware/call ownership. Calls/unplug retire epochs; disconnected virtual nodes remain to prevent speaker fallback. Explicit route verification and route-change callbacks stop incorrect Android routing.

Validated on build server root@192.168.104.201: Android NDK35 aarch64 broker, Linux AArch64 C99 binaries, ASan/UBSan ring/protocol/descriptor tests, production broker fixtures. Java APK builds with SDK36 and Java17, signing key stays private on build server. The first APK build's repeated password-file consumption was fixed without regenerating the key.

Isolated production native tests ran on the phone, using fake Android endpoints and separate abstract sockets/private PipeWire runtimes; no AudioTrack/AudioRecord or HDMI hardware was touched. Consecutive final runs passed: arbitrary-bit raw playback/capture and final partial packets, simultaneous native-PipeWire/PulseAPI applications on different devices, moving a live PulseAPI application, capture, retained nodes on unplug, call/reconnect teardown. Build-server QEMU was unsuitable for graph timing. Final proof logs are copied to the release validation folder and manifest.

Cold-start FIFO is separate (500 ms bound), normally bypassed during steady state; steady transport FIFO is 80 ms and playback credit 20 ms. Native drain uses serial acknowledgements to avoid repeated-drain races. Normal idle drains queued PipeWire buffers and raw tails; explicit call/unplug/stop discards the old epoch. Queues never silently drop to catch up. Overruns/discontinuities stop with visible diagnostics.

Linux server supervisor uses private /tmp/linux-audio-UID sockets, preserves XDG_RUNTIME_DIR/DBus, scopes WirePlumber to policy only, and stops its owned child PIDs. Start/stop tested on device without hardware endpoints; legacy Termux PulseAudio remains running. Desktop migration tested only in disposable home: backup, original Pulse export, existing key bindings and idempotence preserved. No real shell/Openbox configuration was changed. pavucontrol/pasystray packages were installed with package downloads on /tmp; their GUI was not opened.

Separate Magisk audio module starts only the root broker, installs the ordinary signed APK, grants no permissions and starts no helper automatically. User starts it visibly once after each reboot. Active streams hold a partial wake lock; microphone opens only on Linux recording. Targeted Magisk/app socket and FD SELinux rules are packaged. Module and APK installation remain manual.

Outstanding physical validation after installation and Start: actual Android multi-device routing, Bluetooth/wired playback and microphone, Linux-only volume, Android call priority and restoration, unplug/replug while dozing, stereo/SCO switching, and 10-minute clock/queue soak. Public Android routing can briefly reroute hardware-buffered samples before app callbacks; absolute prevention would require audio-policy changes. No physical audio success is claimed by synthetic tests.
