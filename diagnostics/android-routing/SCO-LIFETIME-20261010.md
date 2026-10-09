# SCO lifetime fix and Quake timing

## Why returning to stereo failed

0.1.8 treated an explicit Headset profile as an active communication user, even
with no remaining headset streams. It also acquired communication routing on
profile selection. In the failed test, the final headset stream stopped around
00:07:05; Android processed inactivity around 00:07:11. By the later stereo
selection, `dumpsys audio` showed MODE_NORMAL and no communication routing client,
but SCO_STATE_ACTIVE_INTERNAL remained. PAL rejected A2DP in suspended state;
AAudio's legacy fallback was rerouted from device 38 to 3 and disconnected.

In the installed [AudioDeviceBroker source](https://raw.githubusercontent.com/LineageOS/android_frameworks_base/3b9065901b817e95c95152082c746cbee46d2290/services/core/java/com/android/server/audio/AudioDeviceBroker.java),
`topCommunicationRouteClient()` depends on mode ownership or active playback /
recording. The framework checks inactive clients after six seconds. Activity and
mode-owner update paths can compute the previous SCO requester after replacing
that state. Later removal can consequently see no old SCO requester and omit
`stopBluetoothSco()`. This source path explains the recorded orphaned voice link;
it is not a claim that all Android versions have this behavior.

## 0.1.9 change

A profile is now only a device-admission preference. Stream preparation acquires
the communication route. Route reservations record which streams require that
route and retain ownership until actual resource retirement. The final headset
stream retirement releases communication even if Headset remains selected or
other non-headset streams are active. A playback stream keeps routing alive when
the microphone stops, and vice versa. Failed setup and duplicate late retirement
use the same reservation lifetime. No fixed delay, polling recovery, repeated
profile toggle, or system audio-policy modification is introduced.

The Java regression test covers independent resources, both duplex owners,
last-owner retirement, stale completion, and cancellation. Installed validation
must keep Headset selected after stream closure, wait beyond Android's inactivity
check, and then return to stereo. Test an idle profile selection separately: it
must not start SCO. Keep the running Quake process alive throughout.

## Quake measurement before updating

Quake's stereo stream uses MMAP; its phone microphone uses legacy AAudio. Thirty
seconds of passive shared-header observations showed 22.04 ms median / 24.85 ms
p95 ring occupancy, and 4 ms median AAudio accepted-minus-read occupancy. The xrun
counter remained at 2 during that interval. These counters omit other upstream
queues and uncalibrated Bluetooth/headset buffering; Pulse's displayed zero
latency is not an end-to-end measurement.

Read-only access to named symbols in the running Quake executable, without
ptrace attachment or thread suspension, found live `s_mixahead=0.2` and
`s_mixPreStep=0.05`. Two hundred counter samples over five seconds showed about
170 ms median / 200 ms p95 between `s_paintedtime` and `s_soundtime`, using the
engine's actual `dma.speed=22050`. This is substantial game-side mixed-ahead
buffering. SDL's output stream is 44.1 kHz; the bridge is 48 kHz. The game settings
were not changed. The two measurement windows must not be added as if they were
one correlated acoustic measurement. See `quake-018-latency-20261010.json`.
