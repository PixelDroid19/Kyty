# Blasphemous 2: renderer bring-up notes

This note records the general renderer and HLE contracts corrected while
bringing the strict path through the opening sequence and into the first
playable room. A startup window, advancing presents, or a single non-flat
capture is not treated as proof of gameplay compatibility.

## Post-logo black-screen root cause

The guest continued drawing and presenting after the logos, but the native
capture was flat black. The resource trace showed Gen5 format `56` reaching a
storage-image descriptor. Sampled format `56` already resolved to
`VK_FORMAT_R8G8B8A8_UNORM` (or SRGB with degamma), while its storage lookup
returned `VK_FORMAT_UNDEFINED`. The centralized table now resolves storage
format `56` to `VK_FORMAT_R8G8B8A8_UNORM`; the public support query and format
result are covered by `EmulatorGraphicsState.Gen5SampledRgba8FormatUsesUnormByDefault`.

The same transition also requires the two-argument, pointer-sized
`sceAudioOut2UserCreate(uint32_t, uintptr_t*)` ABI. Reading a nonexistent third
argument or writing a 32-bit handle caused the audio user initialization to
target the wrong guest address. A compile-time function-type assertion and
mapped-output tests protect that ABI.

### AudioOut2 context lifecycle regression

A later hardening pass reintroduced the same visible symptom by rejecting
`ContextResetParam`, `ContextQueryMemory`, and `ContextCreate`
unconditionally. A debugger trace established the complete call chain:

- `ResetParam` receives a writable `0x40`-byte block and must return success
  without overwriting it before the caller fills the fields;
- the supported profile queries a `0x10000`-byte workspace; and
- `ContextCreate` receives that mapped workspace and publishes a 32-bit
  context handle.

The failing guest branch tests the negative return from `ResetParam` and skips
both later calls, leaving the post-logo frame black. The HLE now validates the
full guest ranges, accepts only the measured profile and exact workspace size,
copies outputs through the guest-memory boundary, and keeps unknown layouts
strict. The focused regression exercises Reset, QueryMemory, Create, and
Destroy as one lifecycle.

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
and report no `not_implemented` or `device_lost` event. The current strict
integration run reached the first playable room, produced a healthy native
capture, consumed two explicit 180-presentation directional holds, and showed
the player changing position. The input route is diagnostic evidence of the
runtime/control frontier; longer compatibility and stability coverage remains
separate from this bounded bring-up result.
