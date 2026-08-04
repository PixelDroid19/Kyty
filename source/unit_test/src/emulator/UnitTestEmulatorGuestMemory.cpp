#include "Kyty/UnitTest.h"

#include "Emulator/GuestMemory.h"

UT_BEGIN(EmulatorGuestMemory);

namespace {

bool QueryMappedRange(uint64_t address, uint64_t size, Kyty::Emulator::GuestMemory::MappedRange* out)
{
	if (out == nullptr || address != 0x1000u || size != 0x2000u)
	{
		return false;
	}
	out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::Flexible;
	out->base = address;
	out->size = size;
	return true;
}

int QueryProtection(void* address, void** start, void** end, int* protection)
{
	if (address == nullptr)
	{
		return -1;
	}
	if (start != nullptr)
	{
		*start = address;
	}
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(address) + 0x1000u);
	}
	if (protection != nullptr)
	{
		*protection = 3;
	}
	return 0;
}

} // namespace

TEST(EmulatorGuestMemory, RejectsIncompleteCallbacksAndForwardsCompleteQueries)
{
	Kyty::Emulator::GuestMemory::Port port;
	EXPECT_FALSE(port.Install({}));
	ASSERT_TRUE(port.Install({QueryMappedRange, QueryProtection}));
	EXPECT_FALSE(port.Install({QueryMappedRange, QueryProtection}));

	Kyty::Emulator::GuestMemory::MappedRange range {};
	ASSERT_TRUE(port.QueryMappedRange(0x1000u, 0x2000u, &range));
	EXPECT_EQ(range.kind, Kyty::Emulator::GuestMemory::MappedRangeKind::Flexible);
	EXPECT_EQ(range.base, 0x1000u);
	EXPECT_EQ(range.size, 0x2000u);

	void* start = nullptr;
	void* end   = nullptr;
	int   protection = 0;
	EXPECT_EQ(port.QueryProtection(reinterpret_cast<void*>(0x2000u), &start, &end, &protection), 0);
	EXPECT_EQ(start, reinterpret_cast<void*>(0x2000u));
	EXPECT_EQ(reinterpret_cast<uintptr_t>(end), 0x3000u);
	EXPECT_EQ(protection, 3);
}

TEST(EmulatorGuestMemory, UninstalledPortFailsClosed)
{
	Kyty::Emulator::GuestMemory::Port port;
	Kyty::Emulator::GuestMemory::MappedRange range {};
	EXPECT_FALSE(port.QueryMappedRange(0x1000u, 0x1000u, &range));
	EXPECT_EQ(port.QueryProtection(reinterpret_cast<void*>(0x1000u), nullptr, nullptr, nullptr), -1);
}

UT_END();
