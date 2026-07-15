# Kyty graphics captures

`scripts/kyty_capture.py` is the reproducible graphics-debugging entry point.
It is deliberately small enough to run today and has a stable JSON manifest so
new probes can be added without changing the workflow.

The design follows the useful part of emulator GPU devtools: capture a bounded
frame window, keep the command/configuration evidence beside the image, and
make the visual gate deterministic. It is not a substitute for RenderDoc; when
RenderDoc is available, use the same capture window and keep its frame capture
beside the Kyty manifest.

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
* `native_frames/*.bmp`: emulator-native readbacks of the emulated `VideoOut`
  source image, never screenshots of the desktop or window compositor;
* `*.bmp.json`: per-frame metadata with title/version when the loaded content
  exposes it, build revision, Vulkan format, source extent, present count, and
  capture milestone;
* `capture-*.json`: a sanitized manifest containing commit, host, capture
  configuration, artifact names, and deterministic image metrics.

The private guest root is used to start the process but is never written to the
manifest. `--auto-cross` is enabled by default for discovery and is explicitly
marked `diagnostic_input` in the manifest; it is not gameplay acceptance.
Disable it with `--no-auto-cross` when a real input sequence is available.

The guest log and screenshots are raw local evidence, not sanitized data. Keep
the output directory ignored and do not commit it; the manifest is the portable
review artifact.

`--baseline` makes the same run a regression gate: the manifest is still written
even when the gate fails, and the process exits non-zero when absolute gameplay
thresholds or relative white coverage, entropy, color diversity, scene, or
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
with `KYTY_AGENT_SOCK` and use the native `kyty_agent` CLI (`docs/agent-tools.md`).
That path talks to the emulator over a local Unix socket for `status`,
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
entropy, directional stripe detection, HUD green coverage, and a conservative
gameplay-scene check. The capture manifest also stores a conservative aggregate
over all samples. The compare command uses that aggregate and relative
thresholds to catch a collapse without pretending that a single screenshot
proves rendering correctness.

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
