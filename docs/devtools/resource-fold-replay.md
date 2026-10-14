# Resource-fold replay

Offline, exact replay of the descriptor-table reads that `ShaderParseUsage2`
makes while folding resource descriptors. It lets a developer reproduce the
evaluator's output for one captured call without a guest, a GPU, or a running
title.

## What is captured, and what is not

This artifact is an **exact evaluator-input replay**. It is **not** an ISA
parser replay.

- The captured input is what `ShaderParseUsage2` consumes: the owned
  `ShaderCode` (every instruction and operand field, labels, embedded-shader
  flags), the user SGPRs, the `ShaderUserData` metadata, the register base, the
  vertex-resource flag, and the configured guest platform (the sampled-image
  path reads it).
- The caller's output objects at entry are input too. `ShaderParseUsage2` adds
  to the bind and never resets it, and it resets every usage field except
  `vertex_attrib` and `vertex_attrib_reg`. The callers seed only the bind
  layout: the pixel path sets `push_constant_offset` and `descriptor_set_slot`
  from the vertex stage, and every path sets `push_constant_size`. These five
  values are serialized as `initial_output` and seed the replay.
- A capture is exact only for a bind that, apart from those layout seeds,
  equals a freshly initialized `ShaderBindResources`. The canonical text, which
  lists every bind field, is used for that comparison. Any other prepopulated
  bind state is not representable, so the capture is dropped and production is
  unchanged.
- Raw instruction words are **not** captured. `ShaderCode` does not retain
  them, and the live parser is not widened to keep them.
- The offline tool rebuilds an identical owned `ShaderCode` from the document and
  runs the production `ShaderParseUsage2` on it. The ISA decoder is not part of
  the replay.

A `code == nullptr` parse is captured as `code_present = 0` and replays with
`nullptr`.

## Where the read seam is

`ShaderSnapshotGuestDescriptorTable` (`Shader.cpp`) performs its two production
reads through `ShaderReadGuestDescriptorWords`. With no reader installed, that
calls `Core::VirtualMemory::CopyFromGuest` exactly as before. A thread-local
scoped reader (`ScopedShaderGuestDescriptorReader`, declared in
`ShaderStorageAnalysis.h`) can override it for one call:

- `ShaderResourceFoldRecorder` calls the real `CopyFromGuest` and records
  `(address, dwords, success, words)` in production order. The stability double
  read and its retry are recorded as separate entries. Failed reads are recorded
  too. Recording stops at 256 reads and reports overflow.
- `ShaderResourceFoldReplayReader` answers only the request at the current
  cursor from the transcript. It never calls `CopyFromGuest`. A missing,
  out-of-order, invalid, or unreadable read is refused with a machine token on
  stderr (`status=missing|order|invalid|unreadable`). The production refusal
  path then runs. A transcript that is not fully consumed reports `unused`, which
  fails the replay.

The dynamic SRT consumer (`ShaderResources.cpp`) calls the same function, so its
reads are covered by the same seam. A search of `Shader.cpp`,
`ShaderResources.cpp` and `ShaderStorageAnalysis.cpp` finds no other guest read
in the `ShaderParseUsage2` call tree. The remaining raw guest-pointer reads in
`Shader.cpp` belong to the legacy `ShaderParseUsage` and to vertex-fetch decoding.

## Capture

Capture is default-off. It runs only when `KYTY_RESOURCE_FOLD_CAPTURE_DIR`
names an existing directory.

- At most 8 captures per process. Captures are never nested.
- The inputs are serialized when `ShaderParseUsage2` starts. At exit they are
  serialized again. If anything differs, the capture is dropped as unstable
  instead of being recorded as exact. A change that is reverted before exit
  cannot be detected this way. The initial output objects are copied at entry,
  so the exit check never compares the bind that the evaluator has written.
- Each document is at most 4 MiB, with at most 256 reads and at most 1 MiB of
  canonical output.
- Files are created with an exclusive open (`"wbx"`) under generated names
  (`resource-fold-<n>.json`). An existing file, symlink, or FIFO is never
  replaced. A partial file created by a failed write is removed.
- A capture that is unstable, overflowing, unrepresentable, or cannot be
  written is dropped with a limited warning. The production result is
  unchanged; there is no alternate success path.
- The output directory must be outside the repository. Captures can contain
  title-derived data and must not be committed.

## Document schema (version 3)

The document is one JSON object. Keys appear exactly once, in the fixed order
below. Numbers are decimal integers with no fraction, exponent, or leading
zeros, and at most 10 digits. The only string is `canonical_output`: printable
ASCII, with `\n` as its single escape.

1. `schema_version`: `3`. Earlier versions are rejected.
2. `parse`: `guest_platform` (1 = PS4, 2 = PS5), `user_sgpr_num`,
   `user_data_register_base`, `vertex_resource_types`, `initial_output`
   (`push_constant_offset`, `push_constant_size`, `descriptor_set_slot`,
   `vertex_attrib`, `vertex_attrib_reg`), `user_sgpr` (`count`, 32 `value`, 32
   `type`), `user_data`, `code_present`, and `code` when present.
3. `user_data`: `eud_size_dw`, `srt_size_dw`, `direct_count`, `direct_present`,
   `direct`, and four `sharp` categories (`count`, `present`, `entries` as
   `[offset_dw, size]`). A nonzero count requires its array.
4. `code`: `type`, the embedded flags and ids, `continuation_pc`, `hash0`,
   `crc32`, `labels`, `indirect_labels` (`[dst, src]` pairs), and
   `instructions`. Each instruction is a 38-element array; each operand is a
   15-element array. The 64-bit `format` is two 32-bit values.
5. `transcript.reads`: `address_hi`, `address_lo`, `dwords` (1..256),
   `success`, and `words` (exactly `dwords` entries on success, none on failure).
6. `canonical_output`: the full evaluator output text.
7. `fingerprint_hi`, `fingerprint_lo`: FNV-1a 64 of `canonical_output`, as a
   summary. The replay compares the canonical text byte for byte; the
   fingerprint alone is never treated as equality.

The canonical output is a line-based `key=value` list covering every
`ShaderParsedUsage` field and every `ShaderBindResources` field. Fixed arrays
are listed in full, including entries past the live counts, so a write past a
count is visible.

## Bounds and rejection

The reader is schema-directed and does not build a generic JSON tree. Each
array grows one checked element at a time, and every limit is checked before
that element is stored. These are all rejected as malformed before the
evaluator runs:

- grammar violations: trailing data, incomplete input, unknown, duplicate,
  missing, or reordered keys, NUL bytes, oversized documents;
- numeric violations: fractions, exponents, leading zeros, values out of range;
- size violations: nesting past 16, over-long arrays, a fingerprint that does
  not match the canonical text.

Metadata is also checked, without repeating any shader-level classification:

- a nonzero direct or sharp count without its array;
- a direct register whose minimal descriptor would extend past the
  `HW::UserSgprInfo::SGPRS_MAX` user SGPRs. The minimal widths are 4 dwords for
  a default slot, 8 for type 8 (a T#), 4 for type 10 (an S#), and 2 for the
  type-5 EUD pointer, whose pair is read without a window check;
- sampler metadata that certainly exceeds 16 samplers.

The evaluator's texture, sampler, storage, zero-scalar-buffer, and GDS-pointer
writers refuse a full array or an out-of-window start themselves before any
access (production `EXIT`). The user window is `HW::UserSgprInfo::SGPRS_MAX`;
extended sources must start at index 16 or above. So when the code or the
descriptor contents decide the resource kind, the case is refused during
replay instead of being written out of bounds. One example is a default slot
in the last four user SGPRs that the code samples as an 8-dword T#.

## Tool

`kyty_resource_fold_replay <document.json> [--print-output | --validate-only]`

The tool initializes the same subsystem set as the graphics integration
binaries and sets the guest platform to PS5 explicitly. It replays PS5
documents only.

Stdout can begin with startup diagnostics printed by runtime initialization, so
stdout as a whole is not the canonical output. With `--print-output`, the
canonical output appears as one length-framed payload:

```text
resource-fold-canonical-begin bytes=<N>
<exactly N payload bytes>resource-fold-canonical-end
fingerprint=<hex> output_matches_document=<0|1>
```

Readers take exactly `N` bytes after the begin line and check that the end line
follows them. The canonical text consists only of `key=value` lines, so it never
contains the markers. The fingerprint line is a summary; equality is decided by
the CLI's byte comparison.

| Exit | Meaning |
| --- | --- |
| 0 | Every recorded read was consumed and the output matches byte for byte. With `--validate-only`, the document passed schema and safe-input validation; a semantic refusal by the evaluator can still occur during replay. |
| 1 | Usage error. |
| 2 | Malformed, out-of-bound, or non-PS5 document. |
| 3 | Unused reads remain in the transcript. This is a failure, not a warning. |
| 4 | The replay consumed the transcript, but the output differs from the document. |
| 5 | The runtime configuration could not be initialized. |

A refused replay (missing, out-of-order, or unreadable read) halts through the
production `EXIT` path with a nonzero status. The replay reader prints the
machine token first.

The wrapper `scripts/kyty_resource_fold_replay.py` delegates both `validate` and
`replay` to the CLI. It takes `--binary` or `KYTY_RESOURCE_FOLD_REPLAY_BIN`, and
returns 6 when the CLI is unavailable. It performs no validation of its own.
`validate` is the CLI's schema and safe-input check, not a replay.

## Limits and claims

- A replay reproduces the evaluator output for the captured inputs and reads.
  It does not establish guest coherence, render correctness, or equivalence on
  other hosts or GPUs. The captured reads are what one run observed.
- Entry and exit input comparison cannot detect a mutation that is reverted
  before the evaluator returns.
- Debug-printf payloads are not captured; such calls are dropped.
- The capture runs on the live host. A fatal `EXIT` before the scope ends
  produces no document.
- Before this tool is trusted on a title, its synthetic tests and a real
  private capture-to-replay run must both pass on the owning build.
