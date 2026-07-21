#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/VulkanQueueIdentity.h"

UT_BEGIN(EmulatorVulkanQueueIdentity);

using namespace Libs::Graphics;

namespace {

VkQueue FakeQueue(uintptr_t value)
{
	return reinterpret_cast<VkQueue>(value);
}

} // namespace

TEST(EmulatorVulkanQueueIdentity, SharesOneLockForEveryRoleOnTheSamePhysicalQueue)
{
	const VulkanQueueIdentity identities[] = {
	    {2, 0, FakeQueue(0x1000)},
	    {2, 0, FakeQueue(0x1000)},
	    {4, 1, FakeQueue(0x2000)},
	    {2, 0, FakeQueue(0x1000)},
	};
	uint32_t lock_indices[4] = {};
	uint32_t lock_count      = 0;

	ASSERT_EQ(VulkanAssignQueueLockIndices(identities, 4, lock_indices, &lock_count),
	          VulkanQueueLockAssignmentStatus::Success);
	EXPECT_EQ(lock_count, 2u);
	EXPECT_EQ(lock_indices[0], lock_indices[1]);
	EXPECT_EQ(lock_indices[0], lock_indices[3]);
	EXPECT_NE(lock_indices[0], lock_indices[2]);
}

TEST(EmulatorVulkanQueueIdentity, KeepsDistinctPhysicalQueuesConcurrent)
{
	const VulkanQueueIdentity identities[] = {
	    {3, 0, FakeQueue(0x1000)},
	    {3, 1, FakeQueue(0x2000)},
	    {4, 0, FakeQueue(0x3000)},
	};
	uint32_t lock_indices[3] = {};
	uint32_t lock_count      = 0;

	ASSERT_EQ(VulkanAssignQueueLockIndices(identities, 3, lock_indices, &lock_count),
	          VulkanQueueLockAssignmentStatus::Success);
	EXPECT_EQ(lock_count, 3u);
	EXPECT_NE(lock_indices[0], lock_indices[1]);
	EXPECT_NE(lock_indices[0], lock_indices[2]);
	EXPECT_NE(lock_indices[1], lock_indices[2]);
}

TEST(EmulatorVulkanQueueIdentity, RejectsConflictingMetadataForTheSameVulkanQueue)
{
	const VulkanQueueIdentity identities[] = {
	    {1, 0, FakeQueue(0x1000)},
	    {1, 1, FakeQueue(0x1000)},
	};
	uint32_t lock_indices[2] = {};
	uint32_t lock_count      = 0;

	EXPECT_EQ(VulkanAssignQueueLockIndices(identities, 2, lock_indices, &lock_count),
	          VulkanQueueLockAssignmentStatus::ConflictingIdentity);
}

TEST(EmulatorVulkanQueueIdentity, RejectsDifferentHandlesForTheSameFamilyAndIndex)
{
	const VulkanQueueIdentity identities[] = {
	    {1, 0, FakeQueue(0x1000)},
	    {1, 0, FakeQueue(0x2000)},
	};
	uint32_t lock_indices[2] = {};
	uint32_t lock_count      = 0;

	EXPECT_EQ(VulkanAssignQueueLockIndices(identities, 2, lock_indices, &lock_count),
	          VulkanQueueLockAssignmentStatus::ConflictingIdentity);
}

UT_END();
