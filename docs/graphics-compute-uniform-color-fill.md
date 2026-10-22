# Ordered compute fills of color-image aliases

A storage buffer and an optimal-tiled Vulkan color image can represent the
same guest allocation. Executing the buffer store alone does not update the
image. Rebinding a sampled image must still preserve it (see
[graphics-color-attachment-preservation.md](graphics-color-attachment-preservation.md));
clearing on that rebind hid missing write propagation and erased unrelated
scene contents.

The renderer propagates one narrow, proven write contract. The decoded compute
program must contain only a linear invocation index, four plain scalar-to-vector
copies, one indexed four-component typed store and program end (with harmless
wait/prefetch padding). The invocation-ID register must survive until index
generation. The live binding must be one exact writable UINT32-vec4 descriptor,
stride 16, with direct scalar values, no swizzling or extra addressing, and an
exact full-range dispatch without partial thread groups. Fill values cannot
depend on the workgroup-ID register. Instruction addresses and checksums do not
select this behavior.

The original compute dispatch always executes. Afterwards:

- Every exact live color-image alias of the filled range is prevalidated
  before any image clear: render targets and sampled texture copies whose
  range equals the buffer range. Partial (containing or crossing) aliases and
  other object types fail closed. Each alias must be single-sample, cover the
  complete range, and use a format whose texel repeats exactly within each
  16-byte word quad: RGBA32F (the quad is one texel, red at the lowest
  address), RGBA16F (two texels per quad) or RGBA8/BGRA8 UNORM/sRGB and R32F
  (four texels per quad). Because every texel of every level and layer
  holds the same value, the clear is independent of tiling. sRGB images are
  cleared with the decoded linear value so the stored bytes match the guest
  write. The clear is ordered in the same recording and the image's defined
  layout is restored.
- If no image alias exists, at most 64 pending events retain the storage
  incarnation, content sequence, dirty-page read observation, submission
  identity and four fill words. First exact attachment materialization consumes
  the event once, before framebuffer creation.
- Consumption requires the same virtual queue and a nondecreasing submission
  sequence. Both immediate and deferred image effects require the recording's
  physical queue to be the graphics queue. No cross-queue dependency or
  exclusive-image ownership transfer is invented.
- CPU writes, storage/image writes, color writes and depth/stencil writes revoke
  affected candidates. Resource reincarnation, partial or ambiguous ranges,
  untracked CPU memory and unsupported alias representations fail closed.

Evidence: a 2D title resets its lighting accumulation target (RGBA16F) and
several RGBA8 targets every frame with this kernel (fill values `(0,0,0,1)`,
`0`, `0xffffffff` and `0xff008080`). With load-on-rebind and no propagation the
targets accumulated prior frames (saturated yellow light, then an olive haze);
with propagation the frames match the earlier clear-on-rebind captures. The
robot platformer resets a 1x1 RGBA32F exposure-state texture with this kernel
(`(1, -1, 0.5, 0.18)`); without the RGBA32F alias the texture stayed zero, the
exposure pass wrote zero and the final composite multiplied the whole 3D scene
by it, leaving only the UI visible.

This is not general buffer/image coherence or support for other clear kernels,
compressed formats, partial fills or arbitrary alias topologies.

A second, constant form is also recognized: the linear invocation index, one
`v_mov_b32` of an inline or literal integer constant, one
`buffer_store_format_x` of that register at the index (idxen, no offset) and
program end. The live binding must be one 32-bit UINT descriptor, stride 4,
whose X lands in X, with a record count that is a whole number of 16-byte
groups and equal to the dispatched invocation count. The fill words are the
constant repeated.

A third, loaded form reads the value instead: the linear invocation index, one
`s_buffer_load_dword` at offset 0 of a second, read-only descriptor, one
`v_mov_b32` of that scalar and the same store. The source descriptor must be
exact, read-only and distinct from the destination, and the workgroup SGPR
must lie outside it. The fill word is the source's first word, read once from
guest memory when the command processor records the dispatch.

## DCC metadata clears

The same kernels also fill color DCC metadata. Gen5 color DCC keeps one key
byte per 256-byte compressed block; a key that names a clear state makes every
texel of its block read as that state. A fill whose bytes all hold one of the
clear keys `0x00` (all channels 0), `0x40` (color 0, alpha 1), `0x80` (color 1,
alpha 0), `0xc0` (all channels 1) or `0x20` (the target's
`CB_COLOR_CLEAR_WORD0/1`) is held as a pending clear of that metadata range,
with the same storage identity, submission and dirty-page rules as above. Any
other fill of the range, including the uncompressed key `0xff`, or any
writable storage-buffer bind over it, drops the pending clear.

The first access to the color surface consumes the clear once, before the
access: a render target bound with DCC (`CB_COLOR_DCC_BASE`) or a shader image,
read or written, whose T# enables compression and names that metadata base.
The clear becomes the contents of every exact color-image alias of the
surface. It is applied only when the fill starts at the metadata base and holds
at least one key per 256 bytes of texels, and only to single-level,
single-slice, single-sample surfaces. Keys `0x40` and `0x80` are decoded only
for four-channel formats with alpha in the most significant channel
(`COMP_SWAP` STD or ALT for a target, `ALPHA_IS_ON_MSB` for a T#); the register
key is decoded only for a render target whose image format is the target
format. A shader image access that cannot apply the clear still drops it, so a
clear never lands on contents written after it.

Evidence: the robot platformer resets its UI layer metadata with `0x40` every
frame and draws the UI over it. Without the clear the UI accumulated earlier
frames (stale publisher text, smeared logo) and, because the composite uses the
UI alpha as scene transmittance, hid the 3D scene. Its lighting target is reset
with `0x00` and then written by four lighting compute passes before its first
render-target bind; consuming the clear at that later bind erased the lighting,
so the clear is consumed at the first shader image access instead.

Evidence for the constant form: the role-playing title clears the DCC
metadata of its render targets with `v_mov_b32 v1, 0` and
`buffer_store_format_x` at the thread index (16,384 records over a
1024x1024, 16-byte-texel target: one key per 256 bytes). Unrecognized, none
of those clears were published; with the form recognized, the same run
publishes them and the render-target and shader-image binds consume them.

Evidence for the loaded form: the same title clears the DCC metadata of its
1920x2200 depth-of-field target with a kernel that loads the fill value from a
one-record buffer. With the form recognized, that metadata range has an owner
and the clear is published (`tracked=1 published=1`).

Not modelled: the pipe-dependent padding of the metadata plane (the coverage
check uses the unpadded key count), mip chains, arrays, MSAA, REV component
swaps and register-key clears read through the texture unit.

Focused tests: `EmulatorComputeColorFill.*` (program recognition and rejection,
event consume-once, identity and queue mismatch, bounded eviction and texel
decoding). The GPU integration cases of the September working copy
(`KytyGraphicsDiagnosticsIntegration.ComputeColorFill*`) have not been ported.
