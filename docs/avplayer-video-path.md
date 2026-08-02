# AvPlayer video path

Kyty presents decoded video through guest-owned linear NV12 buffers. The host
decoder, guest ABI, GPU dirty tracker, and Vulkan texture cache must agree on
the ownership and lifetime of every frame.

## Correct frame flow

1. `sceAvPlayerAddSource*` resolves the guest URI and opens the host media
   backend. A source without a supported video stream fails initialization;
   Kyty does not substitute generated color frames.
2. The decoder produces a bounded FIFO of NV12 frames. A full video queue
   applies backpressure to the decoder instead of removing the oldest frame.
3. `sceAvPlayerGetVideoDataEx` consumes the earliest available decoded frame.
   It selects an output buffer only after a frame exists, copies the complete
   NV12 payload, and publishes the decoder timestamp.
4. The host copy notifies GPU memory tracking before the guest buffer is reused.
   This invalidates any cached Vulkan representation of the previous contents.
5. The guest renders the new buffer through the normal texture path.

Advancing the output-buffer ring before a decoded frame is available can expose
an old buffer again. Dropping the front of the decoder queue causes visible
timestamp gaps. Omitting the host-write notification lets Vulkan reuse stale
copies of rotating guest buffers. These faults appear as alternating solid
colors, repeated frames, or a short backward jump during playback.

## Strict integration reproduction

Use a legally obtained guest fixture that contains a startup video and a build
with the media backend enabled:

```sh
export KYTY_GUEST_ROOT=/absolute/path/to/guest
export KYTY_AGENT_ENDPOINT=/tmp/kyty-avplayer.sock
export KYTY_NATIVE_CAPTURE_DIR=/tmp/kyty-avplayer-captures
export KYTY_DUMP_AVPLAYER=1

_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

In another terminal:

```sh
_build_linux/agent/kyty_agent wait-ready --timeout-ms 30000
_build_linux/agent/kyty_agent wait-phase interactive --timeout-ms 120000
_build_linux/agent/kyty_agent watch --seconds 20
_build_linux/agent/kyty_agent capture --timeout-ms 10000
_build_linux/agent/kyty_agent last-error
```

Do not enable bring-up or permissive graphics features for acceptance. For a
24 fps source, the diagnostic timestamps should begin at the first decoded
frame and remain strictly increasing, typically `0, 42, 84, 126, ...` after
millisecond conversion. Captures taken at different presentation counts must
contain actual video frames rather than a synthetic solid-color surface.

## Playback timing contract

`sceVideoOutGetVblankStatus` reports display time, not the number of times the
guest queried the function and not the number of host presents. Its count is
derived from a monotonic 59.94 Hz clock and starts at one for an open output.
Polling the status repeatedly inside one refresh interval must return the same
count; after one refresh period it must advance independently of rendering
speed.

Keep this query clock separate from the internal vblank event counter. Event
delivery and `sceVideoOutWaitVblank` retain their own synchronization state;
overwriting that state from a status query mixes two clocks and can create
duplicate or missing events.

When a transition flashes a clear color before video, measure the source-open
path before changing frame conversion. Record the duration of media open,
stream discovery, codec creation, guest texture allocation, READY callback,
and first decoded frame. If those stages finish in milliseconds while the
runtime records multi-second frames with no video upload, the pause precedes
AvPlayer and belongs to scene loading or another guest subsystem. Hiding those
presents or substituting a frame is not a media fix.

For a visual ordering check, capture at least four bounded samples during the
same playback interval. Compare their presentation numbers and scene motion in
order. This distinguishes a real stale-buffer or timestamp regression from an
intentional bright frame in the source artwork.

## Scalar buffers sourced from executable images

Some shaders read small constant tables located in loader-owned executable
memory with `S_BUFFER_LOAD`. For a statically known scalar offset, Kyty derives
the exact byte span from the consuming instruction. It does not multiply the
descriptor stride by its record count to decide how much host memory to copy.

The renderer accepts this path only when all of the following are proven:

- the descriptor has only scalar raw consumers;
- every scalar offset is static and the shader contains no unknown or indirect
  use of the descriptor;
- the complete requested range is a readable guest allocation;
- the binding is read-only and the transient copy is at most 4 KiB.

Dynamic, mixed, typed, writable, unreadable, or oversized ranges stay on the
normal tracked GPU-memory path and fail visibly when their backing is missing.
