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

## NGS2 standard sampler

Rack 0x1000 renders through the same `Ngs2SystemRender` mix as the custom path.
The control layouts were read from guest param builders and their call sites in a
captured streaming title; sizes are the values the guest writes:

| Control | Size | Contract |
| --- | --- | --- |
| Setup `0x10000000` | 40 | format (24 bytes) at +8; word at +0x20 (the one call site passes 0) and word at +0x24 must be 0 |
| Blocks `0x10000001` | 32 | data at +8, flags at +0x10, count at +0x14, blocks at +0x18 |
| Pitch `0x10000005` | 16 | float ratio at +8; zero word at +0xc |
| Callback `0x0007` | 32 | callback at +8, data at +0x10, flags at +0x18 |

- Option size 0xd8 is derived, not captured whole: the classic 0xa8 sampler
  option plus the 0x30-byte common expansion, with `max_voices` at +0x50.
- Formats: PCM_I16L (0x12) and PCM_F32L (0x18), 1 or 2 channels, 1 to 192000 Hz,
  `frame_offset` and `frame_margin` zero. Float samples must be finite.
- Blocks flags: the native caller adds one block with flags 4 (reset). Only 4 is
  accepted: the blocks replace the voice's waveform, cursor and phase. Flags 0,
  1 (continue) and other bits have no native evidence and are refused. A voice
  ends when its queue drains.
- Blocks are copied (and decoded) at add time. Host budgets, checked from the
  block headers before any guest read or allocation: 64 blocks per add, 4M frames
  per block, 32 MiB of queued float frames per voice, and 128 MiB of queued float
  frames per system. The 128 MiB limit covers queued data after adds only, not
  peak memory: during an add the voice's old queue is still held while the new
  one is built (each within 32 MiB), plus one raw block read or one superframe at
  a time; adds are serialized per system by its state lock. A rejected add
  changes nothing. Host allocation failure is not recoverable and follows the
  project-wide out-of-memory policy.
- Resampling is linear from the source rate to the system output rate, with the
  position carried across grains and blocks. Across a block end the next sample
  is the loop start when the block repeats, else the next block's first frame.
- Pitch is limited to 16 (a host limit, logged when exceeded), so a voice moves at
  most 64 source frames per output frame. Every voice (sampler and custom PCM)
  is interpolated and gained in double precision and summed into one double
  accumulator for the whole system, with no per-voice clip; the sum is clamped
  once to +-1 when the float output is written. Finite sources and gains stay
  finite in double, and opposite-sign voices are combined before the clamp.
  Floating-point summation can still round according to voice order.
- Callbacks: only block end (flag 1) is confirmed on a guest. Registration with
  any other flag, including block repeat (2), is refused. Repeat passes play
  without a callback. The info carries the guest's block user data unchanged.
- Callbacks run after the dispatching thread releases its system lease and state lock, each
  under a per-system dispatch mutex. A queued callback runs only while its voice
  is still registered, active and the same record; a callback that destroys its
  voice, rack or system stops the later callbacks for those voices. A destroy on
  another thread waits for a running callback before erasing records.
- A render on a thread that holds the explicit system lock does not run
  callbacks; they are held for the system and run at unlock. At most 16384
  callbacks are admitted per render or held per system. A render that could
  exceed the applicable budget is refused with the render's invalid-control
  result before any voice state changes; nothing is dropped. Deferred callbacks
  run after the dispatching thread releases its explicit lock; another thread
  can acquire the state lock concurrently.
- Destroying a rack (like a system) while this thread holds that system's lock
  is refused as an invalid rack, before anything is marked closing. This is a
  host qualification: a locked destroy is not supported here, and no separate
  native error value is assumed.
- The voice state reports `num_decoded_samples` at +0x10 of the 48-byte state.
- Stop-immediate clears the queue. Setup resets the voice to Empty.

Unresolved: the continue flag and any non-reset add, the meaning of the setup
word at +0x20, the block repeat callback, and stop, pause and resume event values
beyond play, stop-immediate and gain.

## NGS2 ATRAC9 path

Waveform format 0x40 uses `ElementaryAudioDecoder` (in `AudioVideoBackend`), which
wraps the FFmpeg build's decoder. Its extradata is the 12-byte RIFF format tail:
version (zero in the standard setup, which carries no version word; the decoder
accepts 2 or less), the FE-prefixed config, and a reserved word. Setup opens a
decoder once and fails unless the channel count and rate it derives from the
config equal the format. The config needs a clear verification bit and an even
superframe index.

Decoder limits from the public config contract: up to 8 channels, 192 kHz,
frames of at most 256 samples and 2048 bytes, 1 or 4 frames per superframe, so a
packet (superframe) is at most 8192 bytes and 1024 samples per channel.

Each reset add decodes its blocks in queue order through one new decoder stream,
with no reset between blocks; the stream and queue replace the previous ones only
when every block decoded. Every superframe of the add's stream, across all of
its blocks, must yield the same frame count and finite samples. Repeats replay the block's decoded frames, and two blocks that
share a superframe decode it twice; neither is confirmed against a guest.

The unit tests decode a generated silent superframe (mono, 48 kHz) built from the
decoder's public bitstream rules, through the decoder and through a sampler
voice. Because the frame is silent, this proves the decode path, the 12-byte
extradata and the per-superframe frame count, but not the audio content, the
skip-sample position, or multi-superframe seams. Those, and non-silent audio,
are checked only when `KYTY_NGS2_ATRAC9_TEST_WAVEFORM` names a lawfully obtained
ATRAC9 RIFF file outside Git; that path stays unverified until root runs it.

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

- ATRAC9 decoding is checked only with a generated silent superframe; non-silent
  output needs a private fixture (see the ATRAC9 path section). Unverified
  CustomSampler formats still fail at their real frontier instead of returning
  decoded silence.
- SDL can convert the supported PCM channel counts, but semantic surround
  speaker placement beyond the guest `8ChStd` ordering still needs host
  capability evidence.
- NGS2 rack/system destruction and long-running voice reclamation require
  explicit lifecycle exports before host state can be reclaimed safely.

## Related graphics boundary

AGC direct-resource index `1` is a Shader Resource Table pointer, not an inline
sampler or storage descriptor. Do not reinterpret it to continue execution;
that changes shader bindings and can surface unrelated SPIR-V failures.
