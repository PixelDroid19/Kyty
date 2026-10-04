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
  16-byte word quad: RGBA16F (two texels per quad) or RGBA8/BGRA8 UNORM/sRGB
  and R32F (four texels per quad). Because every texel of every level and layer
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
with propagation the frames match the earlier clear-on-rebind captures.

This is not general buffer/image coherence or support for other clear kernels,
compressed formats, partial fills or arbitrary alias topologies.

Focused tests: `EmulatorComputeColorFill.*` (program recognition and rejection,
event consume-once, identity and queue mismatch, bounded eviction and texel
decoding). The GPU integration cases of the September working copy
(`KytyGraphicsDiagnosticsIntegration.ComputeColorFill*`) have not been ported.
