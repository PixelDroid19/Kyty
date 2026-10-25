# Graphics table outputs

The `TileTextureInfo_*.inc` files are checked-in C++ initializers consumed by
`Graphics/Tile.cpp`. Each row lists a format, the level-0 width, height and
pitch, the level count and the per-level offsets, sizes and alignments.

`unit_test/src/emulator/UnitTestEmulatorGraphicsTables.cpp` includes every
table and re-derives each row from the layout rule of its tiling family:
thin-1d micro-tiled mips (`13_*`), linear k8_8_8_8UNorm (`0_56`, reversed mip
layout), Display_LinearAligned (`8_*`, 64-byte pitch floor with a 256-byte
level floor), Display_2dThin (`10_10_0`, 64/128-texel rows with a 32/64 KiB
level-0 base), Thin_2dThin (`14_10_0`, pitch-scaled level-0 alignment) and
Depth_2dThin_256 (`2_4_7`, 32/128-texel rows). Rows with zero width, height
and pitch are placeholders and are skipped.

Run the check from a build directory:

```sh
./kyty_unit_test --gtest_filter='EmulatorGraphicsTables*'
```

To add a row, write its dimensions and the values the family rule gives; the
test reports the row index and level of any value that disagrees. A row that
needs a layout outside the rules is a new family: add its rule to the test in
the same change, backed by evidence for that layout.
