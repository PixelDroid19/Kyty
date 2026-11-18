# Shader ALU contracts from an outside correctness review (2026-11-17)

An outside review listed six ALU lowerings that differ from the RDNA2 ISA
text. Each claim was checked against the ISA before any change; every fix but
the min/max zero rule has a fixture case in `EmulatorShaderArithmetic` that
fails on the old lowering.
Translator version 221 -> 228, one step per fix.

## Fixed

| Instruction | Old behavior | ISA contract | Fix |
| --- | --- | --- | --- |
| `S_MIN/S_MAX_I32/U32` | SCC = result != 0 | SCC = S0 < S1 (min) or S0 > S1 (max), signed or unsigned (12.1) | SCC from the strict compare |
| `V_CVT_RPI_I32_F32` (VOP3) | rejected as not implemented | `floor(S0 + 0.5)` (12.8) | VOP3 decode shares the VOP1 lowering |
| `V_MIN/V_MAX_F32`, `MIN3/MAX3/MED3` | signed-zero merge (OR/AND of bits) when the operands compared equal | -0/+0 rule only for zeros (12.7) | order operands by integer keys built from their bits |
| `V_SIN/V_COS_F32` | phase scaled by 2*pi, then reduced | functional examples: -FLT_MAX -> 0 and 1, sin(0.25) = 1, cos(0.25) = 0, sin(-0) = -0, +-inf -> `0xffc00000` (12.8) | reduce in turns, split into quadrant and remainder; exact quadrant values selected |
| `V_SUB_CO_CI_U32` (VOP2, VOP3B) | rejected as unknown | `S0 - S1 - VCC`, borrow-out `S1 + VCC > S0` (12.7) | new instruction type, shares the reverse-borrow lowering |
| `V_CVT_I32_F64`, `V_CVT_U32_F64` | raw SPIR-V conversion, undefined out of range | saturate, NaN -> 0 (12.8) | bounds compare before the conversion |

No f32 float-controls execution mode is emitted, so denormal handling in a
host float compare is driver-defined. Min/max therefore compare integer keys:
the bits, with the magnitude reversed for negative operands. The keys order
-0 before +0 and keep distinct subnormals apart on a flushing host; the guest
FP32 input denormal mode is applied to the operands first (see the
follow-up below).

## Verification

- Full `kyty_unit_test`: 2722 passed, 11 skipped (virtual memory and audio
  host checks), 0 failed; CTest gates
  `KytyArchitectureBoundaryIntegration.Repository`,
  `KytyArchiveDagIntegration.Repository` and `KytyHostShaders.CheckedInIncludes`
  passed.
- Strict 60 fps-limited runs on Intel Xe (Mesa ANV), private SPIR-V cache:
  - `PPSA17221 - Minecraft` (9ae052f04, before the quadrant select; cold cache): in the world at 140 s,
    50 fps, clean capture.
  - `PPSA21567 - Astro Bot`: 9ae052f04 with a cold cache, intro warp at
    120 s, clean capture; eabde50fa with a warm cache, intro warp at 110 s,
    33.5 fps, clean captures at 100 and 110 s. The route stays in the intro
    warp through 120 s, so Astro gameplay is unverified.
- `kyty_agent playable-regression` was not run.
- Not measured: whether these titles execute the changed instructions. The
  visual effect of the fixes on any title is unknown.

## Follow-up: rounded conversions, input denormals and F64 e64

Translator version 229 invalidates modules from earlier translations.
The focused suites passed 40 arithmetic tests and 14 translation-cache tests.
The new arithmetic cases validate complete pixel and compute SPIR-V modules.

- RPI and FLR now classify out-of-range operands by bits before conversion.
  The AMD gfx1036 probe returned signed saturation for tested NaNs, infinities
  and overflow. It ran 25 input pairs in 64 MODE settings twice and agreed
  across e32 and e64. This observation is from physical RDNA2 hardware.
- RPI uses round-to-nearest-even plus an exact positive half-tie correction.
  The former FP32 add rounded `0x3effffff` to 1 and `0x4b000001` to
  8388610. The hardware returned 0 and 8388609, respectively.
- MIN/MAX and their ternary forms apply the guest FP32 input denormal mode,
  preserve the sign of flushed zeros and order the remaining operands by
  bits. FLR yields -1 for a preserved negative subnormal and zero when
  that input is flushed.
- VOP3 opcodes `0x183` and `0x195` decode a two-dword source and share
  the existing F64-to-I32 and F64-to-U32 conversions.

## Open

- `V_SIN/V_COS_F32` periodicity for mid-range inputs (for example 1000.25
  turns) is assumed; the ISA states no input domain and its only large-input
  example is -FLT_MAX. The lowering's fps cost is unmeasured.
- `V_SIN/V_COS_F32` of a NaN input returns the host's NaN; the ISA examples
  cover only infinities.
