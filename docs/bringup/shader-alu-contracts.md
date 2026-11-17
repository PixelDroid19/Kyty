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
| `V_MIN/V_MAX_F32`, `MIN3/MAX3/MED3` | signed-zero merge (OR/AND of bits) when the operands compared equal | -0/+0 rule only for zeros (12.7) | merge only when both operands are zero by bits |
| `V_SIN/V_COS_F32` | phase scaled by 2*pi, then reduced | functional examples: -FLT_MAX -> 0 and 1, sin(0.25) = 1, cos(0.25) = 0, sin(-0) = -0, +-inf -> `0xffc00000` (12.8) | reduce in turns, split into quadrant and remainder; exact quadrant values selected |
| `V_SUB_CO_CI_U32` (VOP2, VOP3B) | rejected as unknown | `S0 - S1 - VCC`, borrow-out `S1 + VCC > S0` (12.7) | new instruction type, shares the reverse-borrow lowering |
| `V_CVT_I32_F64`, `V_CVT_U32_F64` | raw SPIR-V conversion, undefined out of range | saturate, NaN -> 0 (12.8) | bounds compare before the conversion |

The min/max merge matters because no f32 float-controls execution mode is
emitted: denormal handling in a compare is host-defined, and a flushing
compare treated two distinct subnormals as equal. The fixture compares with
host IEEE floats, so it cannot reproduce the flush; its subnormal cases pin
the contract, not the failure.

## Verification

- Full `kyty_unit_test`: 2722 passed, 11 skipped (virtual memory and audio
  host checks), 0 failed; CTest gates
  `KytyArchitectureBoundaryIntegration.Repository`,
  `KytyArchiveDagIntegration.Repository` and `KytyHostShaders.CheckedInIncludes`
  passed.
- Strict 60 fps-limited runs on Intel Xe (Mesa ANV), private SPIR-V cache:
  - `PPSA17221 - Minecraft` (9ae052f04, cold cache): in the world at 140 s,
    50 fps, clean capture.
  - `PPSA21567 - Astro Bot` (9ae052f04 cold, then the quadrant select warm):
    intro warp at 120 s in both runs, clean captures. The route does not reach
    gameplay within 120 s, so Astro gameplay is unverified.
- `kyty_agent playable-regression` was not run.
- Not measured: whether these titles execute the changed instructions. The
  visual effect of the fixes on any title is unknown.

## Open

- `V_CVT_RPI_I32_F32` and `V_CVT_FLR_I32_F32` still pass NaN, infinities and
  out-of-range values to an undefined SPIR-V conversion. The ISA text gives
  only `(int)floor(...)` with no saturation rule, so no result was chosen.
  Direction: confirm hardware output for those inputs, then share the FP32
  saturation.
- `V_CVT_I32_F64` / `V_CVT_U32_F64` VOP3 forms (`0x183`, `0x195`) are not
  decoded.
- `V_SIN/V_COS_F32` periodicity for mid-range inputs (for example 1000.25
  turns) is assumed; the ISA states no input domain and its only large-input
  example is -FLT_MAX. The lowering's fps cost is unmeasured.
- `V_MIN/V_MAX_F32` do not apply the FP_DENORM input mode; subnormals follow
  the host driver.
- `V_SIN/V_COS_F32` of a NaN input returns the host's NaN; the ISA examples
  cover only infinities.
