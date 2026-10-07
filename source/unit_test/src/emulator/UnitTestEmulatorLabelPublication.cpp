#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Objects/Label.h"

#include <array>
#include <cstring>

UT_BEGIN(EmulatorLabelPublication);

using namespace Kyty::Libs::Graphics;

namespace {

void IgnoreWrite(void*, uint64_t, uint64_t) {}

struct Buffer
{
	std::array<uint64_t, 8> guest {};
	std::array<uint64_t, 8> gpu {};
	GpuWritebackPageCache cache {16};
	LabelStoragePublication publication;

	uint64_t Address(size_t offset = 0) { return reinterpret_cast<uint64_t>(guest.data()) + offset; }
	void Upload(const LabelFenceRegistry& registry)
	{
		publication.Upload(registry, gpu.data(), guest.data(), sizeof(guest), {{0, sizeof(guest)}}, &cache);
	}
	GpuWritebackResult Copy(const LabelFenceRegistry& registry)
	{
		return publication.Copy(registry, guest.data(), gpu.data(), sizeof(guest), &cache, IgnoreWrite, nullptr);
	}
};

} // namespace

TEST(EmulatorLabelPublication, CompletedLabelCanBeReusedByLaterUploadedStorageInSameMapping)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	buffer.Upload(registry);
	ASSERT_EQ(registry.Register(buffer.Address(), 4), LabelFenceRegistrationStatus::Inserted);
	buffer.guest[0] = 1;
	registry.Complete(buffer.Address(), 4);
	buffer.gpu[0] = 2;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 1u);
	EXPECT_TRUE(buffer.publication.NeedsUpload(registry, buffer.Address(), sizeof(buffer.guest)));
	// Exactly the storage upload path resets both acquisition version and GPU
	// snapshot. No allocation release or removal of the protected range occurs.
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	EXPECT_TRUE(buffer.Copy(registry).content_changed);
	EXPECT_EQ(buffer.guest[0], 2u);
	EXPECT_EQ(registry.Size(), 1u);
	EXPECT_FALSE(buffer.Copy(registry).content_changed);
}

TEST(EmulatorLabelPublication, OlderBackingRemainsExcludedAfterNewerBackingPublishes)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	GpuWritebackPageCache older_cache(16);
	LabelStoragePublication older_publication;
	std::array<uint64_t, 8> older_gpu {};
	older_publication.Upload(registry, older_gpu.data(), buffer.guest.data(), sizeof(buffer.guest), {{0, sizeof(buffer.guest)}},
	                         &older_cache);
	ASSERT_EQ(registry.Register(buffer.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	buffer.guest[0] = 1;
	registry.Complete(buffer.Address(), 8);
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	(void)buffer.Copy(registry);
	ASSERT_EQ(buffer.guest[0], 2u);
	older_gpu[0] = 3;
	(void)older_publication.Copy(registry, buffer.guest.data(), older_gpu.data(), sizeof(buffer.guest), &older_cache, IgnoreWrite, nullptr);
	EXPECT_EQ(buffer.guest[0], 2u);
	older_gpu[1] = 4;
	(void)older_publication.Copy(registry, buffer.guest.data(), older_gpu.data(), sizeof(buffer.guest), &older_cache, IgnoreWrite, nullptr);
	EXPECT_EQ(buffer.guest[0], 2u);
	EXPECT_EQ(buffer.guest[1], 4u);
}

TEST(EmulatorLabelPublication, NewCpuStoreToAcquiredLabelWordWinsOverGpuCopy)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	ASSERT_EQ(registry.Register(buffer.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	buffer.guest[0] = 1;
	registry.Complete(buffer.Address(), 8);
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	buffer.guest[0] = 0x100000001ULL;
	buffer.gpu[1] = 7;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 0x100000001ULL);
	EXPECT_EQ(buffer.guest[1], 7u);
	EXPECT_FALSE(buffer.Copy(registry).content_changed);
	buffer.gpu[1] = 8;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 0x100000001ULL);
}

TEST(EmulatorLabelPublication, PartialOverlapsAndConcurrentReservationsKeepTheirVersions)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	ASSERT_EQ(registry.Register(buffer.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	ASSERT_EQ(registry.Register(buffer.Address(4), 4), LabelFenceRegistrationStatus::Inserted);
	ASSERT_EQ(registry.Register(buffer.Address(4), 4), LabelFenceRegistrationStatus::AlreadyRegistered);
	std::memset(reinterpret_cast<void*>(buffer.Address()), 0x11, 8);
	registry.Complete(buffer.Address(), 8);
	registry.Complete(buffer.Address(4), 4);
	buffer.Upload(registry);
	std::memset(buffer.gpu.data(), 0x22, sizeof(buffer.gpu));
	(void)buffer.Copy(registry);
	const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.guest.data());
	EXPECT_EQ(bytes[0], 0x22);
	EXPECT_EQ(bytes[3], 0x22);
	EXPECT_EQ(bytes[4], 0x11);
	EXPECT_EQ(bytes[7], 0x11);
	EXPECT_EQ(bytes[8], 0x22);
	std::memset(reinterpret_cast<void*>(buffer.Address(4)), 0x33, 4);
	registry.Complete(buffer.Address(4), 4);
	(void)buffer.Copy(registry);
	EXPECT_EQ(bytes[4], 0x33);
	buffer.Upload(registry);
	std::memset(buffer.gpu.data(), 0x44, sizeof(buffer.gpu));
	(void)buffer.Copy(registry);
	EXPECT_EQ(bytes[4], 0x44);
	EXPECT_EQ(bytes[8], 0x44);
}

TEST(EmulatorLabelPublication, CpuStoreBetweenNotificationAndPublicationCannotBeClobbered)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	ASSERT_EQ(registry.Register(buffer.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	buffer.guest[0] = 1;
	registry.Complete(buffer.Address(), 8);
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	bool injected = false;
	struct Race
	{
		Buffer* buffer;
		bool* injected;
	} race {&buffer, &injected};
	const auto cpu_store = [](void* opaque, uint64_t address, uint64_t bytes)
	{
		auto* race = static_cast<Race*>(opaque);
		if (address == race->buffer->Address() && bytes == 8)
		{
			race->buffer->guest[0] = 3;
			*race->injected = true;
		}
	};
	(void)buffer.publication.Copy(registry, buffer.guest.data(), buffer.gpu.data(), sizeof(buffer.guest), &buffer.cache,
	                              cpu_store, &race);
	EXPECT_TRUE(injected);
	EXPECT_EQ(buffer.guest[0], 3u);
	EXPECT_TRUE(buffer.publication.NeedsUpload(registry, buffer.Address(), sizeof(buffer.guest)));
	// An unchanged old GPU snapshot must not be retried after CPU ABA.
	buffer.guest[0] = 1;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 1u);
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 2u);
}

TEST(EmulatorLabelPublication, OverlappingDwordCannotSplitNewerCpuQwordStore)
{
	LabelFenceRegistry registry;
	Buffer buffer;
	ASSERT_EQ(registry.Register(buffer.Address(), 4), LabelFenceRegistrationStatus::Inserted);
	registry.Complete(buffer.Address(), 4);
	ASSERT_EQ(registry.Register(buffer.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	buffer.guest[0] = 1;
	registry.Complete(buffer.Address(), 8);
	buffer.Upload(registry);
	buffer.gpu[0] = 2;
	// The CPU qword store changes only the upper dword. A dword-first publisher
	// would wrongly merge the lower GPU value into this newer CPU-owned word.
	buffer.guest[0] = 0x100000001ULL;
	(void)buffer.Copy(registry);
	EXPECT_EQ(buffer.guest[0], 0x100000001ULL);
}

TEST(EmulatorLabelPublication, UnrelatedAllocationCompletionDoesNotAcquireOrInvalidateOwnership)
{
	LabelFenceRegistry registry;
	Buffer first;
	Buffer second;
	ASSERT_EQ(registry.Register(first.Address(), 8), LabelFenceRegistrationStatus::Inserted);
	first.guest[0] = 1;
	registry.Complete(first.Address(), 8);
	first.Upload(registry);
	ASSERT_EQ(registry.Register(second.Address(), 4), LabelFenceRegistrationStatus::Inserted);
	second.guest[0] = 2;
	registry.Complete(second.Address(), 4);
	EXPECT_FALSE(first.publication.NeedsUpload(registry, first.Address(), sizeof(first.guest)));
	EXPECT_EQ(registry.ReleaseAllocation(second.Address(), sizeof(second.guest)), LabelFenceReleaseStatus::Released);
	EXPECT_EQ(registry.Size(), 1u);
	first.gpu[0] = 3;
	(void)first.Copy(registry);
	EXPECT_EQ(first.guest[0], 3u);
}

UT_END();
