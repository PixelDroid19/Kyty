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
- render linearly resamples to the system rate; the result reaches an output
  only through the voice graph below.

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
  is interpolated in double precision into its own bus and summed down the voice
  graph in double, with no per-voice clip; each render buffer is clamped once to
  +-1 when it is written. Finite sources and gains stay
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
- Stop-immediate and kill clear the queue. Setup resets the voice to Empty with
  a unit port volume.

Unresolved: the continue flag and any non-reset add, the meaning of the setup
word at +0x20, and the block repeat callback.

## NGS2 voice commands and port state

`Ngs2VoiceRunCommands` takes 16-byte records `{u32 id, u8 flags, u8 type,
u16 count, 8-byte value}`; Kyty reads the first 12 bytes, the last word is
padding the guest leaves uninitialized. Two independent references agree on:

| Command | Type | Value |
| --- | --- | --- |
| Event `2` | `0x400` (unsigned) | one bit: play 0x1, stop 0x2, stop-immediate 0x4, kill 0x8, pause 0x10, resume 0x20 |
| Port volume `6` | `0x100` (float) | linear gain of port 0 |
| Port matrix `7` | `0x300` (integer) | refused: no matrix is implemented |

Play on a sampler needs queued blocks; a pause keeps the cursor and a resume
continues from it. A source voice and a submixer take the port volume.

`Ngs2VoiceGetPortInfo(voice, 0, info, 24)` writes `{i32 matrix_id, f32 volume,
u32 num_delay_samples, u32 destination_input, uintptr destination}`: matrix -1,
the port volume, no delay, input 0 and the patch destination as the guest named
it (0 once the destination is gone). Another port or size is refused without a
write. A title ramps fades by reading the volume back.

## NGS2 voice graph

Patch (`0x5`, 24 bytes, destination at +16) connects port 0 of a source,
submixer or reverb voice to input 0 of a submixer, reverb or mastering voice of
the same system; the destination is a voice handle or the handle of a one-voice
rack. Cycles, mastering sources and source-voice destinations are refused. Each
grain, every source voice renders into its own stereo bus, then the routing
voices run in decreasing distance from the end of their route:

- a playing submixer (setup `0x20000000` with 2 channels) passes its input on at
  its port volume;
- a playing reverb voice passes it on through its reverb, every grain, with a
  silent input when nothing feeds it;
- a playing mastering voice (setup `0x30000000`: 1, 2 or 8 channels; output
  `0x30000005`: render buffer index) adds it to its render buffer: stereo as is,
  mono as the mean, eight channels on the front pair. A stereo mastering voice
  may also write the front pair of an eight-channel buffer, whose other channels
  stay silent; any other channel count that differs from the setup refuses the
  render.

Every render buffer is cleared, and a buffer shorter than one grain is refused
before anything is written. A voice that is not patched reaches no output.

## NGS2 reverb

Rack `0x2001`. Setup `0x20010000` (24 bytes: input and output channels, flags,
reserved) admits only `{2, 2, 0, 0}`, as the bus is stereo; a setup clears the
tail and loads the block the guest library fills by default (wet 1, dry 0, room,
reflections and reverb at -10000 mB), so a voice that was only set up is silent.
The I3DL2 block `0x20010001` (96 bytes) holds wet and dry gains, then room,
room_hf, reflection_pattern, decay_time, decay_hf_ratio, reflections,
reflections_delay, reverb, reverb_delay, diffusion, density and hf_reference,
then eight reserved words that are not checked (the guest library writes 8 to
the first). Two references agree on this layout; the values one title sends
match the published I3DL2 cave preset.

Non-finite fields are refused and the rest are clamped to the I3DL2 ranges. The
host rendition (`AudioNgs2Reverb`) is derived from the published I3DL2
definitions, not from native output:

- -10000 mB is silence; other levels are `10^(mB/2000)`;
- early reflections are four taps spread over reverb_delay after
  reflections_delay, at room * reflections;
- the late reverb is a four-line feedback delay network fed
  reflections_delay + reverb_delay after the input, at room * reverb; each line
  decays by 60 dB in decay_time, tending to decay_time * decay_hf_ratio above
  hf_reference, and density scales the line lengths;
- room_hf attenuates the wet path above hf_reference;
- the output is `dry * input + wet * reverb`. The wet/dry reading rests on the
  field names and one implementation: an assumption, not a measured contract.

An identical block re-sent every update changes nothing; an immediate stop or
kill clears the tail. Buffers are sized at setup for the I3DL2 maxima, and a
render does not allocate.

Unresolved: the reflection patterns (one generic pattern is rendered for every
value), the absolute levels against native output, and setups other than
stereo. A title that sets up an eight-channel reverb stays refused.

## NGS2 guest allocators

`SystemCreateWithAllocator` and `RackCreateWithAllocator` take a 24-byte
allocator (alloc handler, free handler, user data) in place of a buffer info.
Two references agree that each handler takes the 64-byte buffer info
(host_buffer at +0, host_buffer_size at +8, user_data at +56). Kyty snapshots the
option first (an unreadable or unsupported one is an invalid option), then calls
the alloc handler with `{null, size, user_data}`, where size is what the
matching QueryBufferSize reports, and creates the object on the buffer the
handler returns. An allocator without both handlers is refused before anything
is allocated; a nonzero handler result is returned as is, and a null buffer is
an invalid buffer address. The free handler gets the buffer back, with the same
size and user data, when the create fails after the allocation, when the rack
is destroyed, and for every remaining rack and the system itself when the
system is destroyed (outside the system's state lock).

## NGS2 waveform parsing

`Ngs2ParseWaveformData` reads guest memory and `Ngs2ParseWaveformFile` reads a
guest path at a byte offset; both fill the same waveform info. Accepted images
are VAG and RIFF WAVE carrying ATRAC9 (tag 0xfffe) or uncompressed PCM: tag 1
with 16-bit samples or tag 3 with 32-bit float samples, whose block align equals
one frame. PCM reports one sample per audio unit and frame, and its sample count
comes from the fact chunk when present, else from the data size. A file parse
reports data and block offsets from the file start, so a title adds them to the
offset of its own read. Other images are refused.

## NGS2 ATRAC9 path

Waveform format 0x40 uses `ElementaryAudioDecoder` (in `AudioVideoBackend`), which
decodes through the bundled LibAtrac9 (`source/3rdparty/LibAtrac9`, unmodified
upstream sources) and does not need FFmpeg. Its extradata is the 12-byte RIFF format tail:
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

- ATRAC9 decoding is checked only with generated superframes; non-silent
  output needs a private fixture (see the ATRAC9 path section). Against FFmpeg's
  decoder, private superframes match within -42 dB in the coded band, with
  inverted polarity and a different band-extension reconstruction; no hardware
  reference decides between them. Unverified
  CustomSampler formats still fail at their real frontier instead of returning
  decoded silence.
- SDL can convert the supported PCM channel counts, but semantic surround
  speaker placement beyond the guest `8ChStd` ordering still needs host
  capability evidence.
- NGS2 rack/system destruction and long-running voice reclamation require
  explicit lifecycle exports before host state can be reclaimed safely.
- HEVAG sampler waveforms are refused. A roguelike title sets up its sampler
  voices with waveform type `0x1c` on `VAGp` data whose header version is
  `0x00020001` and whose frames use predictor indices above 4 combined with
  the high nibble of the flag byte. That is the high-efficiency variant, which
  needs a 128-entry, four-tap predictor table; no source for that table fits
  the data boundary here (the local references that carry it are GPL, no
  native module is available, and the system FFmpeg decodes only classic
  PS-ADPCM). The same title adds blocks with flags 0 right after a setup, and
  builds its graph from a custom submixer rack `0x4002` with the 0x30-byte
  option expansion (0x508 bytes: module 0 is id `0x1d`, which no reference
  defines), eight-channel reverb and custom submixer setups, and an
  eight-channel mastering voice. Its samplers patch straight to mastering, so
  the waveform format is the first blocker. Direction: obtain the predictor
  table from a permissible source (the native library or a published
  contract), then admit block adds with flags 0 on an empty queue.

## Related graphics boundary

AGC direct-resource index `1` is a Shader Resource Table pointer, not an inline
sampler or storage descriptor. Do not reinterpret it to continue execution;
that changes shader bindings and can surface unrelated SPIR-V failures.
