#include "Kyty/UnitTest.h"
#include "../../../emulator/src/Graphics/GuestHostImportPlan.h"

#include <cstdint>
#include <limits>
#include <vulkan/vulkan_core.h>

UT_BEGIN(EmulatorGuestHostImport);

using namespace Libs::Graphics::GuestHostImportPlan;

TEST(EmulatorGuestHostImport, IntersectsHostAndBufferMemoryTypeMasks)
{
	ImportPlan plan;
	EXPECT_TRUE(PlanPhysicalImport(0x10000u, 0x10000u, 0x1000u, 0x10000u, 0b1010u, 0b1100u, false, false, &plan));
	EXPECT_EQ(plan.memory_type_bits, 0b1000u);
	EXPECT_FALSE(PlanPhysicalImport(0x10000u, 0x10000u, 0x1000u, 0x10000u, 0b0010u, 0b1100u, false, false, &plan));
}

TEST(EmulatorGuestHostImport, SelectsOnlyHostVisibleCoherentCompatibleMemory)
{
	const uint32_t flags[] = {
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	};
	uint32_t selected = std::numeric_limits<uint32_t>::max();

	EXPECT_TRUE(SelectMemoryType(0b111u, flags, 3u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                            &selected));
	EXPECT_EQ(selected, 2u);
	EXPECT_FALSE(SelectMemoryType(0b011u, flags, 3u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                             &selected));
}

TEST(EmulatorGuestHostImport, RejectsRequiredButAllowsPreferredDedicatedAllocation)
{
	ImportPlan plan;
	EXPECT_FALSE(PlanPhysicalImport(0x10000u, 0x10000u, 0x1000u, 0x10000u, 1u, 1u, true, false, &plan));
	EXPECT_TRUE(PlanPhysicalImport(0x10000u, 0x10000u, 0x1000u, 0x10000u, 1u, 1u, false, true, &plan));
}

TEST(EmulatorGuestHostImport, RejectsSmallPhysicalSpanButPadsAnOwnedSnapshot)
{
	ImportPlan plan;
	EXPECT_FALSE(PlanPhysicalImport(0x10000u, 0x1000u, 0x1000u, 0x10000u, 1u, 1u, false, false, &plan));
	EXPECT_TRUE(PlanOwnedSnapshotImport(0x10000u, 0x1000u, 0x1000u, 0x10000u, 0x10000u, 0x200000u, 1u, 1u, false, false,
	                                   &plan));
	EXPECT_EQ(plan.host_capacity, 0x10000u);
	EXPECT_EQ(plan.allocation_size, 0x10000u);
}

TEST(EmulatorGuestHostImport, RejectsMisalignedPointersAndPhysicalRequirementOverrun)
{
	ImportPlan plan;
	EXPECT_FALSE(PlanPhysicalImport(0x11000u, 0x10000u, 0x1000u, 0x10000u, 1u, 1u, false, false, &plan));
	EXPECT_FALSE(PlanPhysicalImport(0x10000u, 0x1000u, 0x2000u, 0x1000u, 1u, 1u, false, false, &plan));
	EXPECT_FALSE(PlanOwnedSnapshotImport(0x11000u, 0x1000u, 0x1000u, 0x10000u, 0x10000u, 0x200000u, 1u, 1u, false, false,
	                                    &plan));
}

TEST(EmulatorGuestHostImport, SnapshotCanCoverLargerRequirementsAndRejectsPaddingOverflow)
{
	ImportPlan plan;
	EXPECT_TRUE(PlanOwnedSnapshotImport(0x10000u, 0x1000u, 0x2000u, 0x10000u, 0x10000u, 0x200000u, 1u, 1u, false, false,
	                                   &plan));
	EXPECT_EQ(plan.host_capacity, 0x10000u);
	EXPECT_FALSE(PlanOwnedSnapshotCapacity(0x1000u, std::numeric_limits<uint64_t>::max(), 0x10000u, 0x200000u,
	                                       &plan.host_capacity));
	EXPECT_FALSE(PlanOwnedSnapshotCapacity(0x1000u, 0x1000u, 0x10000u, 0x1000u, &plan.host_capacity));
}

UT_END();

