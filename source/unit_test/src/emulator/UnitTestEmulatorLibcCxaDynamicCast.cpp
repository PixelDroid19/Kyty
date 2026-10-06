#include "Kyty/UnitTest.h"

#include "Emulator/Libs/CxaDynamicCast.h"

#include <cstdint>

UT_BEGIN(EmulatorLibcCxaDynamicCast);

using Kyty::Libs::LibC::CxaDynamicCastApply;
using Kyty::Libs::LibC::CxaDynamicCastResolve;
using Kyty::Libs::LibC::CxaTypeInfoVtables;

TEST(EmulatorLibcCxaDynamicCast, AppliesItaniumSrc2dstOffsetAndNullPolicy)
{
	alignas(16) unsigned char storage[64] {};
	void* const               base = storage + 16;

	EXPECT_EQ(CxaDynamicCastApply(nullptr, 0), nullptr);
	EXPECT_EQ(CxaDynamicCastApply(nullptr, -1), nullptr);

	// src2dst == 0: unique base at offset 0.
	EXPECT_EQ(CxaDynamicCastApply(base, 0), base);

	// src2dst > 0: src is base at that offset inside most-derived → subtract.
	EXPECT_EQ(CxaDynamicCastApply(base, 16), storage);

	// -1: unspecified relationship → same-address optimistic path.
	EXPECT_EQ(CxaDynamicCastApply(base, -1), base);

	// -2 / -3: fail closed.
	EXPECT_EQ(CxaDynamicCastApply(base, -2), nullptr);
	EXPECT_EQ(CxaDynamicCastApply(base, -3), nullptr);

	// RTTI walk: Root <- Text and Root <- Calendar (single inheritance). An
	// object whose dynamic type is Text must not cast to Calendar even with a
	// zero offset hint, and must still cast from its Root base to Text.
	const void* class_vtable[4] {};
	const void* si_vtable[4] {};
	const CxaTypeInfoVtables vtables {class_vtable, si_vtable, nullptr};
	const void* root_ti[2] {class_vtable + 2, "4Root"};
	const void* text_ti[3] {si_vtable + 2, "4Text", root_ti};
	const void* calendar_ti[3] {si_vtable + 2, "8Calendar", root_ti};
	const void* text_vtable[3] {nullptr, text_ti, nullptr};
	const void* text_object[2] {text_vtable + 2, nullptr};

	EXPECT_EQ(CxaDynamicCastResolve(text_object, root_ti, text_ti, 0, vtables), text_object);
	EXPECT_EQ(CxaDynamicCastResolve(text_object, root_ti, calendar_ti, 0, vtables), nullptr);
	EXPECT_EQ(CxaDynamicCastResolve(text_object, calendar_ti, text_ti, -2, vtables), nullptr);

	// Unknown type_info kind: fall back to the hint arithmetic.
	const void* foreign_ti[2] {nullptr, "7Foreign"};
	const void* foreign_vtable[3] {nullptr, foreign_ti, nullptr};
	const void* foreign_object[2] {foreign_vtable + 2, nullptr};
	EXPECT_EQ(CxaDynamicCastResolve(foreign_object, root_ti, text_ti, 0, vtables), foreign_object);
}

UT_END();
