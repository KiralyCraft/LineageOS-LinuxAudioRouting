# Version 1 contract

All connections begin with a length-prefixed JSON `hello` containing `version:1`, `op:"hello"` and `role` (`linux`, `helper` or `data`). The broker is root-owned and validates SO_PEERCRED against Linux UID and `/data/system/packages.list`. Test fixtures require a distinct socket name and explicit `--test isolated`; production authentication is not relaxed.

Control JSON is UTF-8, prefixed by a little-endian uint32 byte count (1..65536). Unknown versions/roles are rejected. The broker's bounded incremental parser prevents a partially written control message from blocking other peers. Request IDs correlate replies; inventory generations reject opens from stale device lists. Helper disconnect invalidates availability and closes sessions.

An open request returns a stream ID, epoch and cryptographically random 128-bit token. Linux and Android attach once each with their authenticated role, side, stream and token. After a JSON `ready`, the broker sends the byte `F` with exactly one SCM_RIGHTS socket descriptor and closes that authentication connection. The client receives the marker/descriptor separately from the JSON body so ancillary data cannot be consumed accidentally. The broker closes each original descriptor after transfer and sends helper `activate` when both ends are handed off. It never receives PCM.

PCM uses this fixed 40-byte little-endian header, followed by unchanged interleaved sample bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | uint32 | Magic `0x50445541` |
| 4 | uint32 | Kind: DATA=1, CLOCK=2 |
| 8 | uint64 | Open epoch |
| 16 | uint64 | DATA first sample-frame index; CLOCK frames consumed |
| 24 | uint64 | Monotonic nanoseconds |
| 32 | uint32 | DATA frame count; CLOCK zero |
| 36 | uint32 | Payload bytes, at most 65536 |

DATA indexes start at zero and must be contiguous. Payload length must match frame count × channels × sample width. Epoch mismatch, unexpected packet kind, invalid lengths or incomplete frames terminate the stream. float32 bits are not interpreted by the transport. Signed16 is likewise copied unchanged.

Playback writes include up to 10 ms; the Android worker writes every byte, including partial AudioTrack writes, then publishes CLOCK from actual playback head/timestamp. Linux allows up to 20 ms outstanding. It sends a short final packet rather than requiring an exact 10 ms tail. Capture is read as raw PCM and sent in roughly 10 ms reads, with a 20 ms Linux prefill (40 ms for SCO input). The capture FIFO reserves its prefill in addition to 80 ms of bounded delivery-burst capacity. A temporary consumer underrun retains queued bytes and re-primes the same recorder, reporting an empty PipeWire chunk until data is available. There is no sample dropping or bridge-generated silence to catch up. PipeWire can adapt its mixer rate to the Android clock outside this protocol.

Control close/call/unplug retires an epoch. The helper closes its direct endpoint; Linux sees EOF. The broker intentionally retains no extra duplicate data descriptor in steady state: abrupt helper process death naturally closes its endpoints, while cooperative control close relies on the authenticated helper's teardown. Late route/close notifications must match a still-live stream and epoch. New device availability permits a new epoch; old data is never replayed.

Kernel stream sockets supply reliable ordered bytes. Bounded FIFOs and fault reporting supply explicit continuity checks; this is a realtime transport and cannot promise playback continuity across hardware loss, process death or a queue overrun. Those conditions are visible failures, not successful lossy transport.
