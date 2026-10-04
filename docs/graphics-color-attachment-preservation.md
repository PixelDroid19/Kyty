# Preserve color attachments across sampled use

A render target remains populated after a shader samples it. Rebinding that
target for a later pass must preserve its contents unless an actual clear or
invalidation operation requires otherwise. Shader-read layout and clear-value
registers do not establish such an operation.

A captured frame exposed the failure sequence: opaque scene rendering, a
sampled scene copy, then a transparent pass on the original target. The
renderer's sampled-layout heuristic selected `CLEAR` for that last pass and
erased the already-rendered scenery. Pixel history identified the clear rather
than shader arithmetic as the destructive operation.

`ResolveColorAttachmentLoadOps` now selects `LOAD` for defined attachments.
The existing first-use clear, clear packing, framebuffer cache identity and
pre-pass layout transition are retained. The focused regressions cover float
scene targets and RGBA targets, including nonzero registered clear values.

The development renderer's bounded menu and opening-scene runs retain visible
scenery after the correction. Those runs include other local work and do not
establish full playability or performance for this standalone change.

Intentional color-metadata clears still need an evidenced, ordered, one-shot
event tied to the target identity. Sampling must not substitute for that model;
cross-title temporal accumulation remains a validation risk.
