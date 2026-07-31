# Shader SPIR-V Modularization Design

## Goal

Split `ShaderSpirv.cpp` into focused C++ translation units without changing
the public API, emitted SPIR-V, error policy, or supported instruction set.

## Architecture

`ShaderSpirv.h` remains the public facade. A private
`ShaderSpirvInternal.h` owns generator state and shared contracts. The public
facade constructs the internal generator, which assembles a module and delegates
instruction translation to a central dispatch table. Emitters are grouped by ISA
domain and depend only on the internal state contract and shared operand helpers.

The source files are:

- `ShaderSpirv.cpp`: public facade and public hash integration.
- `ShaderSpirvInternal.h`: private generator state, shared types, and contracts.
- `ShaderSpirvEmitters.h`: private declarations for instruction emitters.
- `ShaderSpirvTemplates.h/.cpp`: reusable SPIR-V snippets and embedded shaders.
- `ShaderSpirvGenerator.cpp`: module sections, constants, variables, and orchestration.
- `ShaderSpirvOperands.cpp`: operand classification and load/materialization helpers.
- `ShaderSpirvControlFlow.cpp`: CFG analysis and branch emitters.
- `ShaderSpirvBuffer.cpp`: buffer, typed-buffer, scalar-buffer, and LDS emitters.
- `ShaderSpirvImage.cpp`: sampled/storage image emitters.
- `ShaderSpirvScalar.cpp`: scalar ALU and scalar state emitters.
- `ShaderSpirvVector.cpp`: vector ALU, interpolation, exports, fetch, and debug emitters.
- `ShaderSpirvDispatch.cpp`: instruction-to-emitter registry.

Dependencies flow from facade to generator to dispatch to emitter families.
Templates and operand helpers are leaf services. Domain-specific helpers remain
private to their translation unit.

## Behavioral Constraints

- Keep all three public functions in `ShaderSpirv.h` source-compatible.
- Preserve generated text byte-for-byte for the same inputs.
- Preserve existing fatal and diagnostic behavior.
- Preserve every current local change in `ShaderSpirv.cpp`.
- Do not fix the pre-existing arrayed-image unit-test failure in this refactor.
- Do not add title-specific behavior or compatibility claims.

## Verification

Reconfigure through the existing CMake glob, build `fc_script` and `unit_test`,
run focused shader-generation tests, and compare failures with the recorded
pre-refactor baseline. Review the complete diff for accidental behavior edits,
private paths, generated files, and unrelated changes.
