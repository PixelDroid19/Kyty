#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/VulkanQueueIdentity.h"
#include "Emulator/Graphics/VulkanQueueSelection.h"

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

TEST(EmulatorVulkanQueueIdentity, SelectsOneFamilyForGraphicsComputeTransferAndPresentation)
{
	const VulkanQueueFamilyCapabilities families[] = {
	    {1, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, true},
	    {1, VK_QUEUE_TRANSFER_BIT, true},
	    {0, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, true},
	};
	VulkanQueueFamilySelection selected;

	EXPECT_EQ(VulkanSelectCommonRenderQueueFamily(families, 3, &selected), VulkanQueueSelectionResult::Success);
	EXPECT_EQ(selected.family, 0u);
	EXPECT_EQ(selected.index, 0u);
}

TEST(EmulatorVulkanQueueIdentity, RejectsSplitFamiliesWithoutOwnershipTransferSupport)
{
	const VulkanQueueFamilyCapabilities families[] = {
	    {1, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, false},
	    {1, VK_QUEUE_TRANSFER_BIT, true},
	};
	VulkanQueueFamilySelection selected;

	EXPECT_EQ(VulkanSelectCommonRenderQueueFamily(families, 2, &selected), VulkanQueueSelectionResult::NoCommonFamily);
}

TEST(EmulatorVulkanQueueIdentity, PrefersCommonFamilyWithMoreNativeQueueIndices)
{
	const VulkanQueueFamilyCapabilities families[] = {
	    {1, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, true},
	    {16, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, true},
	};
	VulkanQueueFamilySelection selected;

	EXPECT_EQ(VulkanSelectCommonRenderQueueFamily(families, 2, &selected), VulkanQueueSelectionResult::Success);
	EXPECT_EQ(selected.family, 1u);
	EXPECT_EQ(selected.index, 0u);
}

TEST(EmulatorVulkanQueueIdentity, SharesQueueRolesWhenTheFamilyHasOneQueue)
{
	VulkanQueueRoleAllocation allocation;

	EXPECT_EQ(VulkanAllocateSharedQueueRoles(1, 8, &allocation), VulkanQueueSelectionResult::Success);
	EXPECT_EQ(allocation.graphics_index, 0u);
	EXPECT_EQ(allocation.compute_first_index, 0u);
	EXPECT_EQ(allocation.compute_count, 1u);
	EXPECT_EQ(allocation.transfer_index, 0u);
	EXPECT_EQ(allocation.present_index, 0u);
	EXPECT_EQ(allocation.family_used, 1u);
}

TEST(EmulatorVulkanQueueIdentity, PreservesOneIndependentComputeQueueWhenOnlyTwoAreAvailable)
{
	VulkanQueueRoleAllocation allocation;

	EXPECT_EQ(VulkanAllocateSharedQueueRoles(2, 8, &allocation), VulkanQueueSelectionResult::Success);
	EXPECT_EQ(allocation.graphics_index, 0u);
	EXPECT_EQ(allocation.compute_first_index, 1u);
	EXPECT_EQ(allocation.compute_count, 1u);
	EXPECT_EQ(allocation.transfer_index, 0u);
	EXPECT_EQ(allocation.present_index, 0u);
	EXPECT_EQ(allocation.family_used, 2u);
}

TEST(EmulatorVulkanQueueIdentity, UsesAvailableIndicesForAllLogicalQueueRoles)
{
	VulkanQueueRoleAllocation allocation;

	EXPECT_EQ(VulkanAllocateSharedQueueRoles(16, 8, &allocation), VulkanQueueSelectionResult::Success);
	EXPECT_EQ(allocation.graphics_index, 0u);
	EXPECT_EQ(allocation.compute_first_index, 1u);
	EXPECT_EQ(allocation.compute_count, 8u);
	EXPECT_EQ(allocation.transfer_index, 9u);
	EXPECT_EQ(allocation.present_index, 10u);
	EXPECT_EQ(allocation.family_used, 11u);

	for (uint32_t i = 0; i < allocation.compute_count; i++)
	{
		EXPECT_EQ(allocation.compute_first_index + i, i + 1);
	}
}

UT_END();
