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

TEST(CoreMSpace, MixedWorkloadKeepsLargeRequestsSatisfiable)
{
	constexpr size_t   kCapacity   = 8u * 1024u * 1024u;
	constexpr uint32_t kIterations = 2000;
	auto*              storage     = new uint8_t[kCapacity];
	auto*              m           = MSpaceCreate("mixed", storage, kCapacity, true, nullptr);
	EXPECT_NE(m, nullptr);
	if (m == nullptr)
	{
		delete[] storage;
		return;
	}

	void*    kept[kIterations] = {};
	uint32_t kept_head         = 0;
	uint32_t kept_tail         = 0;
	for (uint32_t i = 0; i < kIterations; i++)
	{
		void* t = MSpaceMalloc(m, 65536u + (i * 7919u) % 65536u);
		void* s = MSpaceMalloc(m, 256u + (i * 31u) % 512u);
		EXPECT_NE(t, nullptr);
		EXPECT_NE(s, nullptr);
		if (s != nullptr)
		{
			kept[kept_tail++] = s;
		}
		EXPECT_TRUE(MSpaceFree(m, t));
		if (i % 4u == 0u && kept_head < kept_tail)
		{
			EXPECT_TRUE(MSpaceFree(m, kept[kept_head++]));
		}
	}

	void* large = MSpaceMalloc(m, 4u * 1024u * 1024u);
	EXPECT_NE(large, nullptr);
	EXPECT_TRUE(MSpaceFree(m, large));

	for (uint32_t k = kept_head; k < kept_tail; k++)
	{
		EXPECT_TRUE(MSpaceFree(m, kept[k]));
	}
	EXPECT_TRUE(Core::MSpaceIsHeapEmpty(m));
	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] storage;
}

static constexpr size_t kStressMaxLive = 512;

struct StressAllocation
{
	uint8_t* buf   = nullptr;
	uint32_t id    = 0;
	size_t   size  = 0;
	size_t   align = 0;
};

struct StressHeap
{
	Core::mspace_t  mspace  = nullptr;
	StressAllocation live[kStressMaxLive];
	size_t          count   = 0;
	uint32_t        next_id = 1;
	uint32_t        state   = 0;
};

static uint8_t StressPattern(uint32_t id, size_t offset)
{
	return static_cast<uint8_t>(id * 31u + offset * 7u);
}

static void StressFill(const StressAllocation& a)
{
	for (size_t j = 0; j < a.size; j++)
	{
		a.buf[j] = StressPattern(a.id, j);
	}
}

static bool StressMatches(const StressAllocation& a, size_t count)
{
	for (size_t j = 0; j < count; j++)
	{
		if (a.buf[j] != StressPattern(a.id, j))
		{
			return false;
		}
	}
	return true;
}

// The high half of a 32-bit LCG is used; its low bits have short periods.
static uint32_t StressNext(uint32_t& state)
{
	state = state * 1664525u + 1013904223u;
	return state >> 16u;
}

static size_t StressSize(uint32_t& state)
{
	if (StressNext(state) % 50u != 0u)
	{
		return 1u + StressNext(state) % 4096u;
	}
	const uint32_t high = StressNext(state);
	const uint32_t low  = StressNext(state);
	return 1u + ((high << 16u) | low) % 524288u;
}

static void StressAdd(StressHeap& h, bool aligned_request)
{
	const size_t size  = StressSize(h.state);
	size_t       align = 32u;
	uint8_t*     buf   = nullptr;
	if (aligned_request)
	{
		align = 32u << (StressNext(h.state) % 8u);
		buf   = static_cast<uint8_t*>(Core::MSpaceMemalign(h.mspace, align, size));
	} else
	{
		buf = static_cast<uint8_t*>(MSpaceMalloc(h.mspace, size));
	}
	// Running out of arena is a valid outcome; the next operation tries again.
	if (buf == nullptr)
	{
		return;
	}

	EXPECT_EQ(reinterpret_cast<uintptr_t>(buf) % align, 0u);
	EXPECT_GE(Core::MSpaceMallocUsableSize(buf), size);

	StressAllocation& a = h.live[h.count];
	a.buf               = buf;
	a.id                = h.next_id++;
	a.size              = size;
	a.align             = align;
	h.count++;
	StressFill(a);
}

static void StressRealloc(StressHeap& h, size_t index)
{
	StressAllocation& a = h.live[index];
	// Realloc guarantees only the 32-byte granule, so over-aligned blocks are not resized here.
	if (a.align != 32u)
	{
		return;
	}

	const size_t size  = StressSize(h.state);
	auto*        grown = static_cast<uint8_t*>(MSpaceRealloc(h.mspace, a.buf, size));
	if (grown == nullptr)
	{
		EXPECT_TRUE(StressMatches(a, a.size));
		return;
	}

	StressAllocation moved = a;
	moved.buf              = grown;
	EXPECT_TRUE(StressMatches(moved, a.size < size ? a.size : size));

	EXPECT_EQ(reinterpret_cast<uintptr_t>(grown) % 32u, 0u);
	EXPECT_GE(Core::MSpaceMallocUsableSize(grown), size);

	moved.size = size;
	a          = moved;
	StressFill(a);
}

static void StressRemove(StressHeap& h, size_t index)
{
	const StressAllocation& a = h.live[index];
	EXPECT_TRUE(StressMatches(a, a.size));
	EXPECT_TRUE(MSpaceFree(h.mspace, a.buf));
	h.live[index] = h.live[h.count - 1];
	h.count--;
}

TEST(CoreMSpace, RandomizedStressKeepsDataIntact)
{
	constexpr size_t   kCapacity = 4u * 1024u * 1024u;
	constexpr uint32_t kOps      = 20000;
	auto*              storage   = new uint8_t[kCapacity];
	auto*              m         = MSpaceCreate("stress", storage, kCapacity, true, nullptr);
	EXPECT_NE(m, nullptr);
	if (m == nullptr)
	{
		delete[] storage;
		return;
	}

	Core::MSpaceSize fresh {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &fresh));

	StressHeap heap;
	heap.mspace = m;
	heap.state  = 0x2468ace1u;

	for (uint32_t op = 0; op < kOps; op++)
	{
		const uint32_t choice = StressNext(heap.state) % 100u;
		if (heap.count == 0u || (choice < 60u && heap.count < kStressMaxLive))
		{
			StressAdd(heap, choice >= 35u);
		} else if (choice >= 60u && choice < 80u)
		{
			StressRealloc(heap, StressNext(heap.state) % heap.count);
		} else
		{
			StressRemove(heap, StressNext(heap.state) % heap.count);
		}
	}

	while (heap.count > 0u)
	{
		StressRemove(heap, heap.count - 1u);
	}
	EXPECT_TRUE(Core::MSpaceIsHeapEmpty(m));

	Core::MSpaceSize after {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &after));
	EXPECT_EQ(after.current_inuse_size, fresh.current_inuse_size);

	void* whole = MSpaceMalloc(m, kCapacity - 64u * 1024u);
	EXPECT_NE(whole, nullptr);
	EXPECT_TRUE(MSpaceFree(m, whole));
	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] storage;
}

TEST(CoreMSpace, StatsTrackInUseBytes)
{
	constexpr size_t kCapacity = 0x10000;
	constexpr size_t kRequest  = 1000;
	auto*            buf       = new uint8_t[kCapacity];
	auto*            m         = MSpaceCreate("inuse", buf, kCapacity, true, nullptr);
	EXPECT_NE(m, nullptr);
	if (m == nullptr)
	{
		delete[] buf;
		return;
	}

	Core::MSpaceSize fresh {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &fresh));

	// The guard keeps the freed block apart from the untouched rest of the arena.
	void* p     = MSpaceMalloc(m, kRequest);
	void* guard = MSpaceMalloc(m, 64);
	EXPECT_NE(p, nullptr);
	EXPECT_NE(guard, nullptr);
	Core::MSpaceSize used {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &used));
	EXPECT_GE(used.current_inuse_size, fresh.current_inuse_size + kRequest + 64);
	EXPECT_GE(used.max_inuse_size, used.current_inuse_size);

	EXPECT_TRUE(MSpaceFree(m, p));
	Core::MSpaceSize freed {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &freed));
	EXPECT_LE(freed.current_inuse_size, used.current_inuse_size - kRequest);
	EXPECT_GE(freed.max_inuse_size, used.current_inuse_size);

	EXPECT_TRUE(MSpaceFree(m, guard));
	Core::MSpaceSize empty {};
	EXPECT_TRUE(Core::MSpaceMallocStats(m, &empty));
	EXPECT_EQ(empty.current_inuse_size, fresh.current_inuse_size);

	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] buf;
}

// memalign(4096) leaves the user pointer far inside its raw chunk; a plain (32-byte) realloc must keep
// the caller's bytes at the returned pointer. The arena starts on a 4096 boundary so the placement is fixed.
TEST(CoreMSpace, ReallocOfOverAlignedBlockKeepsData)
{
	constexpr size_t kCapacity  = 0x10000;
	constexpr size_t kBlockSize = 1000;
	auto*            storage    = new uint8_t[kCapacity + 4096];
	auto*            buf        = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage) + 4095u) & ~static_cast<uintptr_t>(4095u));
	auto*            m          = MSpaceCreate("realloc-align", buf, kCapacity, true, nullptr);
	EXPECT_NE(m, nullptr);
	if (m == nullptr)
	{
		delete[] storage;
		return;
	}

	const size_t new_sizes[] = {kBlockSize + 8, kBlockSize - 500};
	for (size_t new_size: new_sizes)
	{
		auto* block = static_cast<uint8_t*>(Core::MSpaceMemalign(m, 4096, kBlockSize));
		EXPECT_NE(block, nullptr);
		if (block == nullptr)
		{
			break;
		}

		StressAllocation original {};
		original.buf  = block;
		original.id   = 1;
		original.size = kBlockSize;
		StressFill(original);

		const size_t kept  = (new_size < kBlockSize ? new_size : kBlockSize);
		auto*        moved = static_cast<uint8_t*>(MSpaceRealloc(m, block, new_size));
		EXPECT_NE(moved, nullptr);
		if (moved != nullptr)
		{
			StressAllocation resized {};
			resized.buf  = moved;
			resized.id   = 1;
			resized.size = new_size;
			EXPECT_TRUE(StressMatches(resized, kept));
			EXPECT_TRUE(MSpaceFree(m, moved));
		}
	}

	EXPECT_TRUE(Core::MSpaceIsHeapEmpty(m));
	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] storage;
}

// 0xFFFFFFC5 plus the 48-byte aligned-path padding is 0xFFFFFFF5 internally, and its block count wraps in 32 bits.
TEST(CoreMSpace, OversizedRequestIsRefusedWithoutTouchingArena)
{
	constexpr size_t kCapacity = 0x10000;
	auto*            buf       = new uint8_t[kCapacity];
	auto*            m         = MSpaceCreate("oversize", buf, kCapacity, true, nullptr);
	EXPECT_NE(m, nullptr);
	if (m == nullptr)
	{
		delete[] buf;
		return;
	}

	EXPECT_EQ(MSpaceMalloc(m, 0xFFFFFFC5u), nullptr);
	EXPECT_TRUE(Core::MSpaceIsHeapEmpty(m));

	EXPECT_TRUE(MSpaceDestroy(m));
	delete[] buf;
}

UT_END();
