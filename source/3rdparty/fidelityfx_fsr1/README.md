# AMD FidelityFX Super Resolution 1.0

`ffx_a.h` and `ffx_fsr1.h` are the unmodified headers from AMD's
FidelityFX-FSR repository (directory `ffx-fsr`, commit
`a21ffb8f6c13233ba336352bdff293894c706575`), released under the MIT license
in `LICENSE.txt`.

They are not compiled into the C++ build. `scripts/build_presentation_fsr_shaders.sh`
compiles the presentation compute shaders in
`source/emulator/src/Graphics/Shaders` against them and writes the embedded
SPIR-V to `source/emulator/src/Graphics/PresentationFsrShaders.inc`.
