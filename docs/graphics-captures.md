# Kyty graphics captures

`scripts/kyty_capture.py` is the reproducible graphics-debugging entry point.
It is deliberately small enough to run today and has a stable JSON manifest so
new probes can be added without changing the workflow.

The design follows the useful part of emulator GPU devtools: capture a bounded
frame window, keep the command/configuration evidence beside the image, and
make the visual gate deterministic. It is not a substitute for RenderDoc; when
RenderDoc is available, use the same capture window and keep its frame capture
beside the Kyty manifest.

The separate playable gate accepts `--runtime-cwd` for an existing writable
runtime directory and `--guest-script` for an independently located Lua input.
Both default to the usual repository layout; relative overrides are resolved
against the repository, not the selected cwd. Use these with `--fc-script` and
`--scratch` when binaries, caches and runtime writes must stay on a separate
filesystem. The local scratch `launch-config.json` records the effective paths
and binary/Lua hashes. These options change only launch locations, not visual,
input, or strict acceptance criteria.

The playable child also preserves explicitly provided shader optimization,
validation and wait-timeout settings; save/sandbox, temporary and shader/pipeline
cache paths; and bounded Mesa cache configuration. Empty or absent overrides
remain absent. This allow-list does not admit shader probes, automatic pad
routes, `INTEL_DEBUG`, preload hooks or permissive switches. Select the same
strict runtime configuration as the baseline rather than assuming every parent
environment variable reaches the child.

## Capture a run

```bash
export KYTY_GUEST_ROOT=/path/to/private/guest/root
python3 scripts/kyty_capture.py capture \
  --build _build_linux \
  --min-frame 1250 \
  --samples 3 \
  --interval 2 \
  --rt-evidence \
  --rt-evidence-ps 3f9d6677 \
  --framebuffer-evidence \
  --videoout-evidence \
  --tex-probe \
  --lightbuf-probe 1 \
  --key-at 700:x \
  --baseline _scratch_playable/captures/baseline.json
```

The command writes an ignored directory such as
`_scratch_playable/captures/` containing:

* `guest-*.log`: the complete guest log;
* `native_frames/*.png`: emulator-native readbacks of the emulated `VideoOut`
  source image, never screenshots of the desktop or window compositor;
* `*.png.json`: per-frame metadata with title/version when the loaded content
  exposes it, build revision, Vulkan format, source extent, present count, and
  capture milestone;
* `capture-*.json`: a sanitized manifest containing commit, host, capture
  configuration, artifact names, and deterministic image metrics.

The private guest root is used to start the process but is never written to the
manifest. Continuous `--auto-cross` is disabled by default because it can keep
activating gameplay or menus after startup. Explicitly enabling it marks the
manifest as `diagnostic_input`; it is discovery-only, never gameplay
acceptance. Use `kyty_playable_regression.py` with an external scene/action
checkpoint for acceptance. Its startup profile and checkpoint supply bounded
input sequences, each followed by clearing the pad overlay. Phase labels inferred
from frame rate are diagnostic observations, not scene classifications.

The guest log and screenshots are raw local evidence, not sanitized data. Keep
the output directory ignored and do not commit it; the manifest is the portable
review artifact.

`--baseline` makes the same run a regression gate: the manifest is still written
even when the gate fails, and the process exits non-zero when absolute material
health thresholds or relative white coverage, entropy, color diversity, or
directional-stripe checks fail.

`--lightbuf-probe 1` arms the bounded ordered lighting/compositor probe. Use
`--lightbuf-probe compositor` when only the final compositor RT is relevant.
`--rt-evidence-ps CRC32` narrows pipeline/attachment logs to one pixel shader,
which keeps long captures readable while preserving the full render-target
contract for that producer.
`--shader-probe-crc CRC32` dumps the decoded GCN IR for one VS or PS, allowing
the compositor's sampling and tone-map operations to be inspected without a
global shader dump.
`--videoout-evidence` logs the final VideoOut source image and swapchain
format/extent/layout immediately before and after the blit is recorded. It is
the last diagnostic seam before presentation, so it separates a bad compositor
output from a VideoOut or swapchain conversion problem without changing GPU
state.
This is diagnostic evidence, not a compatibility run. If the guest never reaches
`--min-frame`, the command now writes an `incomplete` manifest with the error,
log, and any screenshots already captured instead of discarding the session.

The capture runner arms `KYTY_NATIVE_CAPTURE_FIRST_PRESENT=1` and uses a
trigger file for later samples. The emulator waits for the presentation submit
fence, reads the source image through `UtilFillBuffer`, and emits a structured
`KYTY_NATIVE_CAPTURE` log line. This keeps the pixel source inside the
emulator; the external runner only coordinates timing and manifest collection.

For **realtime agent control** without Python or `xdotool`, start the emulator
with `KYTY_AGENT_ENDPOINT` and use the native `kyty_agent` CLI (`docs/agent-tools.md`).
That path talks to the emulator over a local endpoint for `status`,
`capture`, pad edges, and structured events while the guest is running.

`--key-at FRAME:KEY` still schedules a single press/release edge through
`xdotool` when that host exposes a controllable window. Prefer `kyty_agent pad`
when the agent socket is available. If host input control is unavailable, the
manifest records `input_error` instead of claiming that an input edge was
delivered.

Strict captures refuse `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE`. Use
`--allow-diagnostics` only for an exploratory run and do not use that manifest
as a compatibility result.

## Score and compare without rerunning

```bash
python3 scripts/kyty_capture.py score _scratch_playable/captures/frame-000.png --gate
python3 scripts/kyty_capture.py compare \
  --baseline _scratch_playable/captures/baseline.json \
  --current _scratch_playable/captures/capture.json
```

The score includes near-white coverage, saturation, quantized color diversity,
entropy and directional stripe detection. It reports `material_health_only`,
`material_healthy` and `scene_status: unknown`; neither OCR words nor green pixels
establish gameplay. The capture manifest also stores a conservative aggregate
over all samples. The compare command uses that aggregate and relative
thresholds to catch a collapse without pretending that a single screenshot
proves rendering correctness.

## Playable scene and action checkpoints

`kyty_playable_regression.py --scene-checkpoint <external-contract.json>` requires
an independently reviewed `kyty_scene_checkpoint_v1` contract. The contract pins
before/after reference PNGs by SHA-256 and names an anonymous fixture and scene
`kind`. Only `gameplay` can pass gameplay acceptance. Keep the contract and its
private reference images outside Git.

The contract declares normalized `scene_regions` with RGB error tolerances and
a `response` region with expected before/after appearance, pixel-change threshold
and minimum changed ratio. Its bounded `action` specifies a pad sequence,
consumption milestones, minimum additional presents and settling time. The exact
field contract lives in `scripts/kyty_scene_checkpoint.py`. Reference images are
never generated by the runner from its own candidate baseline.

Acceptance binds fresh emulator-native captures to the current child's capture
directory, hashes and presentation order around that delivered action. It requires
both scene matches, the specific expected response, observed guest consumption,
successful pad clearing and post-action progress. A menu, static image, generic
pixel change or tap counter alone cannot pass. A profile without a valid external
checkpoint fails `scene_checkpoint` and `action_response` honestly.

`no_runtime_failure` covers errors before and after presentation, input, capture
and scoring. `expected_exit` distinguishes an observed successful guest exit from
a documented coordinator stop; abnormal child returns remain failures even when
a timeout also occurred. `within_deadline` and `final_error_observed` are separate
requirements. Reports retain actual child return, timeout and stop provenance.
New material baselines are written only after all acceptance gates pass.

## Extending the system

Add new evidence in one of three places:

1. **Runtime probe**: emit a bounded, env-gated line such as `RT_EVIDENCE` or
   `FRAMEBUFFER_BEGIN`/`ORDERED_RT_CAPTURE` from the owning graphics seam.
2. **Manifest field**: add a sanitized scalar or list under `captures` or
   `config`; never write private guest paths, title IDs, or raw dumps to a
   tracked file.
3. **Metric/gate**: add a deterministic function in `kyty_capture.py` and a
   focused unit fixture for it before using it in a strict workflow.

This keeps one command, one artifact directory, and one machine-readable
contract while allowing later PM4 snapshots, resource graphs, RenderDoc links,
and input traces to be added without rewriting the runner.

For the **clear packing**, **sample→RenderTexture alias**, and **WaitRegMem /
Label fence** contracts that implementers must preserve when changing Gen5
color paths, see `docs/graphics-rt-clear-and-sample-alias.md`.

## Diagnosing diagonal primitive corruption

A transient half-screen triangle can originate before a color resolve. Capture
both the multisampled source attachment and the presented image around the same
frame, then inspect the primitive state of the draw that wrote the source. If
the source already contains the diagonal, changing resolve synchronization or
discarding the frame only hides the producer error.

For Gen5 indirect register arrays, preserve the register class declared by the
packet. `R_UC_REGS_INDIRECT` must try the UCONFIG dispatch table before the
compatibility paths for context or shader registers. The numeric register
spaces overlap; in particular, a UCONFIG offset can share a number with a
compute user-data register. Dispatching by number before class can consume a
valid `VGT_PRIMITIVE_TYPE` write as shader data and leave the topology at zero.
Zero is not a triangle-list fallback, so it must not be translated into a
drawable Vulkan topology.

To reproduce and verify this class of fault without changing guest behavior:

1. Capture a bounded consecutive VideoOut window containing the transition.
2. Capture the corresponding source render target when the diagonal appears.
3. Confirm that the indirect UCONFIG array contains a valid primitive value and
   that the draw uses that same value.
4. Repeat the same window after the fix and compare every frame, not only the
   final present.

Keep these captures in an ignored local directory. A correct result preserves
the intended full-frame transition and removes the diagonal at its producer;
it does not skip the draw, the resolve, or the present.
