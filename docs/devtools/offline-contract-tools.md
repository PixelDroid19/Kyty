# Offline contract tools

Two standalone Python tools under `scripts/` check graphics contracts without
running a title. Neither is a compatibility result, a synchronization proof, or
a hardware measurement beyond what each section states. They use only the Python
standard library and read only the files you pass. Keep every input and output
file outside Git. Captures are private evidence.

Run the tests with:

```bash
python3 -I scripts/tests/test_operation_graph.py
python3 -I scripts/tests/test_hardware_oracle.py
```

## 1. Operation dependency graph (`kyty_operation_graph.py`)

Builds a diagnostic graph from ordered operation records. Each record is a
draw, dispatch, DMA or resolve with explicit resource spans.

- A read span is linked to the nearest preceding writer whose bytes overlap it.
  Each still-unattributed byte is charged to the nearest writer that covers it,
  so partially overlapping writers each produce an edge for their part.
- Bytes with no earlier writer become `external_inputs`.
- A record's reads observe the state before that record. A read-modify-write
  span (read and write of the same bytes in one record) therefore reads from
  earlier writers and then publishes its write for later records.
- Later records never satisfy an earlier read.

```bash
python3 scripts/kyty_operation_graph.py operations.json --json graph.json --dot graph.dot
```

Input schema `kyty_operation_graph_input_v1`:

```json
{"schema": "kyty_operation_graph_input_v1",
 "records": [
  {"id": "dma0", "kind": "dma", "spans": [{"resource": "vb", "offset": 0, "size": 64, "access": "write"}]},
  {"id": "draw1", "kind": "draw", "spans": [{"resource": "vb", "offset": 0, "size": 16, "access": "read"}]}
 ]}
```

- `kind` is one of `draw`, `dispatch`, `dma`, `resolve`. `access` is `read` or `write`.
- Identifiers (record ids and resource names) must fully match
  `[A-Za-z0-9_.:-]{1,128}`. Trailing newlines and surrounding text are rejected.
- Offsets and sizes are integers with `size >= 1` and `offset + size <= 2^48`.
  Booleans, floats, NaN and duplicate JSON keys are rejected.
- Rejected as conflicting: duplicate record ids, overlapping writes inside one
  record (no defined final value), unknown keys, and an empty or oversized span list.

Bounds (all fail with an actionable error, never a partial output):

| Bound | Value |
| --- | --- |
| Input file | 4 MiB |
| Records / spans per record / total spans | 4096 / 64 / 65536 |
| Candidate writers examined | 1,000,000 |
| Unattributed fragments per read span | 256 |
| Output items (edges + external inputs, deduplicated) | 131072 |

Output schema `kyty_operation_graph_v1` always sets `diagnostic_only: true` and
`synchronization_proof: false`. Edges and external inputs are sorted by reader
order, resource and offset, so identical input gives identical output. An edge
means "in this trace, these bytes were last written by that record". It does
not show that hardware waited for the write. Do not remove a barrier, wait or
fence because the graph has no edge.

The DOT rendering keeps names in disjoint namespaces: operation nodes are
`"op:<id>"` and generated external sources are `"ext:<resource>"`. An operation
id such as `external:vb` cannot collide with the external source for `vb`.

CLI exit status: 0 success, 1 invalid input or unreadable/unwritable file.

## Live producer (opt-in, default off)

The emulator records draws and dispatches itself. It is off unless the process
starts with `KYTY_TRACE_OPERATIONS_DIR` set to an absolute directory. Nothing is
recorded or written otherwise, and no per-frame logging is emitted.

- Hooks run in the indexed draw path (`GraphicsRenderDrawIndex`, after the index
  and resource bindings are set, before the draw command) and in the direct
  dispatch path (`GraphicsRenderDispatchDirect`, before `vkCmdDispatch`). They
  build spans from data already in scope and never change draw behavior.
- Operations are recorded in CPU command-recording order under the render lock.
  This does not establish execution order across GPU queues. An operation with no
  exact span is omitted and counted as dropped, and recording continues after it.
  Only a cap stops recording: every later operation is then dropped and counted.
- Each run writes two files into the directory, with exclusive creation. An
  existing file, symbolic link or FIFO at the target makes creation fail. Nothing
  is overwritten or followed. The stem is `operations-<milliseconds>`:
  - `<stem>.operations.input.json`, the `kyty_operation_graph_input_v1` document
    (absent when no operation was recorded);
  - `<stem>.operations.meta.json`, the `kyty_operation_trace_meta_v1` sidecar.
- Files are written once, at normal process exit (or by the explicit flush).
- Forced termination and `_Exit` bypass the normal-exit flush. A bounded guest
  run may therefore leave no trace files. The owned producer integration uses
  the explicit flush; it does not change a guest's termination behavior.
- Failure policy. Host allocation failure follows the project-wide out-of-memory
  policy. It is not recoverable, and the trace does not record it as truncation.
  Exceptions are not used. A file-creation failure logs one warning and writes
  nothing more. The input file is created before the meta sidecar. If the sidecar
  then fails, the input remains without its sidecar. The adapter then reports
  completeness as absent, never as complete.

Caps: 4096 records, 65536 spans in total, 64 spans per record, and 4 MiB across
both documents. The per-record cap is 64 spans. The byte cap can trigger before
the total span cap. Whichever cap is reached first sets `truncated`.

**Address model.** Every span is `{"resource":"guest","offset":<guest virtual
address>,"size":<bytes>}`. One resource ID covers all guest memory, so aliases
overlap in the graph. Offsets are guest virtual addresses from the packet or
descriptor. Host pointers are never written. These files are private captures:
they contain guest addresses and must not be committed.

**What is recorded, and only when its extent is exact.**

| Span | Source | Access |
| --- | --- | --- |
| Index buffer | index address and count times 2 or 4 bytes | read |
| Vertex buffer | vertex input descriptor, `stride * num_records` | read |
| Storage buffer (VS or PS, draw; compute, dispatch) | buffer descriptor, `stride * num_records` | read; read-write also writes after its read |
| Depth plane | depth layout size | read when depth test is enabled; write when depth write is enabled and not suppressed |

Read-write storage yields a read span followed by a write span for the same
range. A read-modify-write therefore reads prior state and then publishes, and
the graph never makes it its own writer.

**What is not recorded.** Each case sets an explicit reason on the record. No
span is guessed.

| Reason | Meaning |
| --- | --- |
| `index_type_unknown` | index type is neither 16-bit nor 32-bit |
| `color_attachment_extent_unknown` | color target has no exact size in the current model |
| `htile_metadata_not_recorded` | depth metadata is not a data span |
| `stencil_access_not_recorded` | stencil plane present; its access is not modeled |
| `attachment_clear_not_recorded` | depth clear is a load-op write with no span |
| `texture_extent_unknown` | sampled textures need format and layout to size |
| `storage_image_extent_unknown` | storage images need format and layout to size |
| `device_address_resources_not_spanned` | device-address descriptor tables are not spanned |
| `gds_pointers_not_spanned` | GDS pointers are not spanned |
| `storage_usage_unknown` | storage usage is unknown, so access is not guessed |
| `conflicting_write_span` | a second write overlaps a write already in the record |
| `span_limit_reached` | more than 64 spans in one record |
| `invalid_span` | address or size is zero where data is required, or leaves the 48-bit space |
| `operation_without_exact_spans` | the operation has no exact span, so it is dropped and counted |

Operations that are not hooked at all are listed in the meta sidecar's
`not_traced_kinds`: `auto_draw`, `indirect_draw`, `indirect_dispatch`,
`depth_stencil_copy_draw`, `dma`, and `resolve_and_copy`. The meta sidecar also
counts `operations_seen`, `records_emitted` and `operations_dropped`. This list
means the trace covers only indexed draws and direct dispatches. A graph built
from it is not a complete account of the frame.

**Adapter.** Pass the sidecar with `--meta`:

```bash
python3 scripts/kyty_operation_graph.py <stem>.operations.input.json \
    --meta <stem>.operations.meta.json --json graph.json
```

The sidecar must describe the input it is paired with. Its `input_md5` field is the
lowercase MD5 of the exact input bytes, and the adapter rejects the pair when the
digest of the file it reads does not match. Matching record counts alone are not
accepted as pairing. This is a content-consistency check for accidental mismatches,
not an authentication mechanism. `records_emitted` must equal the number of records
in the input, and the counts must reconcile with `operations_seen`.
The graph output then carries a `completeness` object. Without `--meta` it reads
`{"metadata": "absent"}`. With it, `traced_operations_complete` is true only when
the trace was not truncated, no operation was dropped, and no record is
incomplete. The untraced kinds are always listed. A graph is never reported as
complete for the whole frame.

## 2. Integer instruction oracle (`kyty_hardware_oracle.py`)

Compares the integer results of one arithmetic instruction across two backends
for the same operand pairs: native AMD gfx1030 execution, and a Vulkan endpoint
that runs the production shader pipeline. The five operations are `add`, `mul`
(low 32 bits), `mulhi` (high 32 bits, unsigned), `shl` and `shr`.

**Native backend (`run`).** The kernel computes each result with one AMDGCN
instruction, written as inline assembly, using the published operand order:

| Op | Instruction | Operands (`D = src0 op src1`) |
| --- | --- | --- |
| `add` | `v_add_nc_u32` | x, y |
| `mul` | `v_mul_lo_u32` | x, y |
| `mulhi` | `v_mul_hi_u32` | x, y |
| `shl` | `v_lshlrev_b32` | count y, value x: `x << (y & 31)` |
| `shr` | `v_lshrrev_b32` | count y, value x: `x >> (y & 31)` |

There is no C++ arithmetic fallback. Before the binary runs, the compiler's
device assembly (`-S --cuda-device-only`) must contain each of the five
mnemonics as an instruction. A mnemonic that appears only in a comment does not
count. A missing one yields `asm_check_failed`. Only `gfx1030` is accepted. Other
architectures are refused at validation with an explicit message.

The encodings were checked against the LLVM AMDGPU assembler (clang 23,
`-mcpu=gfx1030`). `v_add_nc_u32` is VOP2 opcode `0x25`, `v_lshrrev_b32` is `0x16`,
`v_lshlrev_b32` is `0x1a`, `v_mul_lo_u32` is VOP3 `0x169`, and `v_mul_hi_u32` is
VOP3 `0x16a`. The AMD RDNA2 ISA document itself was not reachable from the
build host, so the primary-document check is not claimed here.

**Operands.** `--mode seeded` (reproducible from `--seed`) and `--mode edges`
generate pairs. `--input-file PATH` reads an absolute regular file of
little-endian uint32 values: `count` operands `a`, then `count` operands `b`.
`count` is 1 to 65536. The file's MD5 is recorded as `input_md5`.

**Metadata.** A measured result records `compiler_version`, `device_name`,
`gcn_arch_name`, `warp_size`, `runtime_version` and `driver_version` from the
device, validated against a strict schema. `warp_size` must equal `--wave-width`,
and the device architecture must be `gfx1030`.

**Vulkan endpoint (`vulkan-run`).** The endpoint is the executable
`kyty_shader_arithmetic_oracle`, built from
`source/integration_test/src/shader_compute/ShaderArithmeticOracle.cpp` by the
`kyty_shader_arithmetic_oracle` target in `source/integration_test/CMakeLists.txt`.
It is not a CTest case, because it needs a Vulkan device with paired-wave support.

Build and run (root-owned; the commands are not run by this document):

```bash
cmake --build <build-dir> --target kyty_shader_arithmetic_oracle
python3 scripts/kyty_hardware_oracle.py vulkan-run --op mulhi --input-file /abs/pairs.bin \
    --wave-width 64 --out-dir /abs/new-run-dir --endpoint /abs/<build-dir>/kyty_shader_arithmetic_oracle
```

How it computes a batch. Each dispatch takes up to 16 pairs. The guest program
loads operand `a[k]` into `v(2k)` and `b[k]` into `v(2k+1)` with `v_mov_b32`
literals read from the input file. It then executes the requested operation once
per pair, writing `v(32+k)`. The encodings are:

- `v_add_nc_u32`: VOP2 opcode `0x25`.
- `v_lshrrev_b32`: VOP2 `0x16`. `v_lshlrev_b32`: VOP2 `0x1a`. The count is in
  src0 (low five bits) and the value is in vsrc1.
- `v_mul_lo_u32`: VOP3A `0x169`. `v_mul_hi_u32`: VOP3A `0x16a`.

The words are generated by the endpoint, then lowered by the production parser and
SPIR-V emitter, assembled by the production toolchain, and dispatched through
`VulkanComputeProbe::DispatchWave`. Probe instrumentation only observes the VGPR
results. It never inserts arithmetic. The results are read as float slots and
bitcast to uint.

Every logical lane runs the same program with uniform literals. The endpoint
therefore requires each pair's result to be identical across all 64 lanes and
both banks. Any difference is a failure. Nothing is averaged or chosen.

**Limits.** At most 1024 pairs per run (16 pairs times 64 dispatches), and at most
64 dispatches. An input larger than 8192 bytes is refused by size before it is
read. `--wave-width` must be 64. Logical wave 32 is reported as `unavailable`
with reason `logical_wave32_unsupported`, and no endpoint is launched. Output
words per dispatch are 64 lanes times the batch size, within the 4096-word probe
limit. Each batch builds its own program, because the operands are literals in
the guest code. Pipelines are therefore not shared across batches.

**Capability and exit status.** The endpoint exits 77 when the compute capability
is unavailable before any dispatch. The Python side reports `endpoint_unavailable`
with exit 3. Exit 2 is a refused request (`endpoint_refused`). Any other failure
is `endpoint_error`. On every failure, the endpoint writes no output and no
metadata, so a failed run cannot produce a measured result.

**Protocol.** The endpoint writes a `kyty_shader_integer_endpoint_v1` report with
the fields `backend`, `op`, `pairs`, `dispatches`, `physical_device`,
`subgroup_size` and `measured`. It may also include `default_subgroup_size`. The
report is written only after every dispatch succeeded, with `measured` true. The
`subgroup_size` is 32, the physical lanes the paired logical wave64 layout runs on.
The driver's default subgroup size is recorded separately and is not conflated
with it. `physical_device` is the selected device name, restricted to printable
characters the validator accepts. The Python side checks that `dispatches` equals
`ceil(pairs / 16)`. A subgroup difference is reported as
`subgroup_mode: differs_qualified`.

**Not measured here.** No Vulkan result or AMD result exists in the tree until
the root runs the endpoint on a supported device and compares the output with the
native backend.

**Result validation.** Result directories are untrusted input. `compare` reads only
`result.json` and the fixed file `raw/outputs.bin` inside each directory. A result
whose `outputs` field names any other location is refused, as is any malformed
field: a wrong type, an out-of-range count, an invalid digest, an unknown
operation, an unsupported architecture or wave width, or a `measured` value that
is not the boolean `true`. Output and metadata files must be regular files. They
are opened without following symbolic links and without blocking on FIFOs.
Oversized files are refused before their content is read, and the output size
must be exactly four bytes per pair. A refused result is a usage error (exit 2),
never a crash.

**Admission.** `--count` cannot be combined with `--input-file`. The operand file
defines the pair count, and an explicit count is refused rather than ignored.

**Compare (`compare`).** Takes a native result directory and a Vulkan result
directory. Both must be measured, of the right backend, with the same operation,
count and `input_md5`. Any other combination is refused or reported unavailable,
never compared. Compared exactly, lane by lane, with the first mismatch reported.

**Host reference.** Each measured result also records `host_reference`, the
exact host arithmetic on the same operands. It is ancillary. It is not the
oracle, and it does not decide the comparison.

**Statuses.** `measured` means the device or endpoint produced outputs that
passed validation. `unavailable` means no hardware or endpoint exists here, and it
never carries a comparison or artifact. Other failure statuses are
`unsupported_architecture`, `compile_failed`, `asm_check_failed`,
`compiler_version_unreadable`, `device_error`, `device_metadata_invalid`,
`wave_width_mismatch`, `arch_mismatch`, `timeout`, `output_size_mismatch`,
`endpoint_error`, `endpoint_refused`, `endpoint_unavailable`, `endpoint_metadata_invalid`,
`endpoint_not_measured`, `output_invalid` (the output is not a regular file) and
`output_size_mismatch` (the output is not exactly four bytes per pair).

**Files.** Outputs go to a new or empty directory outside the repository, which
is never overwritten. The summary printed to stdout contains counts, status,
artifact and comparison, but no host paths. Raw inputs, outputs, logs and the
binary stay in the output directory.

**Exit status.** 0 for `measured` or a `match`, 1 for a verification failure or
mismatch, 2 for a usage error, 3 for `unavailable`, and 4 for an I/O error.

**Status.** The probe and the unavailable path have been exercised on a host
without `/dev/kfd` and `hipcc`. The native kernel has not been compiled, and no
device has run it. The Vulkan endpoint source and target exist and have not been
built or run by this change; the root owns that build. The tests use labeled fake
runners that write files and check parsing, argument construction, exit-status
mapping and comparison rules. They do not measure hardware or run the endpoint.
No measured result exists in this tree.
