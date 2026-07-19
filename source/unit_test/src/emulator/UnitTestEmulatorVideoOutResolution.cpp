#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/ResolutionAliasPolicy.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VideoOutFlipLifecycleGate.h"
#include "Emulator/Graphics/VideoOutMaterializationGate.h"

#include <atomic>
#include <chrono>
#include <future>
#include <initializer_list>
#include <thread>

UT_BEGIN(EmulatorVideoOutResolution);

using namespace Libs::Graphics;

namespace {

void SetNativeImage(VideoOutVulkanImage* image, uint32_t width = 3840, uint32_t height = 2160)
{
	ASSERT_NE(image, nullptr);
	image->SetNativeExtent(width, height);
}

GpuMemoryOverlapEntry Exact(GpuMemoryObjectType type, uint32_t count = 1)
{
	return {type, GpuMemoryOverlapType::Equals, count, true};
}

GpuMemoryOverlapSnapshot Snapshot(std::initializer_list<GpuMemoryOverlapEntry> entries)
{
	GpuMemoryOverlapSnapshot result;
	for (const auto& entry: entries)
	{
		result.entries[result.entry_count++] = entry;
		result.total_count += entry.count;
		if (entry.exact)
		{
			result.exact_count += entry.count;
		}
	}
	return result;
}

} // namespace

TEST(EmulatorVideoOutResolution, SelectsEveryImageBeforeReportingScaledSetSuccess)
{
	VideoOutVulkanImage first;
	VideoOutVulkanImage second;
	VideoOutVulkanImage third;
	SetNativeImage(&first);
	SetNativeImage(&second);
	SetNativeImage(&third);
	VideoOutVulkanImage* images[] = {&first, &second, &third};

	VideoOutHostExtentSetState state;
	EXPECT_EQ(VideoOutBufferSelectHostExtentSet(images, 3, 1280, 720, &state), VideoOutHostExtentSetSelectionStatus::Selected);
	EXPECT_EQ(state.width, 1280u);
	EXPECT_EQ(state.height, 720u);
	EXPECT_EQ(state.image_count, 3u);

	for (auto* image: images)
	{
		VideoOutHostExtentState image_state;
		ASSERT_TRUE(VideoOutBufferGetHostExtentState(image, &image_state));
		EXPECT_TRUE(image_state.selected);
		EXPECT_EQ(image_state.width, 1280u);
		EXPECT_EQ(image_state.height, 720u);
	}
}

TEST(EmulatorVideoOutResolution, ExplicitlySelectsNativeExtentForEveryImage)
{
	VideoOutVulkanImage first;
	VideoOutVulkanImage second;
	SetNativeImage(&first);
	SetNativeImage(&second);
	VideoOutVulkanImage* images[] = {&first, &second};

	VideoOutHostExtentSetState state;
	EXPECT_EQ(VideoOutBufferSelectHostExtentSet(images, 2, 3840, 2160, &state), VideoOutHostExtentSetSelectionStatus::Selected);
	EXPECT_EQ(VideoOutBufferInspectHostExtentSet(images, 2, &state), VideoOutHostExtentSetInspectionStatus::Uniform);
	EXPECT_EQ(state.width, 3840u);
	EXPECT_EQ(state.height, 2160u);
	EXPECT_EQ(state.image_count, 2u);
}

TEST(EmulatorVideoOutResolution, StickyMismatchDoesNotPartiallySelectEarlierImages)
{
	VideoOutVulkanImage first;
	VideoOutVulkanImage second;
	SetNativeImage(&first);
	SetNativeImage(&second);
	VideoOutHostExtentState selected_state;
	ASSERT_EQ(VideoOutBufferSelectHostExtent(&second, 1920, 1080, &selected_state), VideoOutHostExtentStatus::Selected);
	VideoOutVulkanImage* images[] = {&first, &second};

	VideoOutHostExtentSetState set_state;
	EXPECT_EQ(VideoOutBufferSelectHostExtentSet(images, 2, 1280, 720, &set_state), VideoOutHostExtentSetSelectionStatus::StickyMismatch);

	VideoOutHostExtentState first_state;
	ASSERT_TRUE(VideoOutBufferGetHostExtentState(&first, &first_state));
	EXPECT_FALSE(first_state.selected);
	EXPECT_EQ(first_state.width, 3840u);
	EXPECT_EQ(first_state.height, 2160u);
}

TEST(EmulatorVideoOutResolution, RejectsDuplicateImagesBeforeSelectingTheSet)
{
	VideoOutVulkanImage image;
	SetNativeImage(&image);
	VideoOutVulkanImage* images[] = {&image, &image};

	VideoOutHostExtentSetState state;
	EXPECT_EQ(VideoOutBufferSelectHostExtentSet(images, 2, 1280, 720, &state), VideoOutHostExtentSetSelectionStatus::InvalidArgument);

	VideoOutHostExtentState image_state;
	ASSERT_TRUE(VideoOutBufferGetHostExtentState(&image, &image_state));
	EXPECT_FALSE(image_state.selected);
}

TEST(EmulatorVideoOutResolution, ConcurrentSetSelectionsCommitOneUniformExtent)
{
	for (uint32_t iteration = 0; iteration < 128; iteration++)
	{
		VideoOutVulkanImage first;
		VideoOutVulkanImage second;
		VideoOutVulkanImage third;
		SetNativeImage(&first);
		SetNativeImage(&second);
		SetNativeImage(&third);
		VideoOutVulkanImage* images[] = {&first, &second, &third};

		std::atomic<uint32_t> ready {0};
		std::atomic<bool>     start {false};
		auto                  wait_for_start = [&ready, &start]
		{
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
		};

		VideoOutHostExtentSetState           first_state;
		VideoOutHostExtentSetState           second_state;
		VideoOutHostExtentSetSelectionStatus first_status {};
		VideoOutHostExtentSetSelectionStatus second_status {};
		std::thread                          first_selection(
		    [&]
		    {
			    wait_for_start();
			    first_status = VideoOutBufferSelectHostExtentSet(images, 3, 1280, 720, &first_state);
		    });
		std::thread second_selection(
		    [&]
		    {
			    wait_for_start();
			    second_status = VideoOutBufferSelectHostExtentSet(images, 3, 1920, 1080, &second_state);
		    });

		while (ready.load(std::memory_order_acquire) != 2)
		{
			std::this_thread::yield();
		}
		start.store(true, std::memory_order_release);
		first_selection.join();
		second_selection.join();

		const bool first_won  = first_status == VideoOutHostExtentSetSelectionStatus::Selected &&
		                        second_status == VideoOutHostExtentSetSelectionStatus::StickyMismatch;
		const bool second_won = second_status == VideoOutHostExtentSetSelectionStatus::Selected &&
		                        first_status == VideoOutHostExtentSetSelectionStatus::StickyMismatch;
		EXPECT_TRUE(first_won || second_won);

		VideoOutHostExtentSetState committed_state;
		ASSERT_EQ(VideoOutBufferInspectHostExtentSet(images, 3, &committed_state), VideoOutHostExtentSetInspectionStatus::Uniform);
		const bool committed_first  = committed_state.width == 1280 && committed_state.height == 720;
		const bool committed_second = committed_state.width == 1920 && committed_state.height == 1080;
		EXPECT_TRUE(committed_first || committed_second);
	}
}

TEST(EmulatorVideoOutResolution, MaterializationGateWaitsUntilAllPinsAreReleased)
{
	VideoOutMaterializationGate gate;
	std::promise<void>          waiter_entered;
	std::promise<void>          waiter_completed;
	auto                        entered   = waiter_entered.get_future();
	auto                        completed = waiter_completed.get_future();
	std::thread                 waiter;

	{
		auto pin = gate.Acquire();
		waiter   = std::thread(
		    [&]
		    {
			    waiter_entered.set_value();
			    gate.WaitUntilIdle();
			    waiter_completed.set_value();
		    });

		EXPECT_EQ(entered.wait_for(std::chrono::seconds(1)), std::future_status::ready);
		EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	}

	EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	waiter.join();
}

TEST(EmulatorVideoOutResolution, FlipLifecycleDrainWaitsForEveryAcceptedRequestOfTheExactOwner)
{
	VideoOutFlipLifecycleGate gate;
	uint32_t                  first_owner  = 0;
	uint32_t                  second_owner = 0;
	gate.Accept(&first_owner);
	gate.Accept(&first_owner);
	gate.Accept(&second_owner);

	std::promise<void> waiter_entered;
	std::promise<void> waiter_completed;
	auto               entered   = waiter_entered.get_future();
	auto               completed = waiter_completed.get_future();
	std::thread        waiter(
	    [&]
	    {
		    waiter_entered.set_value();
		    gate.WaitUntilIdle(&first_owner);
		    waiter_completed.set_value();
	    });

	EXPECT_EQ(entered.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	gate.Complete(&second_owner);
	EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	gate.Complete(&first_owner);
	EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	gate.Complete(&first_owner);
	EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	waiter.join();
}

TEST(EmulatorVideoOutResolution, ExposesStableVideoOutSelectionAndInspectionStatusNames)
{
	EXPECT_STREQ(VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus::Selected), "selected");
	EXPECT_STREQ(VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus::StickyMatch), "sticky_match");
	EXPECT_STREQ(VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus::StickyMismatch), "sticky_mismatch");
	EXPECT_STREQ(VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus::InvalidArgument), "invalid_argument");
	EXPECT_STREQ(VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus::Empty), "empty");

	using Libs::VideoOut::VideoOutRegisteredHostExtentStatus;
	using Libs::VideoOut::VideoOutRegisteredHostExtentStatusName;
	EXPECT_STREQ(VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus::Uniform), "uniform");
	EXPECT_STREQ(VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus::Unselected), "unselected");
	EXPECT_STREQ(VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus::NonUniform), "non_uniform");
	EXPECT_STREQ(VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus::InvalidArgument), "invalid_argument");
	EXPECT_STREQ(VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus::NoBuffers), "no_buffers");
}

TEST(EmulatorVideoOutResolution, InspectionRejectsUnselectedAndNonUniformSets)
{
	VideoOutVulkanImage first;
	VideoOutVulkanImage second;
	SetNativeImage(&first);
	SetNativeImage(&second);
	VideoOutVulkanImage*       images[] = {&first, &second};
	VideoOutHostExtentSetState state;

	EXPECT_EQ(VideoOutBufferInspectHostExtentSet(images, 2, &state), VideoOutHostExtentSetInspectionStatus::Unselected);

	VideoOutHostExtentState image_state;
	ASSERT_EQ(VideoOutBufferSelectHostExtent(&first, 1280, 720, &image_state), VideoOutHostExtentStatus::Selected);
	ASSERT_EQ(VideoOutBufferSelectHostExtent(&second, 1920, 1080, &image_state), VideoOutHostExtentStatus::Selected);
	EXPECT_EQ(VideoOutBufferInspectHostExtentSet(images, 2, &state), VideoOutHostExtentSetInspectionStatus::NonUniform);
}

TEST(EmulatorVideoOutResolution, AllowsObservedExactVideoOutStorageAlias)
{
	const auto overlaps = Snapshot({Exact(GpuMemoryObjectType::VideoOutBuffer), Exact(GpuMemoryObjectType::StorageBuffer)});
	EXPECT_TRUE(ResolutionAliasPolicyAllowsSnapshot(overlaps, GpuMemoryObjectType::VideoOutBuffer, false));
}

TEST(EmulatorVideoOutResolution, RejectsPartialMultipleTruncatedAndUnexpectedAliases)
{
	auto partial        = Snapshot({{GpuMemoryObjectType::VideoOutBuffer, GpuMemoryOverlapType::Crosses, 1, false}});
	auto multiple       = Snapshot({Exact(GpuMemoryObjectType::VideoOutBuffer, 2)});
	auto unexpected     = Snapshot({Exact(GpuMemoryObjectType::VideoOutBuffer), Exact(GpuMemoryObjectType::Texture)});
	auto truncated      = Snapshot({Exact(GpuMemoryObjectType::VideoOutBuffer)});
	truncated.truncated = true;

	EXPECT_FALSE(ResolutionAliasPolicyAllowsSnapshot(partial, GpuMemoryObjectType::VideoOutBuffer, false));
	EXPECT_FALSE(ResolutionAliasPolicyAllowsSnapshot(multiple, GpuMemoryObjectType::VideoOutBuffer, false));
	EXPECT_FALSE(ResolutionAliasPolicyAllowsSnapshot(unexpected, GpuMemoryObjectType::VideoOutBuffer, false));
	EXPECT_FALSE(ResolutionAliasPolicyAllowsSnapshot(truncated, GpuMemoryObjectType::VideoOutBuffer, false));
}

TEST(EmulatorVideoOutResolution, AllowsEmptyDepthOnlyWhenCallerAuthorizesIt)
{
	const GpuMemoryOverlapSnapshot empty;
	EXPECT_TRUE(ResolutionAliasPolicyAllowsSnapshot(empty, GpuMemoryObjectType::DepthStencilBuffer, true));
	EXPECT_FALSE(ResolutionAliasPolicyAllowsSnapshot(empty, GpuMemoryObjectType::DepthStencilBuffer, false));
}

UT_END();
