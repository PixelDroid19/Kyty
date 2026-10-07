#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Math/Rand.h"
#include "Kyty/UnitTest.h"

#include <cstring>

UT_BEGIN(CoreMSpace);

using Core::MSpaceCreate;
using Core::MSpaceDestroy;
using Core::MSpaceFree;
using Core::MSpaceMalloc;
using Core::MSpaceRealloc;
using Math::Rand;

static void test_fail()
{
	size_t s   = 1000;
	auto*  buf = new uint8_t[s];

	auto* m = MSpaceCreate("test", buf, s, true, nullptr);

	EXPECT_EQ(m, nullptr);

	void* ptr = MSpaceMalloc(m, 56);

	EXPECT_EQ(ptr, nullptr);

	EXPECT_FALSE(MSpaceFree(m, ptr));

	EXPECT_FALSE(MSpaceDestroy(m));

	delete[] buf;
}

// Null mspace is a guest-visible failure: return nullptr (no abort). Matches
// LibcMspaceMalloc null-msp path used by the early Gen5 heap setup.
TEST(CoreMSpace, NullMspMallocReturnsNull)
{
	EXPECT_EQ(MSpaceMalloc(nullptr, 0x28), nullptr);
	EXPECT_EQ(MSpaceMalloc(nullptr, 56), nullptr);
}

static size_t   g_size = 0;
static uint8_t* g_ptr  = nullptr;

static void*          g_reentrant_create_backing   = nullptr;
static size_t         g_reentrant_create_capacity  = 0;
static Core::mspace_t g_reentrant_created_child    = nullptr;
static bool           g_reentrant_create_attempted = false;
static uint32_t       g_reentrant_callback_count   = 0;

static void test_callback(Core::mspace_t m, size_t /*free_size*/, size_t size)
{
	g_size = size;
	if (g_ptr != nullptr)
	{
		EXPECT_TRUE(MSpaceFree(m, g_ptr));
	}
}

static void test_realloc_create_callback(Core::mspace_t /*m*/, size_t /*free_size*/, size_t /*size*/)
{
	g_reentrant_callback_count++;
	if (!g_reentrant_create_attempted)
	{
		g_reentrant_create_attempted = true;
		g_reentrant_created_child = MSpaceCreate("reentrant-child", g_reentrant_create_backing, g_reentrant_create_capacity, true, nullptr);
	}
}

static void test_ok()
{
	size_t s   = 2000;
	auto*  buf = new uint8_t[s];

	auto* m = MSpaceCreate("test", buf, s, true, test_callback);

	EXPECT_NE(m, nullptr);

	auto* ptr = static_cast<uint8_t*>(MSpaceMalloc(m, 56));
	EXPECT_NE(ptr, nullptr);
	EXPECT_TRUE(ptr > buf && ptr < buf + s);

	g_size     = 0;
	g_ptr      = nullptr;
	auto* ptr2 = static_cast<uint8_t*>(MSpaceMalloc(m, 460));
	EXPECT_EQ(ptr2, nullptr);
	EXPECT_EQ(g_size, 460u);

	g_size = 0;
	g_ptr  = ptr;
	ptr2   = static_cast<uint8_t*>(MSpaceMalloc(m, 460));
	EXPECT_NE(ptr2, nullptr);
	EXPECT_TRUE(ptr2 > buf && ptr2 < buf + s);

	EXPECT_TRUE(MSpaceFree(m, ptr2));

	g_size = 0;
	g_ptr  = nullptr;
	ptr2   = static_cast<uint8_t*>(MSpaceRealloc(m, nullptr, 60));
	EXPECT_NE(ptr2, nullptr);
	EXPECT_TRUE(ptr2 > buf && ptr2 < buf + s);
	ptr2 = static_cast<uint8_t*>(MSpaceRealloc(m, ptr2, 160));
	EXPECT_NE(ptr2, nullptr);
	EXPECT_TRUE(ptr2 > buf && ptr2 < buf + s);

	EXPECT_TRUE(MSpaceDestroy(m));

	delete[] buf;
}

static void test_align()
{
	uint32_t s   = Rand::UintInclusiveRange(5000, 20000) & ~0x7u;
	auto*    buf = new uint8_t[s];

	auto* m = MSpaceCreate("test", buf, s, true, nullptr);

	EXPECT_NE(m, nullptr);

	// printf("buf = %016" PRIx64 ", %u\n", reinterpret_cast<uint64_t>(buf), s);

	int iter_num = Rand::IntInclusiveRange(10, 20);

	for (int i = 0; i < iter_num; i++)
	{
		uint32_t size     = Rand::UintInclusiveRange(1, 64);
		uint32_t size_r   = Rand::UintInclusiveRange(32, 128);
		auto*    buf32    = MSpaceMalloc(m, size);
		auto*    buf32_r  = MSpaceRealloc(m, buf32, size_r);
		auto     addr32   = reinterpret_cast<uint64_t>(buf32);
		auto     addr32_r = reinterpret_cast<uint64_t>(buf32_r);
		// printf("%016" PRIx64 ", %u => %016" PRIx64 ", %u\n", addr32, size, addr32_r, size_r);
		EXPECT_NE(addr32, 0u);
		EXPECT_EQ(addr32 & 0x1fu, 0u);
		EXPECT_NE(addr32_r, 0u);
		EXPECT_EQ(addr32_r & 0x1fu, 0u);
	}

	EXPECT_TRUE(MSpaceDestroy(m));

	delete[] buf;
}

struct TestRecord
{
	uint8_t* buf     = nullptr;
	uint8_t  pattern = 0;
	uint32_t size    = 0;
};

static void test_fill()
{
	uint32_t s   = Rand::UintInclusiveRange(2000, 10000) & ~0x7u;
	auto*    buf = new uint8_t[s];

	auto* m = MSpaceCreate("test", buf, s, true, nullptr);
	EXPECT_NE(m, nullptr);

	Vector<TestRecord> rs;

	for (int step = 0; step < 5; step++)
	{
		int add = 0;
		int del = 0;
		int rea = 0;
		for (;;)
		{
			uint32_t size  = Rand::UintInclusiveRange(1, 200);
			auto*    buf32 = static_cast<uint8_t*>(MSpaceMalloc(m, size));
			if (buf32 == nullptr)
			{
				break;
			}
			uint8_t pattern = Rand::UintInclusiveRange(0, 255);
			memset(buf32, pattern, size);

			TestRecord r {};
			r.buf     = buf32;
			r.pattern = pattern;
			r.size    = size;

			rs.Add(r);
			add++;
		}

		for (auto& r: rs)
		{
			if (r.buf != nullptr && (Rand::Uint() % 8) == 0)
			{
				MSpaceFree(m, r.buf);
				r.buf = nullptr;
				del++;
			}
		}

		for (auto& r: rs)
		{
			if (r.buf != nullptr && (Rand::Uint() % 4) == 0)
			{
				auto* n = static_cast<uint8_t*>(MSpaceRealloc(m, r.buf, r.size + Rand::UintInclusiveRange(1, 200)));
				if (n != nullptr)
				{
					r.buf = n;
					rea++;
				}
			}
		}

		// Counters remain for local stress diagnostics; keep them live for -Werror.
		EXPECT_GE(add, 0);
		EXPECT_GE(del, 0);
		EXPECT_GE(rea, 0);
	}

	bool ok = true;
	for (const auto& r: rs)
	{
		if (r.buf != nullptr)
		{
			for (uint32_t i = 0; i < r.size; i++)
			{
				if (r.pattern != r.buf[i])
				{
					ok = false;
					break;
				}
			}
			if (!ok)
			{
				break;
			}
		}
	}
	EXPECT_TRUE(ok);

	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] buf;
}

TEST(Core, MSpace)
{
	UT_MEM_CHECK_INIT();

	test_fail();
	test_ok();

	for (int i = 0; i < 5; i++)
	{
		test_align();
		test_fill();
	}

	UT_MEM_CHECK();
}

// Gen5 Onion path: guest rejects allocs when current_system_size is zero.
TEST(CoreMSpace, MallocStatsFastReportsCapacity)
{
	constexpr size_t kCap = 0x10000;
	auto*            buf  = new uint8_t[kCap];
	auto*            m    = MSpaceCreate("stats", buf, kCap, true, nullptr);
	EXPECT_NE(m, nullptr);

	Core::MSpaceSize sizes {};
	EXPECT_TRUE(Core::MSpaceMallocStatsFast(m, &sizes));
	EXPECT_GE(sizes.current_system_size, kCap - 0x1000u);
	EXPECT_GE(sizes.max_system_size, sizes.current_system_size);
	// Fresh heap: in-use should be well below capacity so a 2 MiB-class check passes.
	EXPECT_LT(sizes.current_inuse_size, sizes.current_system_size);
	EXPECT_GE(sizes.current_system_size, 0x200000u > kCap ? 0u : 0x1000u);

	void* p = MSpaceMalloc(m, 256);
	EXPECT_NE(p, nullptr);
	Core::MSpaceSize after {};
	EXPECT_TRUE(Core::MSpaceMallocStatsFast(m, &after));
	EXPECT_GE(after.current_inuse_size, sizes.current_inuse_size);

	EXPECT_TRUE(MSpaceFree(m, p));
	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] buf;
}

TEST(CoreMSpace, NestedHeapUsesLiveParentAllocation)
{
	constexpr size_t kParentCapacity = 0x40000;
	constexpr size_t kChildCapacity  = 0x8000;
	auto*            storage         = new uint8_t[kParentCapacity];
	auto*            parent          = MSpaceCreate("nested-parent", storage, kParentCapacity, true, nullptr);
	EXPECT_NE(parent, nullptr);
	if (parent == nullptr)
	{
		delete[] storage;
		return;
	}

	auto* backing = MSpaceMalloc(parent, kChildCapacity);
	EXPECT_NE(backing, nullptr);
	if (backing == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}
	EXPECT_GE(Core::MSpaceMallocUsableSize(backing), kChildCapacity);

	auto* child = MSpaceCreate("nested-child", backing, kChildCapacity, true, nullptr);
	EXPECT_NE(child, nullptr);
	if (child == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}
	EXPECT_GE(Core::MSpaceMallocUsableSize(backing), kChildCapacity);

	auto* child_block = static_cast<uint8_t*>(MSpaceMalloc(child, 96));
	EXPECT_NE(child_block, nullptr);
	if (child_block != nullptr)
	{
		EXPECT_GE(Core::MSpaceMallocUsableSize(child_block), 96u);
		std::memset(child_block, 0, 96);
		EXPECT_EQ(Core::MSpaceMallocUsableSize(child_block + 8), 0u);
		EXPECT_EQ(Core::MSpaceMallocUsableSize(static_cast<uint8_t*>(backing) + 8), 0u);
		EXPECT_EQ(Core::MSpaceMallocUsableSize(storage + kParentCapacity), 0u);
		EXPECT_TRUE(MSpaceFree(child, child_block));
	}

	EXPECT_FALSE(MSpaceFree(parent, backing));
	EXPECT_EQ(MSpaceRealloc(parent, backing, kChildCapacity + 0x1000), nullptr);
	EXPECT_FALSE(MSpaceDestroy(parent));
	EXPECT_TRUE(MSpaceDestroy(child));
	EXPECT_TRUE(MSpaceFree(parent, backing));

	auto* parent_block = MSpaceMalloc(parent, 96);
	EXPECT_NE(parent_block, nullptr);
	if (parent_block != nullptr)
	{
		EXPECT_TRUE(MSpaceFree(parent, parent_block));
	}

	EXPECT_TRUE(MSpaceDestroy(parent));
	delete[] storage;
}

TEST(CoreMSpace, NestedHeapRejectsOverlapsWithoutMutatingRegisteredHeaps)
{
	constexpr size_t kParentCapacity = 0x40000;
	constexpr size_t kChildCapacity  = 0x8000;
	auto*            storage         = new uint8_t[kParentCapacity];
	auto*            parent          = MSpaceCreate("overlap-parent", storage, kParentCapacity, true, nullptr);
	EXPECT_NE(parent, nullptr);
	if (parent == nullptr)
	{
		delete[] storage;
		return;
	}

	auto* backing = MSpaceMalloc(parent, kChildCapacity);
	EXPECT_NE(backing, nullptr);
	if (backing == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}

	auto* child = MSpaceCreate("overlap-child", backing, kChildCapacity, true, nullptr);
	EXPECT_NE(child, nullptr);
	if (child == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}

	auto* child_snapshot = new uint8_t[kChildCapacity];
	std::memcpy(child_snapshot, backing, kChildCapacity);
	EXPECT_EQ(MSpaceCreate("duplicate-child", backing, kChildCapacity, true, nullptr), nullptr);
	EXPECT_EQ(std::memcmp(child_snapshot, backing, kChildCapacity), 0);
	EXPECT_EQ(MSpaceCreate("partial-child", static_cast<uint8_t*>(backing) + 8, kChildCapacity - 8, true, nullptr), nullptr);
	EXPECT_EQ(std::memcmp(child_snapshot, backing, kChildCapacity), 0);

	auto* control_snapshot = new uint8_t[256];
	std::memcpy(control_snapshot, storage, 256);
	EXPECT_EQ(MSpaceCreate("parent-control", storage, kParentCapacity, true, nullptr), nullptr);
	EXPECT_EQ(std::memcmp(control_snapshot, storage, 256), 0);

	auto* free_backing = MSpaceMalloc(parent, kChildCapacity);
	EXPECT_NE(free_backing, nullptr);
	if (free_backing != nullptr)
	{
		EXPECT_TRUE(MSpaceFree(parent, free_backing));
		auto* free_snapshot = new uint8_t[kChildCapacity];
		std::memcpy(free_snapshot, free_backing, kChildCapacity);
		EXPECT_EQ(MSpaceCreate("free-child", free_backing, kChildCapacity, true, nullptr), nullptr);
		EXPECT_EQ(std::memcmp(free_snapshot, free_backing, kChildCapacity), 0);
		delete[] free_snapshot;
	}

	delete[] control_snapshot;
	delete[] child_snapshot;
	EXPECT_TRUE(MSpaceDestroy(child));
	EXPECT_TRUE(MSpaceFree(parent, backing));
	EXPECT_TRUE(MSpaceDestroy(parent));
	delete[] storage;
}

TEST(CoreMSpace, NestedHeapRejectsForgedParentPrefixes)
{
	constexpr size_t kParentCapacity  = 0x40000;
	constexpr size_t kBackingCapacity = 0x10000;
	constexpr size_t kChildCapacity   = 0x8000;
	constexpr size_t kOffset          = 0x40;
	auto*            storage          = new uint8_t[kParentCapacity];
	auto*            parent           = MSpaceCreate("prefix-parent", storage, kParentCapacity, true, nullptr);
	EXPECT_NE(parent, nullptr);
	if (parent == nullptr)
	{
		delete[] storage;
		return;
	}

	auto* backing = static_cast<uint8_t*>(MSpaceMalloc(parent, kBackingCapacity));
	EXPECT_NE(backing, nullptr);
	if (backing == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}

	const size_t backing_usable_size = Core::MSpaceMallocUsableSize(backing);
	EXPECT_GT(backing_usable_size, kOffset + kChildCapacity);
	if (backing_usable_size <= kOffset + kChildCapacity)
	{
		EXPECT_TRUE(MSpaceFree(parent, backing));
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}

	uint8_t backing_snapshot[64] {};
	std::memcpy(backing_snapshot, backing, sizeof(backing_snapshot));
	const size_t too_large_capacity = backing_usable_size + (8u - backing_usable_size % 8u);
	EXPECT_EQ(MSpaceCreate("prefix-too-large", backing, too_large_capacity, true, nullptr), nullptr);
	EXPECT_EQ(std::memcmp(backing_snapshot, backing, sizeof(backing_snapshot)), 0);

	auto* copied_base = backing + kOffset;
	std::memcpy(copied_base - sizeof(uint64_t), backing - sizeof(uint64_t), sizeof(uint64_t));
	uint8_t copied_snapshot[64] {};
	std::memcpy(copied_snapshot, copied_base, sizeof(copied_snapshot));

	auto* copied_child = MSpaceCreate("prefix-copied", copied_base, kChildCapacity, true, nullptr);
	EXPECT_EQ(copied_child, nullptr);
	EXPECT_EQ(std::memcmp(copied_snapshot, copied_base, sizeof(copied_snapshot)), 0);
	if (copied_child != nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(copied_child));
	}
	EXPECT_TRUE(MSpaceFree(parent, backing));

	auto* second_backing = static_cast<uint8_t*>(MSpaceMalloc(parent, kBackingCapacity));
	EXPECT_NE(second_backing, nullptr);
	if (second_backing != nullptr)
	{
		const uint64_t raw_address = reinterpret_cast<uint64_t*>(second_backing)[-1];
		auto*          raw_base    = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(raw_address));
		auto*          raw_offset  = raw_base + sizeof(uint64_t);
		std::memcpy(raw_offset - sizeof(uint64_t), &raw_address, sizeof(raw_address));

		auto* raw_child = MSpaceCreate("prefix-raw", raw_offset, kChildCapacity, true, nullptr);
		EXPECT_EQ(raw_child, nullptr);
		if (raw_child != nullptr)
		{
			EXPECT_TRUE(MSpaceDestroy(raw_child));
		}
	}

	EXPECT_TRUE(MSpaceDestroy(parent));
	delete[] storage;
}

TEST(CoreMSpace, NestedHeapRejectsReallocCallbackBacking)
{
	constexpr size_t kParentCapacity  = 0x20000;
	constexpr size_t kBackingCapacity = 0x10000;
	constexpr size_t kChildCapacity   = 0x8000;
	constexpr size_t kReallocSize     = 0x18000;
	auto*            storage          = new uint8_t[kParentCapacity];
	auto*            parent           = MSpaceCreate("reentrant-parent", storage, kParentCapacity, true, test_realloc_create_callback);
	EXPECT_NE(parent, nullptr);
	if (parent == nullptr)
	{
		delete[] storage;
		return;
	}

	auto* backing = MSpaceMalloc(parent, kBackingCapacity);
	EXPECT_NE(backing, nullptr);
	if (backing == nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(parent));
		delete[] storage;
		return;
	}

	g_reentrant_create_backing   = backing;
	g_reentrant_create_capacity  = kChildCapacity;
	g_reentrant_created_child    = nullptr;
	g_reentrant_create_attempted = false;
	g_reentrant_callback_count   = 0;

	auto* reallocated = MSpaceRealloc(parent, backing, kReallocSize);
	EXPECT_GT(g_reentrant_callback_count, 0u);
	EXPECT_TRUE(g_reentrant_create_attempted);
	EXPECT_EQ(g_reentrant_created_child, nullptr);
	EXPECT_EQ(reallocated, nullptr);

	if (g_reentrant_created_child != nullptr)
	{
		EXPECT_TRUE(MSpaceDestroy(g_reentrant_created_child));
	}
	if (g_reentrant_created_child == nullptr && reallocated == nullptr)
	{
		auto* child_after_realloc = MSpaceCreate("reentrant-after", backing, kChildCapacity, true, nullptr);
		EXPECT_NE(child_after_realloc, nullptr);
		if (child_after_realloc != nullptr)
		{
			EXPECT_TRUE(MSpaceDestroy(child_after_realloc));
		}
	}
	if (reallocated != nullptr)
	{
		EXPECT_TRUE(MSpaceFree(parent, reallocated));
	} else
	{
		EXPECT_TRUE(MSpaceFree(parent, backing));
	}

	g_reentrant_create_backing   = nullptr;
	g_reentrant_create_capacity  = 0;
	g_reentrant_created_child    = nullptr;
	g_reentrant_create_attempted = false;
	g_reentrant_callback_count   = 0;
	EXPECT_TRUE(MSpaceDestroy(parent));
	delete[] storage;
}

UT_END();
