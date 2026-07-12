# NGS2 rack and voice handle ABI (learning note)

This note records **evidenced** Gen5 NGS2 contracts used by Kyty’s HLE.
It describes Kyty types and guest-visible behavior only. It does not document
third-party projects or private fixture identities.

## Common rack option prefix

On the 64-bit SysV layout used by this emulator, the classic
`Ngs2RackOption` common prefix is **0x80** bytes:

| Offset | Field |
| ------ | ----- |
| 0x00 | `size` (`size_t`) |
| 0x08 | `name[16]` |
| 0x18 | `flags` |
| 0x1c | `max_grain_samples` |
| 0x20 | `max_voices` |
| 0x24 | `max_input_delay_blocks` |
| … | further common fields / reserved |

`Ngs2RackQueryBufferSize` and `Ngs2RackCreate` allocate:

```text
host_buffer_size = sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * max_voices
```

Voices are laid out contiguously after the rack header.  
`Ngs2RackGetVoiceHandle(rack, voice_id, out)` returns a pointer into that array
when `voice_id < max_voices`.

## Gen5 extended option block

Some Gen5 rack option blobs are larger than the classic prefix. Observed sizes:

| Rack id | Option `size` | Notes |
| ------- | ------------- | ----- |
| 0x4001 (custom sampler) | 0x518 | Fully extended custom rack |
| 0x2001 (reverb) | 0xb8 | Classic reverb body + 0x30-byte extension |

When `option->size >= 0xb0` (classic 0x80 + 0x30 extension), the common numeric
fields are **not** at the classic offsets. Captured guest bytes place:

| Offset | Field (extended) |
| ------ | ---------------- |
| 0x48 | `flags` (after 0x30-byte extension past `size`) |
| 0x50 | `max_voices` |

Examples from strict Console capture (private fixture, not committed):

- Custom sampler 0x4001 / size 0x518: value at +0x20 is 0; value at **+0x50 is 256**.
- Reverb 0x2001 / size 0xb8: value at +0x20 is 0; value at **+0x50 is 16**.

Reading only `option->max_voices` (classic +0x20) under-allocates the voice
array (often **0** voices). Subsequent `Ngs2RackGetVoiceHandle` calls with
`voice_id` in range then hit an emulator invariant check.

## Resolution rule in Kyty

`Ngs2GetRackMaxVoices(rack_id, option)`:

1. If `option->size >= 0xb0`, read a `uint32_t` at **offset 0x50**.
2. If that value is non-zero, use it as `max_voices`.
3. Otherwise fall back to the classic `option->max_voices` field.

`Ngs2RackCreate` stores the resolved count in `rack->option.common.max_voices`
and initializes that many voice slots.

## GetVoiceHandle errors

| Condition | Result |
| --------- | ------ |
| `rack_handle == 0` | `0x804a0261` (invalid rack handle), `*handle = 0` |
| `max_voices == 0` or `voice_id >= max_voices` | `0x804a0300` (invalid voice handle), `*handle = 0` |
| In-range `voice_id` | `0` (OK), `*handle` points at the voice object |

Out-of-range voice indices are ordinary guest errors, not process exits.

## Tests

Focused coverage lives in `UnitTestEmulatorAudio.cpp`:

- Custom rack 0x518: buffer size scales with the +0x50 voice count.
- Reverb-style 0xb8 option: create with 16 voices at +0x50; handles for ids
  0 and 15 succeed; id 16 returns `0x804a0300`.

## Residual

Host mixing quality, full NGS2 module graphs, and later synchronization
frontiers are outside this note. Advance one strict fail at a time.
