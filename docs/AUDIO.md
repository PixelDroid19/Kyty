# Audio runtime

Kyty keeps guest audio generation separate from host delivery. NGS2 decodes
and mixes voice data; AudioOut owns guest-visible ports, timing and per-channel
volume; SDL converts and queues PCM for the selected host device. No PCM cache
is written to disk.

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

The host queue is bounded by time, not by a guest-specific number of grains:

- target queued duration: 60 ms;
- maximum producer wait while saturated: 250 ms;
- queue errors are reported once per port instead of being converted to fake
  success with discarded state.

MAIN and BGM ports own SDL devices. Other guest port types retain timing but do
not invent a host sink.

## Host pause

`F9` pauses/resumes the guest and host audio together. Pause closes the
producer gate before pausing each SDL device. Resume starts devices before
waking producers, preventing a resumed grain from racing a still-paused sink.
Port close clears queued audio and releases both the conversion stream and SDL
device.

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
cmake --build _build_linux --target fc_script -j4
_build_linux/fc_script scripts/run_unit_tests.lua \
  --gtest_filter='EmulatorAudio.*'
```

For runtime evidence, use a Release build, silent HLE function logging and a
clean host audio session. Record device format, channels, sample rate,
peak/RMS level and zero-crossing count. A live stream containing only zeroes
proves device creation, not sound generation.

## Failure diagnosis

- `audio queue failed: Invalid audio device ID` after an earlier fatal error is
  usually shutdown damage; diagnose the first failure.
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
