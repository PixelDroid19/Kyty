# Audio runtime

Kyty keeps guest audio generation separate from the host output device. NGS2
produces interleaved floating-point stereo grains, AudioOut owns guest-visible
ports and timing, and SDL queues the resulting PCM to the selected host
backend. Audio data is streamed in memory; this path does not write a PCM or
shader-style cache to disk.

## Implemented path

- CustomSampler format control `0x40010000` accepts the captured S16 mono or
  stereo, 44.1 kHz source contract.
- CustomSampler data control `0x40010001` validates the captured source pointer,
  byte count, frame count and `bytes = frames * channels * 2` invariant.
- Play and stop commands update per-voice state. Render mixes active voices,
  applies voice gain, resamples linearly to 48 kHz and clips to float stereo.
- MAIN and BGM AudioOut ports use bounded SDL queued audio. Queue depth is
  limited to four guest blocks so host latency cannot grow without bound.
- Linux prefers SDL's PipeWire backend when it is compiled in and initializes
  successfully. An explicit `SDL_AUDIODRIVER` remains authoritative; SDL's
  normal selection is used when PipeWire is unavailable.

Unrecognized CustomSampler module controls remain opaque. They are accepted
only where the module class is known, and no audible behavior is invented for
them.

## Verification

Run focused contracts with:

```bash
ninja -C _build_linux fc_script
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='EmulatorAudio.*'
```

For runtime validation, use Release, silent function logging and a clean host
audio session. Verify both the SDL/PipeWire stream format and non-zero monitor
samples. A stream existing with an all-zero capture proves device creation but
does not prove sound generation.

Record the sample rate, channels, peak/RMS level, zero-crossing count and cache
state in untracked scratch evidence. Do not commit captured PCM, private paths
or workload identifiers.

## Failure diagnosis

- `audio queue failed: Invalid audio device ID` after an earlier fatal error is
  normally shutdown wreckage. Fix the first graphics/HLE failure first.
- A PulseAudio main-loop assertion on Linux can be a host backend failure. The
  capability-based PipeWire preference avoids that backend when available.
- A live 48 kHz stereo stream with silence means the host sink is working;
  inspect NGS2 voice format/data/play controls and the render buffer next.
- Console HLE logging materially changes frame pacing. Do not compare its FPS
  or underruns with a Silent run.

## Known limits

- Only the captured CustomSampler S16 mono/stereo 44.1 kHz source contract is
  decoded. Other codecs and sampler formats remain unsupported.
- AudioOut volume is retained per port, while the verified audible path applies
  NGS2 voice gain. Full per-channel output-volume mixing still needs a captured
  contract and regression test.
- Eight-channel routing and device conversion require host-capability tests
  before they can be considered portable.
- NGS2 rack/system destruction and long-running voice reclamation need explicit
  lifecycle exports before host state can be reclaimed safely.

## Related compatibility boundary

AGC direct-resource index `1` is a Shader Resource Table pointer, not an inline
sampler or storage descriptor. Titles that reach this path require an SRT
walker/materialization contract. Do not reinterpret the pointer as another
descriptor merely to continue execution; that changes shader bindings and can
surface unrelated SPIR-V failures.
