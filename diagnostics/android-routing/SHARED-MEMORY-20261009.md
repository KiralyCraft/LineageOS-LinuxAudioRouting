# Routing and shared-memory investigation, 2026-10-09

## Recovered routing and remaining defects

Observed Quake and Firefox both requesting Bluetooth stereo playback and the
headset microphone concurrently. Quake uses native PipeWire through SDL and
opens an active microphone stream at the menu. Moving both recording streams
to the phone restored stereo; the user confirmed Quake audio and phone capture.
A separate capture client received 334336 frames in seven seconds, with nonzero
samples. No microphone PCM was saved. At the user's request the final defaults
are the phone bottom microphone and stereo headphones, with explicit stereo
profile selection preventing an incidental headset capture from taking it over.

Logs also show capture FIFO overflow on stream pause and route changes. This
remains a real lifecycle issue, not proof that every microphone is broken.
The current serialized manager can be busy opening/draining one endpoint while
another endpoint has stopped consuming. Increasing buffers is not a sufficient
fix; consumer inactivity, recorder cancellation and buffer retirement need an
explicit contract. Profile selection also needs standard Linux card/profile
integration; separate sink/source defaults do not express Bluetooth exclusivity.

## Delay is not principally a socket-throughput problem

PCM stereo at 48 kHz float32 is 384000 bytes/s. Socket copies and wakeups should
be removed where practical, but this does not imply they explain hundreds of
milliseconds of latency. Quake's saved SDL configuration has s_mixPreStep=0.05
and s_mixahead=0.2. These control mix-ahead and must not be counted as an exact
fixed 250 ms addition: the engine limits/adapts the mix horizon.

The phone currently negotiated AAC, 44.1 kHz stereo, with A2DP offload enabled.
Bluetooth reports remote delay 2500 units of 0.1 ms (250 ms). That is reported
metadata, not an independent acoustic measurement. It is not the user's KDE
SBC-XQ configuration. Codec and platform buffering require a controlled A/B.

A bounded root silence probe reused PlaybackBuffer while requesting different
Android AudioTrack performance classes. Requested route 38 was verified:

| Request | First median | Repeat median |
| --- | ---: | ---: |
| Power-saving | 477 ms | 368 ms |
| Normal | 378 ms | not repeated |
| Low-latency | 263 ms | 263 ms |

These are written-frame versus native presentation timestamp estimates. Runs
were short and not acoustic/game latency tests. Variability in the buffered
path means the first power-saving value must not be treated as a constant.
Changing output classes also invalidated the live helper track during probes;
therefore selecting LOW_LATENCY everywhere is not a validated multi-device fix.
The existing helper uses distinct classes because preferred devices can conflict
on a shared Android output. All probes released their streams; production
remains on the existing 0.1.5 backend.

## MMAP feasibility established, scope limited

The native C AAudio probe was compiled using NDK 29 on the build server and
executed twice as root. It requested SHARED + LOW_LATENCY and device 38.
AAudioStream_isMMapUsed reported true. The running service showed a shared
endpoint backed by MMAP, and AudioFlinger showed MMAP_PLAYBACK explicitly routed
to AUDIO_DEVICE_OUT_BLUETOOTH_A2DP. Both probes reported zero xruns. Client burst
was 96 frames, selected buffer 192 frames, and hardware was S16/48 kHz/stereo.

The short MMAP timestamp estimate must NOT be described as Bluetooth acoustic
latency: whether that timestamp covers downstream codec/radio/headset delay is
unverified. The ordinary helper UID, audible waveform correctness, independent
speaker/stereo routing, capture, calls and disconnects still need validation.

## Proposed next backend

Keep Android AudioManager/call/device control and Linux PipeWire mixing/per-app
routing. Add a native C AAudio backend and a versioned shared-memory transport:
per-stream bounded PCM storage, generation IDs, explicit producer/consumer
ownership and acquire/release counters, plus eventfd notifications. Send FDs
through the authenticated existing broker; retain sockets for lifecycle/control.
No sample dropping, bridge resampling or bridge gain is introduced.

Shared memory eliminates PCM socket transfers and Java heap/direct-buffer
staging. It is not automatically end-to-end zero-copy: AAudio shared mixing,
format conversion and filling callback buffers may still copy. First establish
a safe shared ring with a bounded final copy; then assess whether PipeWire can
produce directly into the shared slots without premature buffer reuse. Do not
expose proprietary HAL buffer FDs or bypass Android call ownership merely to
claim zero-copy.

Sources: [AAudio/MMAP architecture](https://source.android.com/docs/core/audio/aaudio),
[AAudio API](https://developer.android.com/ndk/guides/audio/aaudio/aaudio),
[ioQuake3 mix-ahead](https://github.com/ioquake/ioq3/blob/main/code/client/snd_dma.c),
[SDL capture initialization](https://github.com/ioquake/ioq3/blob/main/code/sdl/sdl_snd.c).
Numeric probe results are in `shared-memory-probe-20261009.json`. Diagnostic
sources are `AudioProfileProbe.java` (compile with PlaybackBuffer.java) and
`aaudio_probe.c` (link libaaudio and libdl). Raw device dumps remain in tmpfs.
