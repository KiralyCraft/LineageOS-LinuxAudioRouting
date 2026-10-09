# Default-device MMAP preference, 0.1.8 candidate

The bridge observes effective PipeWire `default.audio.sink` and
`default.audio.source` metadata. A selected default requests AUTO MMAP; other
shared-PCM endpoints request NEVER MMAP. Opening all secondary endpoints first
therefore cannot consume the helper's MMAP slot. Android applications outside
this helper can still compete for that hardware resource. No system properties,
vendor policy, Bluetooth codec, or microphone preset is changed.

The helper serializes process-local policy selection, AAudio open and restoration.
Both preferred and secondary routes retain LOW_LATENCY when Android exposes the
policy API; otherwise secondary routes use the public PERFORMANCE_NONE fallback.
AUTO may fall back to legacy AAudio. Default selection is not a capability claim.

Default changes drain/retire affected streams before replacements open. Existing
PipeWire nodes and Linux clients survive. This can briefly interrupt audio and
is not advertised as seamless stream migration. WirePlumber may itself move
applications following the default; the handoff fixture pins its two clients so
both endpoints remain active while their preferences exchange.

## Installed-policy evidence

The active configuration is `/vendor/etc/audio/audio_policy_configuration.xml`,
confirmed by `dumpsys media.audio_policy`. Its `mmap_no_irq_out` profile has
`maxOpenCount: 1` and includes Bluetooth A2DP, but excludes SCO headset playback.
`mmap_no_irq_in` is separate and lists built-in and SCO microphones; enumeration
alone does not establish that the requested capture format/preset will obtain
MMAP. Installed 0.1.7 captures used legacy AAudio. The planned 0.1.8 test must
record actual `prefer_mmap` and `mmap` values, plus PCM progress and route identity.

## Automated checks

The native policy test exercises concurrent AUTO/NEVER opens, restoration after
failed opens, rejected policy changes, and the absent-export fallback under
ASan/UBSan. The isolated graph fixture exercises secondary-first opens, active
default handoff, independent input preference, continued PCM, and client survival.
These simulate Android's data endpoint: they do not prove MMAP on the phone.

A short-file graph check intermittently observed 9088 of 9600 nonzero float
samples (one stereo 256-frame block missing), before any default switch. It was
reproduced with the unchanged 0.1.7 bridge as well as the initial 0.1.8 candidate.
Other runs retained all 9600. This pre-existing graph startup/drain issue remains
unresolved; passing reruns must not be represented as a fix. Direct native
transport tests retained the exact 20000-byte pattern and final partial packet.
Evidence is retained under `/tmp/linux-audio-route-debug/` in
`mmap-test-first-failure/` and `baseline017-current-fixture.log`.

## Physical test gate

After manual APK installation and Start: test default stereo headphones plus
bottom phone microphone, then default headset playback plus headset microphone.
Inspect actual native MMAP status independently for both directions, preserve
SBC, and restore stereo plus bottom microphone afterwards. No acoustic latency
or successful installed-0.1.8 result is claimed here.
