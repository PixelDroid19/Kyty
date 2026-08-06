# Blasphemous 2: renderer bring-up notes

This note records the two renderer contracts corrected while bringing the
opening sequence up to the first interactive screens. It is intentionally
limited to integration evidence; a startup window or a single capture is not
treated as proof of gameplay compatibility.

## Color-control modes

The `CB_COLOR_CONTROL.MODE` field is three bits wide. The renderer keeps mode
`3` for the fixed-function color resolve path. Every other encoded value is
accepted as an ordinary raster draw, with the guest color mask still derived
from `CB_TARGET_MASK` and `CB_SHADER_MASK`.

Previously the hardware-state validator accepted only modes `0`, `1`, and `3`.
When the guest programmed mode `2` (or another valid encoded operation), strict
bring-up reported `not_implemented` before the draw could be submitted. That
left the current target stale and produced the black/flat-color transition
seen during the opening sequence.

## Thread-trace instruction

`s_ttracedata` writes only to the GPU thread-trace side channel. It does not
write a scalar/vector register, memory, or control-flow state. The shader parser
now lowers it to the existing no-op instruction path instead of rejecting the
entire shader stage.

## Persistent shader-module cache invalidation

The dotted glyphs on the license screen were caused by a stale persistent
SPIR-V module, not by the R8 font texture, its detile, component mapping,
sampler, or the host GPU. The same draw was clean with an empty SPIR-V cache
and reproduced the dotted glyphs when an older persistent module was reused.

`ShaderTranslationCache` includes the translator version in the key for every
persisted module. If translator code changes without a version change, an old
`.spvmod` can satisfy the lookup and bypass the current translation path. The
old module was the source of the invalid SDF glyph output; changing the texture
or sampler contract would only mask the cache problem and could regress other
text paths.

`kShaderTranslatorVersion` is now `9`. Existing cache entries remain on disk,
but no longer match the key and are recompiled automatically. No title-specific
texture or sampler override is involved.

The bounded A/B evidence was:

- the previous translator version with the existing persistent cache produced
  dotted regular glyphs;
- the same binary with an empty cache produced clean glyphs;
- the rebuilt translator version `9` with the existing cache produced clean
  glyphs again.

Validation therefore uses the rebuilt binary and reaches the license screen;
manually deleting the cache is not required after the version bump.

## Bounded integration reproduction

Use a legally obtained guest fixture and keep all runtime artifacts outside the
repository:

```sh
export KYTY_GUEST_ROOT=/absolute/path/to/guest
export KYTY_AGENT_ENDPOINT=/tmp/kyty-b2.sock
export KYTY_NATIVE_CAPTURE_DIR=/tmp/kyty-b2-captures
export KYTY_PAD_SCRIPT="$PWD/scripts/input/reach_first_gameplay.pad"
export KYTY_PRINTF_DIRECTION=Silent

_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

In a second terminal, use the native runtime interface:

```sh
_build_linux/agent/kyty_agent --endpoint "$KYTY_AGENT_ENDPOINT" wait-ready --timeout-ms 30000
_build_linux/agent/kyty_agent --endpoint "$KYTY_AGENT_ENDPOINT" wait-phase interactive --timeout-ms 45000
_build_linux/agent/kyty_agent --endpoint "$KYTY_AGENT_ENDPOINT" wait-present --delta 180 --timeout-ms 45000
_build_linux/agent/kyty_agent --endpoint "$KYTY_AGENT_ENDPOINT" capture --timeout-ms 10000
_build_linux/agent/kyty_agent --endpoint "$KYTY_AGENT_ENDPOINT" last-error
```

The corrected path must keep presenting, produce a non-black native capture,
and report no `not_implemented` or `device_lost` event. The exploratory input
route continues through the title and tutorial; complete gameplay evidence
requires the later route window and a changing captured scene.
