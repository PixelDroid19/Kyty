#include "Kyty/UnitTest.h"

#include <cstdint>
#include <iterator>

// Re-derives every row of the checked-in Graphics/Tables/TileTextureInfo_*.inc
// lookups from the layout rules of its tiling family, so a table edit that
// breaks the family's layout cannot pass silently.

UT_BEGIN(EmulatorGraphicsTables);

namespace {

// Same layout as the private structs in Graphics/Tile.cpp that the tables initialize.
struct TextureInfoSize
{
	uint32_t total_size    = 0;
	uint32_t total_align   = 0;
	uint32_t mipmap_offset = 0;
	uint32_t mipmap_size   = 0;
};

struct TextureInfoSize2
{
	uint32_t mipmap_offset = 0;
	uint32_t mipmap_size   = 0;
};

struct TextureInfoPadded
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

struct TextureInfo
{
	uint32_t          dfmt   = 0;
	uint32_t          nfmt   = 0;
	uint32_t          width  = 0;
	uint32_t          height = 0;
	uint32_t          pitch  = 0;
	uint32_t          levels = 0;
	uint32_t          tile   = 0;
	bool              neo    = false;
	TextureInfoSize   size[16];
	TextureInfoPadded padded[16];
};

struct TextureInfo2
{
	uint32_t          fmt         = 0;
	uint32_t          width       = 0;
	uint32_t          height      = 0;
	uint32_t          pitch       = 0;
	uint32_t          levels      = 0;
	uint32_t          tile        = 0;
	uint32_t          total_size  = 0;
	uint32_t          total_align = 0;
	TextureInfoSize2  size[16];
	TextureInfoPadded padded[16];
};

#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_0_56.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_10_10_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_10_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_10_9.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_35_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_36_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_37_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_13_37_9.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_14_10_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_2_4_7.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_8_10_0.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_8_10_9.inc"
#include "../../../emulator/src/Graphics/Tables/TileTextureInfo_8_1_0.inc"

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
	return ((value + alignment - 1u) / alignment) * alignment;
}

uint32_t RoundUpPow2(uint32_t value)
{
	uint32_t result = 1;
	while (result < value)
	{
		result *= 2u;
	}
	return result;
}

uint32_t Max(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

// Tables end with a zero placeholder row when a section has no entries.
bool IsPlaceholder(const TextureInfo& info)
{
	return info.width == 0 && info.height == 0 && info.pitch == 0;
}

struct MicroTiledFormat
{
	uint32_t dfmt;
	uint32_t nfmt;
	uint32_t block_width;
	uint32_t block_height;
	uint32_t bytes_per_element;
};

constexpr MicroTiledFormat kMicroTiledFormats[] = {
    {1, 0, 1, 1, 1},  {3, 0, 1, 1, 2},   {10, 0, 1, 1, 4},  {10, 9, 1, 1, 4},
    {35, 0, 4, 4, 8}, {36, 0, 4, 4, 16}, {37, 0, 4, 4, 16}, {37, 9, 4, 4, 16},
};

const MicroTiledFormat* FindMicroTiledFormat(uint32_t dfmt, uint32_t nfmt)
{
	for (const auto& format: kMicroTiledFormats)
	{
		if (format.dfmt == dfmt && format.nfmt == nfmt)
		{
			return &format;
		}
	}
	return nullptr;
}

// Thin-1d micro-tiled families (tile 13). Power-of-two sections halve each mip
// directly; fixed sections round each halved dimension up to a power of two and,
// for multi-level surfaces, store a level size that rounds the padded height up
// to a power of two (offsets advance by that size).
TextureInfo ExpectMicroTiled(const TextureInfo& row, bool pow2_section)
{
	TextureInfo expected = row;
	const auto* format   = FindMicroTiledFormat(row.dfmt, row.nfmt);
	EXPECT_NE(format, nullptr);
	if (format == nullptr)
	{
		return expected;
	}
	uint32_t width  = row.width;
	uint32_t height = row.height;
	uint32_t pitch  = row.pitch != 0 ? row.pitch : row.width;
	uint32_t offset = 0;
	for (uint32_t level = 0; level < row.levels; level++)
	{
		if (level > 0)
		{
			width  = pow2_section ? Max(width / 2u, 1u) : RoundUpPow2(width / 2u);
			height = pow2_section ? Max(height / 2u, 1u) : RoundUpPow2(height / 2u);
			pitch  = pow2_section ? Max(pitch / 2u, 1u) : RoundUpPow2(pitch / 2u);
		}
		const uint32_t element_width  = Max((width + format->block_width - 1u) / format->block_width, 1u);
		const uint32_t element_height = Max((height + format->block_height - 1u) / format->block_height, 1u);
		const uint32_t element_pitch  = Max((pitch + format->block_width - 1u) / format->block_width, element_width);
		const uint32_t padded_width   = AlignUp(element_pitch, 8u);
		const uint32_t padded_height  = AlignUp(element_height, 8u);
		const uint32_t raw_size       = padded_width * padded_height * format->bytes_per_element;
		const uint32_t size =
		    (pow2_section || row.levels == 1) ? raw_size : padded_width * RoundUpPow2(padded_height) * format->bytes_per_element;
		expected.size[level]   = {offset + raw_size, 256u, offset, size};
		expected.padded[level] = {padded_width, padded_height};
		offset += size;
	}
	return expected;
}

TextureInfo ExpectFamily(const TextureInfo& row, const uint32_t* sizes, uint32_t alignment)
{
	TextureInfo expected = row;
	uint32_t    offset   = 0;
	for (uint32_t level = 0; level < row.levels; level++)
	{
		expected.size[level]   = {offset + sizes[level], alignment, offset, sizes[level]};
		expected.padded[level] = {};
		offset += sizes[level];
	}
	return expected;
}

// Display_LinearAligned (tile 8): the pitch halves down to a 64-byte floor, the
// height halves to 1, and every level reserves at least 256 bytes.
TextureInfo ExpectDisplayLinear(const TextureInfo& row)
{
	const uint32_t bytes_per_element = row.dfmt == 1 ? 1u : 4u;
	uint32_t       sizes[16] {};
	for (uint32_t level = 0; level < row.levels; level++)
	{
		const uint32_t pitch  = Max(row.pitch >> level, 64u / bytes_per_element);
		const uint32_t height = Max(row.height >> level, 1u);
		sizes[level]          = Max(pitch * height * bytes_per_element, 256u);
	}
	return ExpectFamily(row, sizes, 256u);
}

// Display_2dThin (tile 10): rows pad to 64 (base) or 128 (neo) texels and level 0
// aligns to the 32 KiB / 64 KiB tile base.
TextureInfo ExpectDisplayThin(const TextureInfo& row)
{
	const uint32_t tile_align = row.neo ? 65536u : 32768u;
	uint32_t       sizes[16] {};
	for (uint32_t level = 0; level < row.levels; level++)
	{
		const uint32_t width    = Max(row.width >> level, 1u);
		const uint32_t height   = Max(row.height >> level, 1u);
		const uint32_t pitch    = Max(row.pitch >> level, width);
		const uint32_t raw_size = AlignUp(pitch, 8u) * AlignUp(height, row.neo ? 128u : 64u) * 4u;
		sizes[level]            = level == 0 ? AlignUp(raw_size, tile_align) : raw_size;
	}
	return ExpectFamily(row, sizes, tile_align);
}

// Thin_2dThin (tile 14): level 0 aligns to a base that scales with the surface
// pitch; the reported alignment is the 32 KiB / 64 KiB grid.
TextureInfo ExpectThin2d(const TextureInfo& row)
{
	const uint32_t size_align = row.pitch * (row.neo ? 512u : 256u);
	uint32_t       sizes[16] {};
	for (uint32_t level = 0; level < row.levels; level++)
	{
		const uint32_t width    = Max(row.width >> level, 1u);
		const uint32_t height   = Max(row.height >> level, 1u);
		const uint32_t pitch    = Max(row.pitch >> level, width);
		const uint32_t raw_size = AlignUp(pitch, 8u) * AlignUp(height, 8u) * 4u;
		sizes[level]            = level == 0 ? AlignUp(raw_size, size_align) : raw_size;
	}
	return ExpectFamily(row, sizes, row.neo ? 65536u : 32768u);
}

// Depth_2dThin_256 (tile 2): the height pads to 32 (base) or 128 (neo) texels.
TextureInfo ExpectDepthThin(const TextureInfo& row)
{
	uint32_t sizes[16] {};
	for (uint32_t level = 0; level < row.levels; level++)
	{
		sizes[level] = AlignUp(Max(row.pitch, row.width), 8u) * AlignUp(Max(row.height, 1u), row.neo ? 128u : 32u) * 4u;
	}
	return ExpectFamily(row, sizes, row.neo ? 65536u : 32768u);
}

// Linear k8_8_8_8UNorm (format 56): mip pitches halve to a floor of 64 and
// heights to 1; the smallest mip is stored first, so offsets are measured from
// the end of the allocation.
TextureInfo2 ExpectLinear(const TextureInfo2& row)
{
	TextureInfo2 expected = row;
	uint32_t     sizes[16] {};
	uint32_t     total = 0;
	for (uint32_t level = 0; level < row.levels; level++)
	{
		const uint32_t pitch   = Max(row.pitch >> level, 64u);
		const uint32_t height  = Max(row.height >> level, 1u);
		sizes[level]           = pitch * height * 4u;
		expected.padded[level] = {pitch, height};
		total += sizes[level];
	}
	uint32_t offset = 0;
	for (uint32_t level = row.levels; level-- > 0;)
	{
		expected.size[level] = {offset, sizes[level]};
		offset += sizes[level];
	}
	expected.total_size  = total;
	expected.total_align = 256u;
	return expected;
}

void ExpectSameRow(const TextureInfo& actual, const TextureInfo& expected, int index)
{
	ASSERT_LE(actual.levels, 16u);
	for (uint32_t level = 0; level < 16; level++)
	{
		EXPECT_EQ(actual.size[level].total_size, expected.size[level].total_size) << "row " << index << " level " << level;
		EXPECT_EQ(actual.size[level].total_align, expected.size[level].total_align) << "row " << index << " level " << level;
		EXPECT_EQ(actual.size[level].mipmap_offset, expected.size[level].mipmap_offset) << "row " << index << " level " << level;
		EXPECT_EQ(actual.size[level].mipmap_size, expected.size[level].mipmap_size) << "row " << index << " level " << level;
		EXPECT_EQ(actual.padded[level].width, expected.padded[level].width) << "row " << index << " level " << level;
		EXPECT_EQ(actual.padded[level].height, expected.padded[level].height) << "row " << index << " level " << level;
	}
}

template <class Derive, size_t N>
int ExpectTable(const TextureInfo (&table)[N], Derive derive)
{
	int rows = 0;
	for (size_t index = 0; index < N; index++)
	{
		if (IsPlaceholder(table[index]))
		{
			continue;
		}
		ExpectSameRow(table[index], derive(table[index]), static_cast<int>(index));
		rows++;
	}
	return rows;
}

template <size_t N>
int ExpectLinearTable(const TextureInfo2 (&table)[N])
{
	for (size_t index = 0; index < N; index++)
	{
		const auto& row = table[index];
		if (row.levels > 16u)
		{
			ADD_FAILURE() << "row " << index << " has " << row.levels << " levels";
			continue;
		}
		const auto expected = ExpectLinear(row);
		EXPECT_EQ(row.total_size, expected.total_size) << "row " << index;
		EXPECT_EQ(row.total_align, expected.total_align) << "row " << index;
		for (uint32_t level = 0; level < 16; level++)
		{
			EXPECT_EQ(row.size[level].mipmap_offset, expected.size[level].mipmap_offset) << "row " << index << " level " << level;
			EXPECT_EQ(row.size[level].mipmap_size, expected.size[level].mipmap_size) << "row " << index << " level " << level;
			EXPECT_EQ(row.padded[level].width, expected.padded[level].width) << "row " << index << " level " << level;
			EXPECT_EQ(row.padded[level].height, expected.padded[level].height) << "row " << index << " level " << level;
		}
	}
	return static_cast<int>(N);
}

TextureInfo MicroFixed(const TextureInfo& row)
{
	return ExpectMicroTiled(row, false);
}

TextureInfo MicroPow2(const TextureInfo& row)
{
	return ExpectMicroTiled(row, true);
}

} // namespace

TEST(EmulatorGraphicsTables, MicroTiledRowsFollowThin1dLayout)
{
	int rows = 0;
	rows += ExpectTable(infos_13_10_0, MicroFixed) + ExpectTable(infos_13_10_0_pow2, MicroPow2);
	rows += ExpectTable(infos_13_10_9, MicroFixed) + ExpectTable(infos_13_10_9_pow2, MicroPow2);
	rows += ExpectTable(infos_13_35_0, MicroFixed) + ExpectTable(infos_13_35_0_pow2, MicroPow2);
	rows += ExpectTable(infos_13_36_0, MicroFixed) + ExpectTable(infos_13_36_0_pow2, MicroPow2);
	rows += ExpectTable(infos_13_37_0, MicroFixed) + ExpectTable(infos_13_37_0_pow2, MicroPow2);
	rows += ExpectTable(infos_13_37_9, MicroFixed) + ExpectTable(infos_13_37_9_pow2, MicroPow2);
	EXPECT_GT(rows, 0);
}

TEST(EmulatorGraphicsTables, DisplayAndDepthRowsFollowTheirFamilyLayout)
{
	int rows = 0;
	rows += ExpectTable(infos_8_1_0, ExpectDisplayLinear) + ExpectTable(infos_8_1_0_pow2, ExpectDisplayLinear);
	rows += ExpectTable(infos_8_10_0, ExpectDisplayLinear) + ExpectTable(infos_8_10_0_pow2, ExpectDisplayLinear);
	rows += ExpectTable(infos_8_10_9, ExpectDisplayLinear) + ExpectTable(infos_8_10_9_pow2, ExpectDisplayLinear);
	rows += ExpectTable(infos_10_10_0, ExpectDisplayThin) + ExpectTable(infos_10_10_0_pow2, ExpectDisplayThin);
	rows += ExpectTable(infos_14_10_0, ExpectThin2d) + ExpectTable(infos_14_10_0_pow2, ExpectThin2d);
	rows += ExpectTable(infos_2_4_7, ExpectDepthThin) + ExpectTable(infos_2_4_7_pow2, ExpectDepthThin);
	EXPECT_GT(rows, 0);
}

TEST(EmulatorGraphicsTables, LinearRowsStoreSmallestMipFirst)
{
	EXPECT_GT(ExpectLinearTable(infos_0_56) + ExpectLinearTable(infos_0_56_pow2), 0);
}

UT_END();
