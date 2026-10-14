#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Graphics/VideoOutFlipLifecycleGate.h"
#include "Emulator/Graphics/VideoOutHostAccessGate.h"
#include "Emulator/Graphics/VideoOutMaterializationGate.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <thread>
#include <utility>

UT_BEGIN(EmulatorVideoOutLifecycle);

using namespace Libs::Graphics;
namespace VirtualMemory = Core::VirtualMemory;

// These CPU fixtures use the production admission/progress gates, accepted
// registration predicate and completion accounting. The explicit presentation
// checkpoints stand in for the host drawing backend; no GPU completion is
// inferred from elapsed time.

TEST(EmulatorVideoOutLifecycle, CloseBeforeFirstCaptureRetainsAcceptedRegistration)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 1, 1};
	const auto accepted = registered;
	flips.Accept(&owner);
	std::promise<void> consume;
	auto consume_gate = consume.get_future();
	std::atomic_uint32_t presented {0};
	std::atomic_bool closing {false};
	std::thread presenter([&] {
		consume_gate.wait();
		// CaptureRegisteredImageLocked uses this same predicate with the
		// identity retained by the accepted FlipQueue::Request.
		EXPECT_TRUE(registered.CanAccess(true, closing.load(), accepted));
		presented.fetch_add(1);
		flips.Complete(&owner);
	});

	auto drain = host.Drain();
	closing.store(true);
	EXPECT_FALSE(registered.CanAccess(true, true, {}));
	consume.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(presented.load(), 1u);
	EXPECT_FALSE(registered.CanAccess(false, true, accepted));
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, CloseBetweenFirstFlipAndVblankAllowsSecondFlipToDrain)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 3, 7};
	flips.Accept(&owner);
	flips.Accept(&owner);
	std::promise<void> first_completed;
	std::promise<void> resume_vblank;
	auto first = first_completed.get_future();
	auto resume = resume_vblank.get_future();
	std::atomic_uint32_t presented {0};
	std::atomic_bool closing {false};
	std::thread presenter([&] {
		EXPECT_TRUE(registered.CanAccess(true, false, registered));
		presented.fetch_add(1);
		flips.Complete(&owner);
		first_completed.set_value();
		resume.wait();
		{
			// This is the active Window -> VideoOutEndVblank seam between
			// FlipWindow calls. Admission is closed before we resume it.
			auto vblank = host.AcquireProgress();
			EXPECT_TRUE(closing.load());
		}
		EXPECT_TRUE(registered.CanAccess(true, closing.load(), registered));
		presented.fetch_add(1);
		flips.Complete(&owner);
	});

	first.wait();
	auto drain = host.Drain();
	closing.store(true);
	EXPECT_EQ(presented.load(), 1u);
	resume_vblank.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(presented.load(), 2u);
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, CloseWaitsForAlreadyPinnedPresentation)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	VideoOutMaterializationGate materialization;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 1, 2};
	flips.Accept(&owner);
	std::promise<void> captured;
	std::promise<void> finish_presentation;
	auto capture_gate = captured.get_future();
	auto finish_gate = finish_presentation.get_future();
	std::atomic_bool presented {false};
	std::thread presenter([&] {
		{
			auto pin = materialization.Acquire();
			EXPECT_TRUE(registered.CanAccess(true, false, registered));
			captured.set_value();
			finish_gate.wait();
			EXPECT_TRUE(registered.CanAccess(true, true, registered));
		}
		presented.store(true);
		flips.Complete(&owner);
	});

	capture_gate.wait();
	auto drain = host.Drain();
	EXPECT_FALSE(presented.load());
	finish_presentation.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	materialization.WaitUntilIdle();
	EXPECT_TRUE(presented.load());
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, AdmissionRemainsClosedWhilePresenterCanProgress)
{
	VideoOutHostAccessGate host;
	auto drain = host.Drain();
	std::promise<void> admission_attempted;
	std::promise<void> admission_acquired;
	auto attempted = admission_attempted.get_future();
	auto acquired = admission_acquired.get_future();
	std::thread producer([&] {
		admission_attempted.set_value();
		auto pin = host.Acquire();
		admission_acquired.set_value();
	});
	attempted.wait();
	{
		auto vblank = host.AcquireProgress();
		EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	}
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	exclusive.Reset();
	acquired.wait();
	producer.join();
}

TEST(EmulatorVideoOutLifecycle, ExclusiveTeardownWaitsForPresenterProgressPin)
{
	VideoOutHostAccessGate host;
	auto drain = host.Drain();
	auto vblank = host.AcquireProgress();
	std::promise<void> teardown_attempted;
	std::promise<void> teardown_acquired;
	auto attempted = teardown_attempted.get_future();
	auto acquired = teardown_acquired.get_future();
	std::thread closer([&, drain = std::move(drain)]() mutable {
		teardown_attempted.set_value();
		auto exclusive = drain.Quiesce();
		teardown_acquired.set_value();
	});
	attempted.wait();
	EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	vblank.Reset();
	acquired.wait();
	closer.join();
}

TEST(EmulatorVideoOutLifecycle, CloseOneOwnerDoesNotRetireAnotherOwnersAcceptedFlip)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int first_owner = 0;
	int second_owner = 0;
	const VideoOutRegistrationIdentity first {&first_owner, 1, 1};
	const VideoOutRegistrationIdentity second {&second_owner, 1, 1};
	flips.Accept(&first_owner);
	flips.Accept(&second_owner);
	auto drain = host.Drain();
	EXPECT_TRUE(first.CanAccess(true, true, first));
	EXPECT_TRUE(second.CanAccess(true, false, second));
	EXPECT_FALSE(second.CanAccess(true, false, first));
	flips.Complete(&first_owner);
	flips.WaitUntilIdle(&first_owner);
	auto exclusive = drain.Quiesce();
	EXPECT_FALSE(first.CanAccess(false, false, first));
	EXPECT_TRUE(second.CanAccess(true, false, second));
	exclusive.Reset();
	{
		auto vblank = host.AcquireProgress();
	}
	flips.Complete(&second_owner);
	flips.WaitUntilIdle(&second_owner);
}

TEST(EmulatorVideoOutLifecycle, ZeroOutstandingCloseAndReopenRejectStaleGenerations)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity accepted {&owner, 1, 1};
	auto drain = host.Drain();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_FALSE(accepted.CanAccess(false, true, accepted));
	const VideoOutRegistrationIdentity reopened {&owner, 2, 1};
	const VideoOutRegistrationIdentity reregistered {&owner, 1, 2};
	EXPECT_FALSE(reopened.CanAccess(true, false, accepted));
	EXPECT_FALSE(reregistered.CanAccess(true, false, accepted));
	EXPECT_TRUE(reopened.CanAccess(true, false, {}));
	EXPECT_TRUE(reopened.CanAccess(true, true, reopened));
	EXPECT_FALSE(reopened.CanAccess(true, true, {}));
}

namespace {

constexpr size_t kOutputOptionsSize = 0x40;

int OutputContractHandle()
{
	static const int handle = []
	{
		if (!Config::IsInitialized())
		{
			Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		Libs::VideoOut::VideoOutInit(1920, 1080);
		return Libs::VideoOut::VideoOutOpen(255, 0, 0, nullptr);
	}();
	return handle;
}

class GuestOutputOptionsBuffer
{
public:
	GuestOutputOptionsBuffer(): m_page_size(VirtualMemory::GetPageSize())
	{
		if (m_page_size != 0)
		{
			m_address = VirtualMemory::Alloc(0, m_page_size * 2u, VirtualMemory::Mode::ReadWrite);
		}
	}

	~GuestOutputOptionsBuffer()
	{
		if (m_address != 0)
		{
			(void)VirtualMemory::Free(m_address);
		}
	}

	GuestOutputOptionsBuffer(const GuestOutputOptionsBuffer&)            = delete;
	GuestOutputOptionsBuffer& operator=(const GuestOutputOptionsBuffer&) = delete;

	[[nodiscard]] bool IsValid() const { return m_address != 0; }
	[[nodiscard]] uint64_t PageSize() const { return m_page_size; }
	[[nodiscard]] uint8_t* Data() const { return reinterpret_cast<uint8_t*>(m_address); }
	[[nodiscard]] uint8_t* At(uint64_t offset) const { return Data() + offset; }
	[[nodiscard]] bool ProtectPage(uint64_t index, VirtualMemory::Mode mode) const
	{
		return m_address != 0 && index < 2 && VirtualMemory::Protect(m_address + m_page_size * index, m_page_size, mode);
	}

private:
	uint64_t m_page_size = 0;
	uint64_t m_address   = 0;
};

std::array<uint8_t, kOutputOptionsSize> OutputContractOptions()
{
	std::array<uint8_t, kOutputOptionsSize> options {};
	options[2] = 0xff;
	return options;
}

void StoreOutputOptions(uint8_t* destination, const std::array<uint8_t, kOutputOptionsSize>& options)
{
	std::memcpy(destination, options.data(), options.size());
}

} // namespace

TEST(EmulatorVideoOutLifecycle, OutputOptionsInitializeExactAbiExtent)
{
	using namespace Libs::VideoOut;
	ASSERT_GT(OutputContractHandle(), 0);
	GuestOutputOptionsBuffer storage;
	ASSERT_TRUE(storage.IsValid());
	std::memset(storage.Data(), 0xaa, 82);
	ASSERT_EQ(VideoOutInitializeOutputOptions(storage.At(9)), 0);
	const auto expected = OutputContractOptions();
	for (size_t i = 0; i < expected.size(); ++i)
	{
		EXPECT_EQ(storage.At(9)[i], expected[i]) << i;
	}
	for (size_t i = 0; i < 9; ++i)
	{
		EXPECT_EQ(storage.Data()[i], 0xaa);
	}
	for (size_t i = 0; i < 9; ++i)
	{
		EXPECT_EQ(storage.At(73)[i], 0xaa);
	}
	EXPECT_EQ(VideoOutInitializeOutputOptions(nullptr), VIDEO_OUT_ERROR_INVALID_ADDRESS);
}

TEST(EmulatorVideoOutLifecycle, OutputModeChecksHandleReservedModeThenOptions)
{
	using namespace Libs::VideoOut;
	const int handle = OutputContractHandle();
	ASSERT_GT(handle, 0);
	GuestOutputOptionsBuffer options;
	ASSERT_TRUE(options.IsValid());
	const auto valid = OutputContractOptions();
	std::array<uint8_t, kOutputOptionsSize> invalid{};
	StoreOutputOptions(options.At(1), invalid);
	constexpr int unknown_mode = VIDEO_OUT_ERROR_INVALID_OUTPUT_MODE;
	for (const int bad_handle: {-1, 0, std::numeric_limits<int>::max()})
	{
		EXPECT_EQ(VideoOutIsOutputSupported(bad_handle, 2, options.At(1), options.At(1), 1), VIDEO_OUT_ERROR_INVALID_HANDLE);
	}
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 2, options.At(1), options.At(1), 0), VIDEO_OUT_ERROR_INVALID_VALUE);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 4, options.At(1), nullptr, 1), VIDEO_OUT_ERROR_INVALID_VALUE);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 2, options.At(1), nullptr, 0), unknown_mode);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 4, options.At(1), nullptr, 0), VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, options.At(1), nullptr, 0), VIDEO_OUT_ERROR_INVALID_VALUE);
	StoreOutputOptions(options.At(1), valid);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, options.At(1), nullptr, 0), 1);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, nullptr, nullptr, 0), 1);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 15, options.At(1), nullptr, 0), 0);
	for (const uint64_t mode: {4u, 7u, 8u, 12u, 13u, 14u, 16u, 17u, 19u})
	{
		EXPECT_EQ(VideoOutIsOutputSupported(handle, mode, nullptr, nullptr, 0), VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE);
	}
	for (const uint64_t mode: std::array<uint64_t, 8>{0, 2, 3, 20, 63, 0x100, uint64_t{1} << 63, UINT64_MAX})
	{
		EXPECT_EQ(VideoOutIsOutputSupported(handle, mode, nullptr, nullptr, 0), unknown_mode);
	}
	for (size_t word = 0; word < 16; ++word)
	{
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			auto changed = valid;
			changed[word * 4 + bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
			StoreOutputOptions(options.At(1), changed);
			EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, options.At(1), nullptr, 0), word == 3 ? 1 : VIDEO_OUT_ERROR_INVALID_VALUE)
				<< word << ',' << bit;
		}
	}
}

TEST(EmulatorVideoOutLifecycle, ConfigureOutputConsumesTheModeAndOptionArguments)
{
	using namespace Libs::VideoOut;
	const int handle = OutputContractHandle();
	ASSERT_GT(handle, 0);
	GuestOutputOptionsBuffer options;
	ASSERT_TRUE(options.IsValid());
	const auto valid = OutputContractOptions();
	std::array<uint8_t, kOutputOptionsSize> invalid{};
	StoreOutputOptions(options.At(1), valid);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 15, options.At(1), nullptr, 0), VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE);
	StoreOutputOptions(options.At(1), invalid);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 1, options.At(1), nullptr, 0), VIDEO_OUT_ERROR_INVALID_VALUE);
	StoreOutputOptions(options.At(1), valid);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 1, options.At(1), options.At(1), 1), VIDEO_OUT_ERROR_INVALID_VALUE);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 1, options.At(1), nullptr, 0), 0);
}

TEST(EmulatorVideoOutLifecycle, OutputOptionsRejectUnmappedAndOverflowingRanges)
{
	using namespace Libs::VideoOut;
	const int handle = OutputContractHandle();
	ASSERT_GT(handle, 0);
	const auto unmapped = reinterpret_cast<const void*>(uintptr_t{1});
	const auto overflowing = reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max() - kOutputOptionsSize / 2u);
	EXPECT_FALSE(Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(unmapped), kOutputOptionsSize));
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, unmapped, nullptr, 0), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, overflowing, nullptr, 0), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 1, unmapped, nullptr, 0), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutInitializeOutputOptions(const_cast<void*>(overflowing)), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutInitializeOutputOptions(reinterpret_cast<void*>(uintptr_t{1})), VIDEO_OUT_ERROR_INVALID_ADDRESS);
}

TEST(EmulatorVideoOutLifecycle, OutputOptionsRequireReadableAndWritableFullGuestRanges)
{
	using namespace Libs::VideoOut;
	const int handle = OutputContractHandle();
	ASSERT_GT(handle, 0);
	GuestOutputOptionsBuffer options;
	ASSERT_TRUE(options.IsValid());

	const uint64_t page_size = options.PageSize();
	const auto crossing = options.At(page_size - kOutputOptionsSize / 2u);
	std::memset(options.At(page_size - kOutputOptionsSize / 2u), 0x57, kOutputOptionsSize / 2u);
	ASSERT_TRUE(options.ProtectPage(1, VirtualMemory::Mode::NoAccess));
	EXPECT_FALSE(VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(crossing), kOutputOptionsSize));
	EXPECT_EQ(VideoOutIsOutputSupported(handle, 1, crossing, nullptr, 0), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutConfigureOutput(handle, 1, crossing, nullptr, 0), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	EXPECT_EQ(VideoOutInitializeOutputOptions(crossing), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	std::array<uint8_t, kOutputOptionsSize / 2u> before_crossing {};
	ASSERT_TRUE(VirtualMemory::CopyFromGuest(before_crossing.data(), reinterpret_cast<uint64_t>(crossing), before_crossing.size()));
	std::array<uint8_t, kOutputOptionsSize / 2u> unchanged_crossing {};
	unchanged_crossing.fill(0x57);
	EXPECT_EQ(before_crossing, unchanged_crossing);

	GuestOutputOptionsBuffer read_only;
	ASSERT_TRUE(read_only.IsValid());
	std::memset(read_only.Data(), 0x35, kOutputOptionsSize);
	ASSERT_TRUE(read_only.ProtectPage(0, VirtualMemory::Mode::Read));
	EXPECT_TRUE(VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(read_only.Data()), kOutputOptionsSize));
	EXPECT_FALSE(VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(read_only.Data()), kOutputOptionsSize));
	EXPECT_EQ(VideoOutInitializeOutputOptions(read_only.Data()), VIDEO_OUT_ERROR_INVALID_ADDRESS);
	std::array<uint8_t, kOutputOptionsSize> after_initialize {};
	ASSERT_TRUE(VirtualMemory::CopyFromGuest(after_initialize.data(), reinterpret_cast<uint64_t>(read_only.Data()), after_initialize.size()));
	std::array<uint8_t, kOutputOptionsSize> unchanged {};
	unchanged.fill(0x35);
	EXPECT_EQ(after_initialize, unchanged);
}

UT_END();
