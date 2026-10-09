# Android routing investigation, 2026-10-09

These are measurements on the Xperia API 35 installation, not claims about every Android HAL. Capture probes discard PCM; only counts and route IDs are retained. Diagnostic builds and traces belong in tmpfs. All compilation ran on the authorized build server.

## Installed LineageOS source audit

The installed release is `22.2-20250608-NIGHTLY-pdx234`, Android 15/API 35. Revisions below come from the phone's `/product/etc/build-manifest.xml`, rather than the current branch heads:

| Component | Revision |
| --- | --- |
| frameworks/av | `4d360975fc42849829b41df550aaa38a01fe4adc` |
| frameworks/base | `3b9065901b817e95c95152082c746cbee46d2290` |
| Sony sm8550 common device | `01930342abe91fa4f1884cf8d04fedfc90130d0e` |
| Qualcomm audio-ar reference | `21a6fd812cc78829ddf68c34d38eb47aab19cfdb` |
| Qualcomm PAL reference | `a79438473f9cd87ec7fc4f83a4a6f63c3c4c52bb` |
| Qualcomm AGM reference | `a7744aaa5082ac1608cf272a014c905877f5a727` |

AudioPolicyManager reports `/vendor/etc/audio/audio_policy_configuration.xml` as its configuration. That file matches the pinned Sony source byte-for-byte, as do the installed QRD mixer/resource-manager files, `usecaseKvManager.xml`, and `microphone_characteristics.xml`:

| Source file under `audio/` | SHA-256 of source and installed copy |
| --- | --- |
| audio_policy_configuration.xml | `785dd9a919a715d7b7cd1b33eda62f8a3b8a23f2a0d08e45d637084916db8f96` |
| resourcemanager.xml | `f8401cf3a971eec3b23e97a076820276bc22c8bc74cf9e9863e798a3d69564f2` |
| mixer_paths.xml | `48122ec3d2d5c0ebe7ca4b6961d6778203155fc39bdf4257853f636cefe6e885` |
| usecaseKvManager.xml | `0bd7ef509dce1a3f975e80ce9ea4c4aa3535c162525d8a4813f2963ea1f3eeb3` |
| microphone_characteristics.xml | `55bbcdebb61d1d80b109831c32400ff67fa8b227f09fec03b15b4a9150c25375` |

Important provenance boundary: Sony's [proprietary-files list](https://github.com/LineageOS/android_device_sony_sm8550-common/blob/01930342abe91fa4f1884cf8d04fedfc90130d0e/proprietary-files.txt) supplies `audio.primary.kalama.so`, `libar-pal.so`, and `libagm.so` as vendor binaries. Those are also loaded by the running HAL. The public Qualcomm revisions are useful reference implementations, not proof that these binaries were compiled from exactly that code. Conclusions about the installed vendor implementation require matching runtime evidence.

### Confirmed raw-capture session exhaustion

Our helper requests `UNPROCESSED` for every microphone when the Android property says it is supported. The [HAL reference's `GetPalStreamType`](https://github.com/LineageOS/android_hardware_qcom_audio-ar/blob/21a6fd812cc78829ddf68c34d38eb47aab19cfdb/hal/AudioStream.cpp) maps this to `PAL_STREAM_RAW`. The [PAL reference](https://github.com/LineageOS/android_vendor_qcom_opensource_arpal-lx/blob/a79438473f9cd87ec7fc4f83a4a6f63c3c4c52bb/resource_manager/src/ResourceManager.cpp) limits that class to one active session (`MAX_SESSIONS_RAW = 1`). Its `isStreamSupported()` rejects another stream of that class. This class limit is distinct from the XML's generic `max_sessions` value or AudioPolicyManager's input-profile open count.

A targeted repeat with two root-owned UNPROCESSED recorders reproduced the vendor path directly:

```text
ResourceManager: isStreamSupported: no new session allowed for stream 9
API: pal_stream_open: stream creation failed status -22
AudioStream: Open: Pal Stream Open Error (ffffffea)
AudioStream: onReadError: read failed -22 usecase(21: audio-record)
```

There were 84 repeated session-open/read failures during this approximately two-second probe. Both framework recording configurations still reported their selected microphone and `isClientSilenced=false`; AudioFlinger had two separate active input threads. The first delivered signal, the second only zero-valued samples. The reference `onReadError()` puts the stream into standby, sleeps for the requested buffer duration and returns the requested byte count, explaining how a vendor failure can appear as a successful read at the API boundary. This does not mean that the bridge generated silence or that Android's permission arbiter silenced this test.

This establishes the immediate failure mechanism for two raw recorders. It does not establish that all microphone combinations are physically impossible. Built-in mic paths additionally share `CODEC_DMA-LPAIF_RXTX-TX-3`; Sony's mono selections program the same `TX DMIC MUX2` with different physical microphones. Separate app-visible ports therefore cannot be assumed to mean independent mono hardware paths. A supported multichannel capture may be a better representation, but its channel mapping and actual samples still need validation.

### Playback selection is per output, not independent per track

The pinned [AudioPolicyManager](https://github.com/LineageOS/android_frameworks_av/blob/4d360975fc42849829b41df550aaa38a01fe4adc/services/audiopolicy/managerdefault/AudioPolicyManager.cpp) chooses devices for an output descriptor shared by its clients. `getNewOutputDevices()` resolves preferred-device requests for that output, and `registerPolicyMixes()` first attaches a render mix to an already-open output supporting its device. Registering separate privileged mixes therefore does not itself allocate separate hardware outputs. This matches the earlier root probe, where both mixes registered successfully but playback converged onto one device.

The installed Sony policy provides distinct FAST/PRIMARY and DEEP_BUFFER outputs. The helper's local-low-latency/A2DP-buffered split has a source-level basis and passed the tested speaker-plus-A2DP combination. It is not an independent routing guarantee for every local device pair, and Bluetooth headset/stereo transitions still need lifecycle handling.

### Implementation consequences

- Keep one Android recorder per active capture route and share it among Linux applications through PipeWire. Enforce the device's raw-session capacity before attempting a second raw open; do not accept an apparently active silent stream as success.
- Represent mutually exclusive mic selections as ports/profiles, with explicit resource-conflict status. Do not infer failure solely from zero samples, which can also represent legitimate silence.
- Investigate the headset's communication capture class separately from a phone's raw class. It has a distinct source/flag mapping in the framework/HAL. A follow-up root probe selected phone raw capture on port 24 and headset VOICE_COMMUNICATION capture on port 48. Both delivered nonzero samples with `isClientSilenced=false`, and the matching HAL log contained no raw-session admission/read errors. Headset startup initially produced zeros during Bluetooth connection establishment; at the last reported interval the phone had 71,049 nonzero samples and the headset 10,401. The helper APK still requires its own live test under its ordinary app UID. Do not silently trade unprocessed capture for a processed source to make the error disappear.
- Serialize route retirement and acquisition around shared Android resources. Keep supported independent playback outputs separate, and distinguish a resource conflict from transport failure or device removal.
- Retain the independent duplex transport fix. It passed the isolated graph suite, including the reverse-direction saturation regression that failed on the old worker. Its replacement binary has not yet been deployed for live routing tests.

The source audit and targeted probes changed no installed helper, vendor configuration, or bridge binary. The headset probe temporarily acquired and then released Android communication mode. Probe resources were released. Full dumps remain in tmpfs; only these minimal diagnostic messages are retained here.

## Playback policy

Two silent 48 kHz float stereo AudioTracks requested speaker (port 3) and JBL A2DP (port 38). Both preferred-device calls returned true, but both tracks reported port 3. A root app_process probe confirmed MODIFY_AUDIO_ROUTING permission and the session-ID policy APIs. Registering two session-specific RENDER mixes returned success, but both tracks then reported port 38. The temporary tracks and policies were unregistered; the post-test policy mix list was empty.

A control using low-latency local playback and normal buffered Bluetooth playback retained routes 3 and 38 concurrently with advancing frame counters. This establishes that the tested pair can work through separate existing outputs. Permission alone does not allocate independent outputs. Android 15 AudioPolicyManager::registerPolicyMixes first binds a render mix to an existing supporting output; separate mixes can therefore share the same output.

Helper 0.1.3 requests LOW_LATENCY for local output and POWER_SAVING for A2DP. Real `aplay` tests subsequently verified speaker and stereo-headphone routes concurrently. AudioFlinger reported a local FAST track and a distinct Bluetooth deep-buffer output. This remains route-verified behavior on this configuration, not a universal guarantee for arbitrary device pairs. Shared local output profiles and Bluetooth stereo/headset mode conflicts still require explicit handling.

## Capture

Each built-in microphone worked separately through the Linux graph, with no client error and verified Android device IDs:

| Microphone | Android port | Frames in about 7 seconds | Nonzero samples |
| --- | ---: | ---: | ---: |
| Bottom | 23 | 333312 | 323095 |
| Top | 24 | 332800 | 322592 |
| Back | 26 | 333824 | 324500 |

Concurrent top/bottom recording produced signal on top and all-zero samples on bottom. A root capture probe with CAPTURE_AUDIO_OUTPUT granted reproduced the limitation: both reported their requested route, and both reported `isClientSilenced=false`, but only top carried a signal. Do not advertise the individual built-in ports as proven independent concurrent microphones. This result does not show that ordinary permission elevation fixes the issue. Multiple Linux applications can still share one PipeWire source/Android recorder.

## Distinct transport failures

- The old played-frame credit gate could deadlock before a 40 ms HAL block completed. Negotiated write acceptance credits now control capacity independently of played-frame drain completion.
- Sound Settings peak meters activated every microphone. Passive client properties prevent meter-only links from opening recorders or switching Bluetooth into headset mode.
- Any inventory generation change retried a failed endpoint, including changes caused by that same endpoint's failure. Failure recovery is now tied to relevant activation/device/priority events.
- Unconditional manager sleep lost notifications during reconciliation. A pending-work predicate retains notifications.
- The active output bypassed its backlog and failed as soon as the 80 ms transport FIFO filled. FIFO-ordered backlog now absorbs bounded stalls. Measured Android open times included 582, 960 and 1154 ms, exceeding the former 500 ms startup capacity. The backlog capacity is 1500 ms, consumed immediately when writes progress; it is not an imposed delay.
- Short sounds can finish while another endpoint opens. Pending PCM still requires opening/draining its destination even if the graph has already paused.
- Device movement exposed a duplex socket stall: Linux blocked sending PCM while Android blocked sending clock/credit metadata. Android logged clock-writer monitor contention, and Linux reported send failure. The Linux worker now retains partial writes and polls readability and writability together. The fixture fills the reverse socket while PCM is in flight; the old worker fails this regression. Validation of the replacement is recorded separately.

PCM bytes remain uncompressed and unchanged across the bridge. Credit metadata does not signal playback completion. FIFO overflow and disconnection remain explicit failures.

## Probe source and limits

`AudioPolicyProbe.java` inspects permissions and framework methods without registering routes. `AudioRouteProbe.java` compares preferred routing, privileged per-session mixes, and separate performance profiles using zero-filled samples. `AudioCaptureProbe.java` reports route, silencing status and numeric sample counts without saving samples. Compile on the build server with the Android SDK and d8; execute the resulting dex with app_process in Android's runtime environment. A chroot shell lacks the ART boot class path; obtain the Android runtime variables from the running helper rather than inventing an incomplete environment. Probes must not run during calls and must release their owned tracks/recorders/policies.

Primary implementation references:

- [Android 15 AudioPolicyManager](https://android.googlesource.com/platform/frameworks/av/+/refs/heads/android15-release/services/audiopolicy/managerdefault/AudioPolicyManager.cpp)
- [Android 15 AudioMixingRule](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/media/java/android/media/audiopolicy/AudioMixingRule.java)
- [Android AudioRouting contract](https://developer.android.com/reference/android/media/AudioRouting)

Full runtime dumps contain device/app metadata and remain outside the repository under `/tmp/linux-audio-route-debug`. No microphone recording is included.
