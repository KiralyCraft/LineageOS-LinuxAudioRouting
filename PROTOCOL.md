# Version 1 contract

All connections begin with a length-prefixed JSON `hello` containing `version:1`, `op:"hello"` and `role` (`linux`, `helper` or `data`). The broker is root-owned and validates SO_PEERCRED against Linux UID and `/data/system/packages.list`. Test fixtures require a distinct socket name and explicit `--test isolated`; production authentication is not relaxed.

Control JSON is UTF-8, prefixed by a little-endian uint32 byte count (1..65536). Unknown versions/roles are rejected. The broker's bounded incremental parser prevents a partially written control message from blocking other peers. Request IDs correlate replies; inventory generations reject opens from stale device lists. Helper disconnect invalidates availability and closes sessions.

An open request returns a stream ID, epoch and cryptographically random 128-bit token. Linux and Android attach once each with their authenticated role, side, stream and token. After a JSON `ready`, the broker sends the byte `F` with exactly one SCM_RIGHTS socket descriptor and closes that authentication connection. The client receives the marker/descriptor separately from the JSON body so ancillary data cannot be consumed accidentally. The broker closes each original descriptor after transfer and sends helper `activate` when both ends are handed off. It never receives PCM.

PCM uses this fixed 40-byte little-endian header, followed by unchanged interleaved sample bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | uint32 | Magic `0x50445541` |
| 4 | uint32 | Kind: DATA=1, CLOCK=2, negotiated CREDIT=3 and PRESENTATION=4 |
| 8 | uint64 | Open epoch |
| 16 | uint64 | DATA first sample-frame index; CLOCK playback-head progress; CREDIT frames accepted; PRESENTATION native presentation frame |
| 24 | uint64 | Monotonic nanoseconds |
| 32 | uint32 | DATA frame count; CLOCK/CREDIT/PRESENTATION zero |
| 36 | uint32 | Payload bytes, at most 65536 |

DATA indexes start at zero and must be contiguous. Payload length must match frame count × channels × sample width. Epoch mismatch, unexpected packet kind, invalid lengths or incomplete frames terminate the stream. float32 bits are not interpreted by the transport. Signed16 is likewise copied unchanged.

Playback writes include up to 10 ms. A new Linux client requests `write_credit:true` in `open`; a supporting helper acknowledges that feature in its reply. For this negotiated mode, the helper publishes CREDIT after each successful blocking AudioTrack write, including partial writes; Linux permits up to 20 ms not yet accepted by Android. CLOCK remains an independent playback-head count; it is not an acoustic presentation timestamp. Acceptance never satisfies playback drain. This avoids deadlocking a HAL whose first playback period exceeds the credit window. Legacy peers retain CLOCK-based credit and never receive unsolicited CREDIT messages. The helper excludes route-probe frames from the Linux playback timeline and keeps the verified track running without pause/flush resets. It sends a short final packet rather than requiring an exact 10 ms tail. Capture is read as raw PCM and sent in roughly 10 ms reads, with a 20 ms Linux prefill (40 ms for SCO input). The capture FIFO reserves its prefill in addition to 80 ms of bounded delivery-burst capacity. A temporary consumer underrun retains queued bytes and re-primes the same recorder, reporting an empty PipeWire chunk until data is available. There is no sample dropping or bridge-generated silence to catch up. PipeWire can adapt its mixer rate to the Android clock outside this protocol.

Control close/call/unplug retires an epoch. The helper closes its direct endpoint; Linux sees EOF. The broker intentionally retains no extra duplicate data descriptor in steady state: abrupt helper process death naturally closes its endpoints, while cooperative control close relies on the authenticated helper's teardown. Late route/close notifications must match a still-live stream and epoch. New device availability permits a new epoch; old data is never replayed.

Kernel stream sockets supply reliable ordered bytes. Bounded FIFOs and fault reporting supply explicit continuity checks; this is a realtime transport and cannot promise playback continuity across hardware loss, process death or a queue overrun. Those conditions are visible failures, not successful lossy transport.

Endpoints advertising `clock_driver:true` use the Android route-ready event and write credits to drive graph demand. Capture arrival drives recording demand. Graph production waits for the verified route, so cold-open duration does not accumulate as a playback backlog. The pending and transport FIFOs are each bounded to 80 ms; normal output demand stops at 20 ms not yet accepted by Android. Older helpers retain the compatibility path with its 1500 ms cold-route FIFO. That compatibility backlog can persist as latency after startup; it is not a low-latency guarantee. Newly arriving audio always stays behind older queued bytes. Normal idle also drains short sounds queued while Android is still opening another endpoint. An overrun is an explicit failure; no PCM is discarded to accelerate playback. Failed endpoints remain latched until a relevant activation, device recovery, or priority-resume event instead of retrying on unrelated inventory updates.

The Linux playback worker services inbound metadata while a PCM write is incomplete. Pending header/payload bytes remain owned by that worker until fully sent; only then does its sent-frame counter advance. Read and write socket readiness share one poll loop, preventing a full metadata direction from blocking the peer needed to accept PCM. The one-second no-write-progress timeout is a fault bound, not an audio pacing timer.

### Audio-path reservations (helper 0.1.4)

Inventory entries may include `resource` (shared Android path class), `selection_group` (alternative ports such as phone microphones), `capture_source` (Android source preset), and `busy` (human-readable contention/profile reason, empty when permitted). `available` still means device/permission availability. A busy node is retained, not treated as an unplug or silently rerouted. Older consumers can ignore this metadata; the helper independently enforces reservations on open.

The tested device allows one raw capture session. Linux applications using the same microphone share one PipeWire source and one Android recorder. Separate raw microphone selections compete for that session; headset communication capture has a separate reservation. Current conservative playback reservations separate buffered Bluetooth stereo from the shared low-latency output path. These are the helper's supported paths, not a claim that every device has independent outputs or that all future HALs have identical limits.

A reservation is released only after native stream retirement, not when cancellation is requested. The control executor waits for bounded retirement before admitting a conflicting replacement; a retirement timeout fails the request and retains ownership until actual release. Explicit Bluetooth `stereo`/`headset` selection blocks the other profile until another explicit selection. It is distinct from transient automatic headset use.

## Negotiated presentation clock and latency (0.1.5)

Linux requests `presentation_clock:true` in `open`; the helper acknowledges it.
Only negotiated playback streams receive PRESENTATION packets. Their zero-payload
frame/time pair comes from `AudioTrack.getTimestamp()`, with route-probe frames
subtracted from the frame position. The timestamp is retained exactly: it can
describe a recently presented frame or one committed for future presentation.
A missing timestamp is not fabricated from playback-head position.

Linux validates the epoch and frame frontier, ignores regressing or implausibly
future metadata, and uses a coherent snapshot. The estimated delay at the graph
boundary is `(produced_frame - presentation_frame) / rate + timestamp - now`.
Arithmetic is bounded and stale observations are rejected. This becomes the
sink's SPA latency parameter, which propagates to native PipeWire and PulseAudio
stream timing queries. `linux.audio.latency.valid` indicates whether the current
observation is usable. An unavailable observation retains the last estimate
marked invalid; retiring the epoch clears it. PulseAudio sink-list latency fields
in this PipeWire implementation are hardcoded zero; use client stream timing or
SPA parameters to verify propagation.

PRESENTATION packets never grant write credit or advance the existing CLOCK-based
drain condition. The graph watchdog is armed only for an outstanding processing
iteration and disarmed on completion; it recovers a missing `trigger_done`, as
required by PipeWire. It does not provide normal audio pacing. Set
`LINUX_AUDIO_CLOCK_DRIVER=0` to opt out, or `=1` for a controlled forced test;
without an override the helper's endpoint capability selects the path.

The helper retains allocation capacity for recovery, but limits the effective
AudioTrack buffer. Initial write-ahead is two transport packets or two advertised
output bursts, whichever is larger, clamped by Android. Route-time enlargement
is detected and the effective limit reapplied. New underruns grow that limit,
bounded by allocated capacity. Route readiness includes consumed-frame progress
and a stable silence-only warmup; no Linux PCM is consumed during this warmup.
Further adjustments change capacity, never sample content. The five-second
route-preparation deadline is a failure bound, not a playback delay.

## Shared PCM and native AAudio (0.1.6)

Endpoint capability, open request and open reply must all agree on
`shared_pcm:true`. Missing capability or `LINUX_AUDIO_SHARED_PCM=0` retains the
existing packet protocol. The authenticated broker is unchanged. After direct
socket handoff Linux sends three `F`/SCM_RIGHTS messages in order: sealed-size
memfd, producer eventfd, consumer eventfd. Android validates descriptor type,
size/seals, version, epoch, format and capacity before starting AAudio. The socket
then carries no PCM; shutdown/HUP cancels the epoch.

The ABI is little-endian AArch64 with naturally aligned fixed-width fields,
version 1, magic `0x53445541`, and data at offset 4096. `include/shared.h` is the
canonical layout (120-byte header). Monotonic reader/writer counters count bytes,
not slots or frames; ring offsets use modulo capacity. One producer owns writer
and one consumer owns reader. Sample publication and retirement use release
stores and acquire loads. Capacity comes from the validated sealed allocation,
not mutable per-operation metadata. A counter distance greater than capacity or
non-frame-aligned positions fail the stream.

Playback: Linux writes; Android consumes via AAudio. Capture: Android writes via
AAudio; Linux consumes. Eventfds notify publication/retirement and are hints:
consumers always recheck counters. PCM never travels through the broker or Java.
A seqlock-protected metadata record carries played frames and clock observations.
Presentation reporting is separately negotiated; it is disabled for native
Bluetooth until calibrated. Writer/reader equality alone does not complete
playback drain: the native played frontier must also reach the written frontier.

Normal capacity remains 80 ms for output, and 80 ms plus capture prefill for
input; capacity is a bound, not a target backlog. Native I/O handles partial
transfers and ring wrap without sample modification. Capture overrun is an
explicit discontinuity failure; samples are not silently discarded to catch up.
Stopped/error fields invalidate the epoch. Stop signals cancellation, shuts down
the lifetime socket and wakes waiters; mappings/native streams are freed only
after their worker has returned. Pausing a PipeWire capture quiesces its producer
immediately; existing barriers protect final retirement.
