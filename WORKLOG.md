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

## 0.1.1 first-device fixes

2026-10-09: Installed module failed APK setup at boot, then exited before starting the broker. Reproduced the Binder FD problem read-only: pm path redirected to a /dev log returned FAILED_TRANSACTION; the same command through a pipe succeeded. The working HDMI module already avoids sending protected log FDs to Package Manager and starts its broker independently. The audio service now follows that lifecycle, captures package output through pipes, skips an exactly matching installed APK, verifies package presence after installation, and never disables the broker on APK-install failure. No broader SELinux permissions were added.

Linux startup now probes PipeWire/Pulse connections rather than stale socket files, and detaches its supervisor into a separate session. A terminated launcher process group previously killed its daemons. Regression fixtures cover process-group death, stale sockets, repeated start/stop, exact APK streaming, installation failures and matching-APK skip. The installed Linux runtime is 0.1.1; original session defaults/Termux PulseAudio remain intact. The installed Magisk module is still 0.1.0, with its broker started manually for this boot; 0.1.1 ZIP installation remains manual.

Physical permission check: the Android Settings activity was foreground while Linux Audio's foreground service (types 0x82, media plus microphone) ran in the background. Phone bottom microphone captured 333824 float-output frames in 7.241 seconds with actual signal, no client error and verified Android device 23. No PCM was stored: samples were discarded and only numeric summaries retained on /tmp.

JBL Tune 770NC exposed separate Stereo, Headset and Microphone nodes. Its SCO input uses 16000 Hz, 320-frame (20 ms) HAL periods with measured jitter around 5 ms; its AudioRecord client buffer holds 960 frames. The original single-period prefill repeatedly exhausted the FIFO and reopened the recorder. Capture now pre-fills two SCO periods (40 ms), reserves that prefill in addition to the bounded 80 ms delivery-burst budget, and treats a temporary consumer underrun as no data/re-prime of the same recorder. It retains every queued PCM byte; PipeWire may expose a gap while data arrives. Malformed/overflow/lost-route faults still stop visibly. Capture diagnostics aggregate re-primes and snapshot request/availability outside realtime logging.

Latest physical Bluetooth check: 334848 float-output frames over 7.009 seconds, actual signal, no client error, verified Android microphone 48, one retained recorder session and one transient re-prime. After recording stopped, stereo availability returned. While-in-use microphone permission was sufficient for both phone and headset with the correctly started foreground service. Stopping/force-stopping the helper is distinct from dismissing its activity. Calls, screen-off/unplug, long soak and audible stereo-output quality still need broader physical validation.

Synthetic regression includes 16 kHz S16 SCO capture with 20 ms bursts/5 ms jitter and an 80 ms delivery stall: bit-exact transport, source re-prime without recorder reopen, native/Pulse per-app routing and live app moves all passed.

## LXDE desktop integration

2026-10-09: User authorized making the bridge the normal desktop audio server and removing obsolete native Termux PulseAudio state after disabling its startup. The migration now exports audio variables through LXSession's actual `Environment_variable` section as well as shell/Openbox configuration; shell exports precede the interactive early return. Volume bindings are added to the LXDE `lxde-rc.xml` and standalone `rc.xml`, preserving existing bindings. PulseAudio, native PipeWire and ALSA client defaults reach the private server without wrappers, and unrelated graphics/DBus/runtime variables remain unchanged.

LXDE supervises `linux-audio tray`, which starts the private server and updates only audio DBus activation variables before becoming pasystray. The menu entry is named Sound Settings under Preferences and opens pavucontrol. Device selection, microphone selection, Linux volume and per-application routing remain standard PipeWire/PulseAudio operations; no transport, Android, broker or native rendering code changed.

Applied the migration to the real user configuration, with originals under `~/.local/state/linux-audio-routing/backup-20261009-200713-561410`. Unwrapped PulseAudio/native PipeWire/ALSA control probes all reached the bridge; JBL stereo was already the default output and was retained. The old native PulseAudio daemon had no streams or other clients and was stopped gracefully. Its socket disappeared. Removed only the obsolete commented native startup block and the custom anonymous chroot socket line from Termux's profile/default.pa; originals and path mapping are in the same backup's termux-native directory. Packages and unrelated native settings were retained.

Disposable-home tests cover upgrades of old managed blocks, idempotence, backup content, LXSession environment preservation, shell early-return ordering, both Openbox profiles and explicit stale-Termux export removal. Existing supervisor lifecycle regression passes. Actual lxpanel/pasystray and pavucontrol connected to the production audio server and created an embedded tray icon/settings window on isolated Xvfb. The virtual-display GUI test used GTK's Cairo renderer and a separate DBus session; no production graphics setting was changed. Temporary GUI processes were stopped, the audio server stayed running, and logs remain on /tmp. No LXDE desktop was running during the real migration, so the icon starts at the next desktop launch. Audible playback, call/doze/unplug and long-soak validation remain separate pending hardware checks.

## 0.1.4 routing/resource candidate (2026-10-09)

The exact installed LineageOS source audit and device evidence are recorded in `diagnostics/android-routing/README.md`. The second UNPROCESSED recorder fails inside PAL's raw-session admission, even when Android reports a successful route and unsilenced recording. A targeted phone-raw plus headset-communication probe delivered nonzero samples from both routes, without the raw-session failure. Full PCM was not saved.

Implemented in the candidate:

- Android reserves shared capture/output paths until native stream release; conflicting selections publish a distinct `busy` reason without disappearing or redirecting to speakers. Multiple Linux apps still share an endpoint through PipeWire.
- Headset capture uses the communication source. Phone capture retains UNPROCESSED when supported. PCM transport remains unchanged in format/content; any platform processing associated with the communication source belongs to Android, not the bridge.
- Explicit Bluetooth Stereo/Headset selection prevents the other mode reopening automatically. Mode changes retire incompatible streams before acquiring the replacement path. Android still owns call priority.
- Linux reconciliation retires old endpoints before opening replacements, independent of inventory ordering. Busy routes retain their identities and recover after their resource becomes free.
- Earlier uncommitted fixes include separate accepted/played credits, passive Sound Settings meters, relevant-event retries, retained manager wakeups and short sounds, bounded startup/backpressure queues, and full-duplex nonblocking PCM writes.

Native build and host tests run on root@192.168.104.201; isolated graph tests run on the phone against a fixture with no hardware access. The user installed 0.1.4. Live tests exposed cold-open playback overflow and simultaneous capture FIFO overflow; the root phone-plus-headset capture probe alone did not establish that the entire PipeWire path worked. Full installed-helper routing validation remains pending. Do not describe the candidate as a completed live validation or install its Magisk package automatically.

## 0.1.5 coordinated latency candidate (2026-10-09)

Measured cold-route backlog, generic AudioTrack oversizing, and missing downstream
presentation-delay reporting. See the latency diagnostic report and numeric
summary. An event-driven-only A/B worsened speaker latency with the old large
Android buffer, so it was not deployed alone.

Implemented capability-selected graph driving, bounded pending queues, coherent
native presentation frame/time observations, SPA latency publication, per-epoch
reset, and adaptive effective AudioTrack sizing that survives Android rerouting.
The graph watchdog handles missed completion callbacks without pacing normal
playback. Native transport remains C99 and bit-preserving. The new latency
arithmetic and buffer policy have unit coverage; isolated graph validation checks
actual client timing, exact sample patterns and lifecycle/routing behavior.
Manifest generation now refuses stale isolated-test results when binary/script
hashes do not match and no longer inherits old physical-microphone claims.

The root policy probe showed that Android can re-enlarge buffers during routing;
reapplying the effective limit fixes that case. These short probe runs are not
ordinary-helper YouTube or long-soak validation. The APK is versioned separately
as 0.1.5/code 6 for manual installation. No Magisk installation is authorized or
needed for the live candidate update.
