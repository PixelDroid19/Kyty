#include "Kyty/UnitTest.h"

#include "../../../emulator/src/Graphics/GraphicsRenderInternal.h"

#include <cstdint>

UT_BEGIN(EmulatorStencilLogicOps);

using Libs::Graphics::get_stencil_state;
using Libs::Graphics::PipelineStencilDynamicState;
using Libs::Graphics::PipelineStencilStaticState;

namespace {

constexpr uint8_t kAnd  = 0x0a;
constexpr uint8_t kOr   = 0x0b;
constexpr uint8_t kXor  = 0x0c;
constexpr uint8_t kNand = 0x0d;
constexpr uint8_t kNor  = 0x0e;
constexpr uint8_t kXnor = 0x0f;

constexpr uint8_t kFuncEqual  = 3;
constexpr uint8_t kFuncAlways = 7;

// DB_STENCIL_CONTROL logic ops combine the stored value with STENCILOPVAL.
uint8_t GuestLogic(uint8_t op, uint8_t s, uint8_t v)
{
	switch (op)
	{
		case kAnd: return static_cast<uint8_t>(s & v);
		case kOr: return static_cast<uint8_t>(s | v);
		case kXor: return static_cast<uint8_t>(s ^ v);
		case kNand: return static_cast<uint8_t>(~(s & v));
		case kNor: return static_cast<uint8_t>(~(s | v));
		default: return static_cast<uint8_t>(~(s ^ v));
	}
}

uint8_t HostOp(VkStencilOp op, uint8_t s, uint32_t reference)
{
	switch (op)
	{
		case VK_STENCIL_OP_KEEP: return s;
		case VK_STENCIL_OP_ZERO: return 0;
		case VK_STENCIL_OP_REPLACE: return static_cast<uint8_t>(reference);
		case VK_STENCIL_OP_INVERT: return static_cast<uint8_t>(~s);
		default: return 0x5a; // no logic op maps to a counting op
	}
}

uint8_t Masked(uint8_t s, uint8_t value, uint32_t mask)
{
	return static_cast<uint8_t>((s & ~mask) | (value & mask));
}

} // namespace

TEST(EmulatorStencilLogicOps, ExactMappingsWriteTheGuestResultForEveryValue)
{
	const uint8_t writemasks[] = {0xff, 0x0f, 0xa5};
	const uint8_t testval      = 0x3c;
	for (uint8_t op = kAnd; op <= kXnor; op++)
	{
		for (uint8_t writemask: writemasks)
		{
			for (uint32_t v = 0; v < 256u; v++)
			{
				PipelineStencilStaticState  st {};
				PipelineStencilDynamicState dyn {};
				const bool exact = get_stencil_state(&st, &dyn, kFuncAlways, 0, op, 0, testval, 0xff, writemask, static_cast<uint8_t>(v));

				const uint8_t covered     = static_cast<uint8_t>(v & writemask);
				const bool    expressible = !(op == kNand || op == kNor) || covered == 0u || covered == writemask;
				ASSERT_EQ(exact, expressible) << "op=" << int(op) << " v=" << v << " writemask=" << int(writemask);
				if (!exact)
				{
					EXPECT_EQ(st.passOp, VK_STENCIL_OP_KEEP);
					continue;
				}
				int mismatches = 0;
				for (uint32_t s = 0; s < 256u; s++)
				{
					const auto stored = static_cast<uint8_t>(s);
					const auto guest  = Masked(stored, GuestLogic(op, stored, static_cast<uint8_t>(v)), writemask);
					const auto host   = Masked(stored, HostOp(st.passOp, stored, dyn.reference), dyn.writeMask);
					mismatches += (guest != host) ? 1 : 0;
				}
				EXPECT_EQ(mismatches, 0) << "op=" << int(op) << " v=" << v << " writemask=" << int(writemask);
			}
		}
	}
}

TEST(EmulatorStencilLogicOps, ConflictingWriteMasksKeepTheOtherOpsWhole)
{
	PipelineStencilStaticState  st {};
	PipelineStencilDynamicState dyn {};
	// Fail replaces with testval over the whole mask; an AND on depth pass would narrow that shared mask.
	EXPECT_FALSE(get_stencil_state(&st, &dyn, kFuncAlways, 0x03, kAnd, 0, 0x81, 0xff, 0xff, 0x0f));
	EXPECT_EQ(st.failOp, VK_STENCIL_OP_REPLACE);
	EXPECT_EQ(st.passOp, VK_STENCIL_OP_KEEP);
	EXPECT_EQ(dyn.writeMask, 0xffu);
	EXPECT_EQ(dyn.reference, 0x81u);
}

TEST(EmulatorStencilLogicOps, OrSettingAComparedBitIsReportedInexact)
{
	PipelineStencilStaticState  st {};
	PipelineStencilDynamicState dyn {};
	// OR writes through the reference; bit 0 is compared but absent from testval.
	EXPECT_FALSE(get_stencil_state(&st, &dyn, kFuncEqual, 0, kOr, 0, 0x10, 0x01, 0xff, 0x01));
	// Within the compared bits testval already holds opval: the compare is unchanged.
	EXPECT_TRUE(get_stencil_state(&st, &dyn, kFuncEqual, 0, kOr, 0, 0x11, 0x0f, 0xff, 0x01));
	EXPECT_EQ(dyn.reference, 0x11u);
	EXPECT_EQ(dyn.writeMask, 0x01u);
}

UT_END();
