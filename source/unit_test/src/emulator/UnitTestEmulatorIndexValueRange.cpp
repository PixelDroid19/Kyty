#include "Kyty/UnitTest.h"

#include "../../../emulator/src/Graphics/GraphicsRenderInternal.h"

#include <cstdint>
#include <cstring>
#include <vector>

UT_BEGIN(EmulatorIndexValueRange);

using namespace Libs::Graphics;

namespace {

// Indexes of `count` elements at an odd byte offset: the extremes at the given positions, `filler` elsewhere.
template <typename Index>
std::vector<uint8_t> UnalignedIndexes(uint32_t count, uint32_t min_at, uint32_t max_at, Index minimum, Index maximum, Index filler)
{
	std::vector<uint8_t> bytes(1u + static_cast<size_t>(count) * sizeof(Index));
	for (uint32_t i = 0; i < count; ++i)
	{
		Index value = i == min_at ? minimum : (i == max_at ? maximum : filler);
		std::memcpy(bytes.data() + 1u + static_cast<size_t>(i) * sizeof(Index), &value, sizeof(Index));
	}
	return bytes;
}

} // namespace

// The vector loop and its tail find the extremes at every position. The filler sits between them on the other
// side of the top bit, so a signed comparison would pick it.
TEST(EmulatorIndexValueRange, FindsUnsignedExtremesAtEveryPositionOfEveryLength)
{
	for (uint32_t count = 2; count <= 70u; ++count)
	{
		for (uint32_t at = 0; at < count; ++at)
		{
			const uint32_t next   = (at + 1u) % count;
			const auto     narrow = UnalignedIndexes<uint16_t>(count, at, next, 0x0001u, 0xfffeu, 0x7fffu);
			const auto     wide   = UnalignedIndexes<uint32_t>(count, next, at, 0x00000001u, 0xfffffffeu, 0x7fffffffu);
			const auto     r16    = GraphicsRenderIndexValueRange(narrow.data() + 1, count, 2u);
			const auto     r32    = GraphicsRenderIndexValueRange(wide.data() + 1, count, 4u);
			EXPECT_EQ(r16.first, 0x0001u) << count << " " << at;
			EXPECT_EQ(r16.second, 0xfffeu) << count << " " << at;
			EXPECT_EQ(r32.first, 0x00000001u) << count << " " << at;
			EXPECT_EQ(r32.second, 0xfffffffeu) << count << " " << at;
		}
	}
}

UT_END();
