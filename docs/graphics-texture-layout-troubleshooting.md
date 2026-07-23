# Texture layout troubleshooting

Texture corruption that changes sprite scale, produces horizontal bands, or
makes atlas regions appear and disappear usually indicates a descriptor-layout
or tiling mismatch. Treat it as a data-contract problem before changing shader
coordinates or viewport state.

## Keep descriptor generations separate

Legacy and Gen5 image descriptors do not encode dimensions in the same bit
layout:

- legacy descriptors use the 14-bit width and height fields in descriptor word
  2;
- Gen5 descriptors use the RDNA2 layout: width is split between word 1 bits
  31:30 and word 2 bits 13:0, while height occupies word 2 bits 29:14.

The loader establishes the graphics generation from the executable ABI. The
texture preparation path must then call the matching accessor. Do not modify a
Gen5 accessor to make a legacy capture look correct: that can multiply Gen5
texture dimensions, inflate allocation ranges, and bind unrelated atlas data.

When diagnosing a dimension mismatch, record the generation, raw descriptor
words, decoded width and height, pitch, format, tile mode, and resource type.
Compare the decoded extent with the viewport or attachment only as supporting
evidence; the descriptor layout remains authoritative.

## Display-thin guest surfaces

A display-thin tiled surface that has no live GPU render-surface alias is
CPU-backed guest data. It must be handled as follows:

1. calculate the padded tiled allocation from width, height, pitch, format, and
   tile mode;
2. detile into a tightly packed linear staging buffer;
3. upload the linear extent to the Vulkan image;
4. retain normal alias tracking so a live GPU-owned surface is never uploaded
   as stale guest bytes.

Creating a render-texture object for such a surface without materializing its
guest data leaves the image undefined. Typical symptoms are smeared glyphs,
oversized logos, or blank atlas regions.

## Regression coverage

Any change to image descriptor decoding or display-thin detiling should verify
both generations:

- a Gen5 descriptor with non-zero split width bits;
- a legacy descriptor whose adjacent high bits are unrelated to dimensions;
- padded display-thin sizes at multiple resolutions;
- a known origin texel after detiling;
- a runtime capture with no horizontal-band classification.

This combination catches the common failure where one generation is repaired
by silently applying its layout to the other.
