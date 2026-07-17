#include <gtest/gtest.h>

#include "Emulator/Libs/CxaDynamicCast.h"

#include <cstdint>

namespace {

using Kyty::Libs::LibC::CxaDynamicCastApply;

TEST(EmulatorLibcCxaDynamicCast, AppliesItaniumSrc2dstOffsetAndNullPolicy)
{
	alignas(16) unsigned char storage[64] {};
	void* const               base = storage + 16;

	EXPECT_EQ(CxaDynamicCastApply(nullptr, 0), nullptr);
	EXPECT_EQ(CxaDynamicCastApply(nullptr, -1), nullptr);

	// src2dst == 0: unique base at offset 0 → same address (captured Dreaming Sarah).
	EXPECT_EQ(CxaDynamicCastApply(base, 0), base);

	// src2dst > 0: src is base at that offset inside most-derived → subtract.
	EXPECT_EQ(CxaDynamicCastApply(base, 16), storage);

	// -1: unspecified relationship → same-address optimistic path.
	EXPECT_EQ(CxaDynamicCastApply(base, -1), base);

	// -2 / -3: fail closed.
	EXPECT_EQ(CxaDynamicCastApply(base, -2), nullptr);
	EXPECT_EQ(CxaDynamicCastApply(base, -3), nullptr);
}

} // namespace
