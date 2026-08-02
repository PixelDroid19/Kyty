# Audio runtime

Kyty keeps guest audio generation separate from host delivery. `Audio.cpp`
owns the guest-visible ABI and services. `AudioHost.cpp` owns SDL devices,
PCM conversion, port synchronization and real-time pacing. NGS2 remains
responsible for voice decoding and mixing. No PCM cache is written to disk.

## AudioOut host path

AudioOut accepts the guest PCM contracts already exposed by its ABI:

- signed 16-bit or 32-bit float samples;
- mono, stereo or eight interleaved channels;
- native and `8ChStd` channel order;
- the frequency and grain size supplied when the port is opened.

`AudioPcmApplyChannelVolumes` is the only volume implementation. It applies
the exact selected channel gains for S16 and F32, saturates S16 output and
preserves the interleaved layout. `8ChStd` remapping is performed once when the
guest volume array is consumed.

SDL may open a device with a different frequency, channel count or sample
buffer size. One `SDL_AudioStream` per port converts the guest stream to that
actual device contract. A different sample format is rejected at open time;
it is not reinterpreted.

Each successful output advances one grain on a monotonic per-port clock. PCM
is converted and copied into SDL while the port lifecycle lock is held, then
the producer sleeps outside that lock until the grain deadline. If a producer
falls more than four grains behind, the deadline is resynchronized instead of
trying to replay an obsolete backlog. This prevents both multi-grain bursts
and long queue-capacity stalls.

Queue or conversion failures are returned to the guest-facing service and
reported once per port. They do not silently destroy the SDL device or switch
the port to synthetic silence.

MAIN and BGM ports own SDL devices. Other guest port types retain timing but do
not invent a host sink.

## Host pause

`F9` pauses/resumes the guest and host audio together. Pause closes the
producer gate before pausing each SDL device. Resume starts devices before
waking producers, preventing a resumed grain from racing a still-paused sink.
Port close clears queued audio and releases both the conversion stream and SDL
device. Shutdown first removes the shared host backend from the guest call
path, then waits for any in-flight SDL copy before closing devices. Calls that
already acquired the backend keep its lifetime valid until they return.

## NGS2 CustomSampler

The currently verified CustomSampler source contract is signed 16-bit mono or
stereo at 44.1 kHz:

- format control `0x40010000` selects the captured source shape;
- data control `0x40010001` validates pointer, bytes, frames and
  `bytes = frames * channels * 2`;
- play/stop update per-voice state;
- render applies voice gain, linearly resamples to 48 kHz and clips to float
  stereo.

Unrecognized module controls remain opaque. Kyty does not synthesize audible
behavior for them.

## Verification

```bash
cmake -S source -B _build_linux -DBUILD_TESTING=ON
cmake --build _build_linux --target kyty_audio_host_integration -j4
SDL_AUDIODRIVER=disk \
SDL_DISKAUDIOFILE=/tmp/kyty-audio-host-integration.raw \
  _build_linux/integration_test/kyty_audio_host_integration
```

The integration opens production SDL output ports, submits twelve 10 ms PCM
grains, checks elapsed monotonic time, and closes a real device concurrently
with an in-flight producer. It then verifies that the closed port rejects the
next grain. The disk driver makes this contract deterministic without needing
speakers; the generated file is disposable.

For runtime evidence, use a Release build, silent HLE function logging and a
clean host audio session. Record device format, channels, sample rate,
peak/RMS level and zero-crossing count. A live stream containing only zeroes
proves device creation, not sound generation.

## Failure diagnosis

- `Invalid audio device ID` at shutdown means an output thread reached SDL
  after its port was closed. The shared backend handoff and lifecycle lock are
  the regression boundary; do not suppress the message or keep using the dead
  numeric ID.
- Audio that repeats a short block and then pauses indicates burst delivery.
  Measure elapsed time across consecutive AudioOut grains: twelve 480-frame
  grains at 48 kHz must take approximately 120 ms, not complete immediately
  and not wait on a 60 ms queue threshold.
- A host backend assertion can originate below Kyty. On Linux, PipeWire is
  preferred when SDL compiled it and initialization succeeds; an explicit
  `SDL_AUDIODRIVER` remains authoritative.
- A valid device stream with silence moves the frontier to NGS2 voice
  format/data/play state.
- Verbose HLE logging changes pacing and must not be used for underrun or FPS
  comparisons.

## Known limits

- ATRAC9/AT9 and unverified CustomSampler formats have no decoder. They fail at
  their real frontier instead of returning decoded silence.
- SDL can convert the supported PCM channel counts, but semantic surround
  speaker placement beyond the guest `8ChStd` ordering still needs host
  capability evidence.
- NGS2 rack/system destruction and long-running voice reclamation require
  explicit lifecycle exports before host state can be reclaimed safely.

## Related graphics boundary

AGC direct-resource index `1` is a Shader Resource Table pointer, not an inline
sampler or storage descriptor. Do not reinterpret it to continue execution;
that changes shader bindings and can surface unrelated SPIR-V failures.
