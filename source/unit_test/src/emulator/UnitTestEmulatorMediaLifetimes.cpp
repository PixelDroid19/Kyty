#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Audio.h"
#include "Emulator/Audio3d.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GuestTextureLayout.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <string>
#include <thread>

UT_BEGIN(EmulatorMediaLifetimes);

namespace {

namespace LibKernel        = Kyty::Libs::LibKernel;
namespace Graphics         = Kyty::Libs::Graphics;
namespace VideoFrameMemory = Kyty::Emulator::VideoFrameMemory;

constexpr int32_t  AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int32_t>(0x80ea0004u);
constexpr int32_t  AUDIO3D_ERROR_INVALID_PORT      = static_cast<int32_t>(0x80ea0002u);
constexpr int32_t  AUDIO3D_ERROR_OUT_OF_RESOURCES  = static_cast<int32_t>(0x80ea0006u);
constexpr uint64_t kGuestMapSize                   = 0x4000;

void EnsureAudioTestSubsystems()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	if (Config::GetGuestPlatform() == GuestPlatform::Unknown)
	{
		ASSERT_TRUE(Config::SetGuestPlatform(GuestPlatform::Ps4));
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

void EnsureGuestMemorySubsystem()
{
	EnsureAudioTestSubsystems();
	static bool memory_initialized = false;
	if (!memory_initialized)
	{
		Kernel::Memory::MemorySubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		memory_initialized = true;
	}
}

class CoreGuestMemoryBlock
{
public:
	explicit CoreGuestMemoryBlock(uint64_t size): m_address(Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite)) {}

	~CoreGuestMemoryBlock()
	{
		if (m_address != 0)
		{
			(void)Core::VirtualMemory::Free(m_address);
		}
	}

	CoreGuestMemoryBlock(const CoreGuestMemoryBlock&)            = delete;
	CoreGuestMemoryBlock& operator=(const CoreGuestMemoryBlock&) = delete;

	[[nodiscard]] bool IsValid() const { return m_address != 0; }

	template <typename T>
	[[nodiscard]] T* At(uint64_t offset = 0) const
	{
		return reinterpret_cast<T*>(static_cast<uintptr_t>(m_address + offset));
	}

	[[nodiscard]] uint64_t Address() const { return m_address; }

private:
	uint64_t m_address = 0;
};

struct TestDecoderOpaque;
using TestDecoderHandle = TestDecoderOpaque*;
struct TestComputeQueueOpaque;
using TestComputeQueueHandle = TestComputeQueueOpaque*;

struct TestComputeConfigInfo
{
	uint64_t this_size;
	uint16_t compute_pipe_id;
	uint16_t compute_queue_id;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint16_t reserved1;
};

struct TestComputeMemoryInfo
{
	uint64_t this_size;
	uint64_t compute_memory_size;
	void*    compute_memory;
};

struct TestDecoderConfigInfo
{
	uint64_t this_size;
	uint32_t resource_type;
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t  max_frame_width;
	int32_t  max_frame_height;
	int32_t  max_dpb_frame_count;
	uint32_t decode_pipeline_depth;
	void*    compute_queue;
	uint64_t cpu_affinity_mask;
	int32_t  cpu_thread_priority;
	bool     optimize_progressive_video;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint8_t  reserved1;
	void*    extra_config_info;
};

struct TestDecoderMemoryInfo
{
	uint64_t this_size;
	uint64_t cpu_memory_size;
	void*    cpu_memory;
	uint64_t gpu_memory_size;
	void*    gpu_memory;
	uint64_t cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	uint64_t max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};

struct TestInputData
{
	uint64_t this_size;
	void*    au_data;
	uint64_t au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct TestFrameBuffer
{
	uint64_t this_size;
	void*    frame_buffer;
	uint64_t frame_buffer_size;
	bool     is_accepted;
};

struct TestOutputInfo
{
	uint64_t this_size;
	bool     is_valid;
	bool     is_error_frame;
	uint8_t  picture_count;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	void*    frame_buffer;
	uint64_t frame_buffer_size;
};

static_assert(sizeof(TestDecoderConfigInfo) == 0x48 && sizeof(TestDecoderMemoryInfo) == 0x48 && sizeof(TestInputData) == 0x30 &&
              sizeof(TestFrameBuffer) == 0x20 && sizeof(TestOutputInfo) == 0x30 && sizeof(TestComputeConfigInfo) == 0x10 &&
              sizeof(TestComputeMemoryInfo) == 0x18);

bool RegisterTestLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes,
	                         const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity)
{
	return Graphics::GuestTextureLayoutRegisterLinear(base, size, row_pitch_bytes, identity);
}

std::atomic_uint32_t g_rejected_frame_registration_count {0};

bool RejectTestLinearFrame(uint64_t, size_t, uint32_t, const Core::VirtualMemory::GuestMappingIdentitySnapshot&)
{
	g_rejected_frame_registration_count.fetch_add(1, std::memory_order_relaxed);
	return false;
}

void UnregisterTestLinearFrame(uint64_t base)
{
	Graphics::GuestTextureLayoutUnregister(base);
}

void IgnoreTestFrameHostWrite(uint64_t, uint64_t) {}
void PruneTestLinearFrames() { Graphics::GuestTextureLayoutPruneStale(); }

class ScopedTestFrameLayoutCallback
{
public:
	explicit ScopedTestFrameLayoutCallback(uint64_t base, VideoFrameMemory::RegisterLinearFrameCallback register_callback = RegisterTestLinearFrame):
	    m_base(base)
	{
		const VideoFrameMemory::Callbacks callbacks {register_callback, UnregisterTestLinearFrame, IgnoreTestFrameHostWrite,
		                                             nullptr, nullptr, PruneTestLinearFrames};
		m_installed = VideoFrameMemory::InstallCallbacks(callbacks);
	}

	~ScopedTestFrameLayoutCallback()
	{
		if (m_installed)
		{
			VideoFrameMemory::UnregisterFrame(m_base);
			(void)VideoFrameMemory::InstallCallbacks({});
		}
	}

	ScopedTestFrameLayoutCallback(const ScopedTestFrameLayoutCallback&)            = delete;
	ScopedTestFrameLayoutCallback& operator=(const ScopedTestFrameLayoutCallback&) = delete;

	[[nodiscard]] bool IsInstalled() const { return m_installed; }

private:
	uint64_t m_base      = 0;
	bool     m_installed = false;
};

struct TransactionPublicationContext
{
	uint64_t            frame_address        = 0;
	uint64_t            frame_bytes          = 0;
	uint32_t            frame_width          = 0;
	uint32_t            frame_height         = 0;
	uint32_t            frame_pitch          = 0;
	const uint8_t*      frame_data           = nullptr;
	uint64_t            frame_buffer_address = 0;
	TestFrameBuffer     frame_buffer_value {};
	uint64_t            output_address = 0;
	TestOutputInfo      output_value {};
	std::promise<void>* visitor_entered = nullptr;
	std::atomic<bool>*  allow_publish   = nullptr;
	std::atomic<bool>*  frame_written   = nullptr;
};

void PublishTransactionFixture(const Core::VirtualMemory::GuestMappingIdentitySnapshot* frame_identity, void* context) noexcept
{
	auto& publish = *static_cast<TransactionPublicationContext*>(context);
	publish.visitor_entered->set_value();
	while (!publish.allow_publish->load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	if (!Graphics::GuestTextureLayoutRegisterLinear(publish.frame_address, static_cast<size_t>(publish.frame_bytes), publish.frame_pitch,
	                                                *frame_identity))
	{
		return;
	}

	auto*          destination = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(publish.frame_address));
	const uint64_t rows        = static_cast<uint64_t>(publish.frame_height) + (static_cast<uint64_t>(publish.frame_height) + 1u) / 2u;
	for (uint64_t row = 0; row < rows; ++row)
	{
		std::memcpy(destination + row * publish.frame_pitch, publish.frame_data + row * publish.frame_width, publish.frame_width);
	}
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(publish.frame_buffer_address)), &publish.frame_buffer_value,
	            sizeof(publish.frame_buffer_value));
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(publish.output_address)), &publish.output_value,
	            sizeof(publish.output_value));
	publish.frame_written->store(true, std::memory_order_release);
}

template <typename Function>
Function FindVideodec2Function(const Loader::SymbolDatabase& symbols, const char32_t* nid)
{
	const auto* record = symbols.FindByNid(nid, Loader::SymbolType::Func);
	return record == nullptr ? nullptr : reinterpret_cast<Function>(record->vaddr);
}

struct Videodec2Functions
{
	using AllocateComputeQueue = KYTY_SYSV_ABI int (*)(const TestComputeConfigInfo*, const TestComputeMemoryInfo*, TestComputeQueueHandle*);
	using ReleaseComputeQueue  = KYTY_SYSV_ABI int (*)(TestComputeQueueHandle);
	using CreateDecoder        = KYTY_SYSV_ABI int (*)(const TestDecoderConfigInfo*, const TestDecoderMemoryInfo*, TestDecoderHandle*);
	using DeleteDecoder        = KYTY_SYSV_ABI int (*)(TestDecoderHandle);
	using Decode               = KYTY_SYSV_ABI int (*)(TestDecoderHandle, const TestInputData*, TestFrameBuffer*, TestOutputInfo*);
	using Flush                = KYTY_SYSV_ABI int (*)(TestDecoderHandle, TestFrameBuffer*, TestOutputInfo*);
	using Reset                = KYTY_SYSV_ABI int (*)(TestDecoderHandle);

	AllocateComputeQueue allocate_queue = nullptr;
	ReleaseComputeQueue  release_queue  = nullptr;
	CreateDecoder        create         = nullptr;
	DeleteDecoder        del            = nullptr;
	Decode               decode         = nullptr;
	Flush                flush          = nullptr;
	Reset                reset          = nullptr;
};

Videodec2Functions LoadVideodec2Functions()
{
	Loader::SymbolDatabase symbols;
	if (!Libs::Init(U"libVideodec2_1", &symbols))
	{
		return {};
	}
	return {FindVideodec2Function<Videodec2Functions::AllocateComputeQueue>(symbols, U"eD+X2SmxUt4"),
	        FindVideodec2Function<Videodec2Functions::ReleaseComputeQueue>(symbols, U"UvtA3FAiF4Y"),
	        FindVideodec2Function<Videodec2Functions::CreateDecoder>(symbols, U"CNNRoRYd8XI"),
	        FindVideodec2Function<Videodec2Functions::DeleteDecoder>(symbols, U"jwImxXRGSKA"),
	        FindVideodec2Function<Videodec2Functions::Decode>(symbols, U"852F5+q6+iM"),
	        FindVideodec2Function<Videodec2Functions::Flush>(symbols, U"l1hXwscLuCY"),
	        FindVideodec2Function<Videodec2Functions::Reset>(symbols, U"wJXikG6QFN8")};
}

} // namespace

TEST(EmulatorMediaLifetimes, Audio3dRejectsInvalidPortsAndMalformedAttributeInputs)
{
	using namespace Libs::Audio::Audio3d;
	EnsureGuestMemorySubsystem();
	CoreGuestMemoryBlock guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());

	auto* parameters0 = guest.At<Audio3dOpenParameters>(0x000);
	auto* parameters1 = guest.At<Audio3dOpenParameters>(0x100);
	auto* port0       = guest.At<uint32_t>(0x200);
	auto* port1       = guest.At<uint32_t>(0x204);
	*parameters0      = {};
	parameters0->size = sizeof(*parameters0);
	*port0            = std::numeric_limits<uint32_t>::max();
	*port1            = std::numeric_limits<uint32_t>::max();
	ASSERT_EQ(Audio3dInitialize(0), OK);
	parameters0->queue_depth = std::numeric_limits<uint32_t>::max();
	EXPECT_EQ(Audio3dPortOpen(255, parameters0, port0), AUDIO3D_ERROR_INVALID_PARAMETER);
	parameters0->queue_depth = 16u * 1024u * 1024u;
	*port0                   = 0x12345678u;
	EXPECT_EQ(Audio3dPortOpen(255, parameters0, port0), AUDIO3D_ERROR_OUT_OF_RESOURCES);
	EXPECT_EQ(*port0, 0x12345678u);

	uint32_t free_ports = 0;
	for (uint32_t candidate = 0; candidate < 4; ++candidate)
	{
		const int result = Audio3dPortGetQueueLevel(candidate, guest.At<uint32_t>(0x300), guest.At<uint32_t>(0x304));
		if (result == AUDIO3D_ERROR_INVALID_PORT)
		{
			++free_ports;
		} else
		{
			EXPECT_EQ(result, OK);
		}
	}
	if (free_ports < 2)
	{
		GTEST_SKIP() << "fewer than two Audio3d ports are available for the bounded concurrency check";
	}

	*parameters1             = *parameters0;
	parameters0->queue_depth = 1;
	parameters1->queue_depth = 1;
	std::atomic_bool start {false};
	int              open_results[2] = {AUDIO3D_ERROR_INVALID_PARAMETER, AUDIO3D_ERROR_INVALID_PARAMETER};
	std::thread      first_open(
	    [&]
	    {
		    while (!start.load(std::memory_order_acquire))
		    {
			    std::this_thread::yield();
		    }
		    open_results[0] = Audio3dPortOpen(255, parameters0, port0);
	    });
	std::thread second_open(
	    [&]
	    {
		    while (!start.load(std::memory_order_acquire))
		    {
			    std::this_thread::yield();
		    }
		    open_results[1] = Audio3dPortOpen(255, parameters1, port1);
	    });
	start.store(true, std::memory_order_release);
	first_open.join();
	second_open.join();
	ASSERT_EQ(open_results[0], OK);
	ASSERT_EQ(open_results[1], OK);
	EXPECT_NE(*port0, *port1);
	EXPECT_LT(*port0, 4u);
	EXPECT_LT(*port1, 4u);
	Audio3dOpenParameters host_parameters   = *parameters0;
	auto*                 host_parameter_id = guest.At<uint32_t>(0x208);
	*host_parameter_id                      = 0xdecafbad;
	uint32_t host_pointer_result            = std::numeric_limits<uint32_t>::max();
	EXPECT_EQ(Audio3dPortOpen(255, &host_parameters, host_parameter_id), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(*host_parameter_id, 0xdecafbadu);
	float host_attribute = 1.0f;
	EXPECT_EQ(Audio3dPortSetAttribute(*port0, 0x10001, &host_attribute, sizeof(host_attribute)), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(Audio3dPortGetQueueLevel(*port0, &host_pointer_result, nullptr), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(host_pointer_result, std::numeric_limits<uint32_t>::max());
	uint32_t host_capability_count = 7;
	EXPECT_EQ(Audio3dPortGetAttributesSupported(*port0, nullptr, &host_capability_count), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(host_capability_count, 7u);
	Audio3dOpenParameters host_defaults {};
	host_defaults.size = 0x12345678u;
	Audio3dGetDefaultOpenParameters(&host_defaults);
	EXPECT_EQ(host_defaults.size, 0x12345678u);

	float* attribute = guest.At<float>(0x400);
	*attribute       = 1.0f;
	EXPECT_EQ(Audio3dPortSetAttribute(*port0, 0x10001, nullptr, sizeof(*attribute)), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(Audio3dPortSetAttribute(*port0, 0x10001, attribute, 0), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(Audio3dPortSetAttribute(*port0, 0x10001, attribute, sizeof(*attribute) - 1), AUDIO3D_ERROR_INVALID_PARAMETER);

	auto* queue_level     = guest.At<uint32_t>(0x500);
	auto* queue_available = guest.At<uint32_t>(0x504);
	EXPECT_EQ(Audio3dPortGetQueueLevel(*port0, nullptr, nullptr), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(Audio3dPortGetQueueLevel(*port0, queue_level, queue_available), OK);
	EXPECT_EQ(*queue_level, 0u);
	EXPECT_EQ(*queue_available, 1u);

	auto* capability_count = guest.At<uint32_t>(0x600);
	auto* capabilities     = guest.At<uint32_t>(0x604);
	*capability_count      = 0;
	EXPECT_EQ(Audio3dPortGetAttributesSupported(*port0, nullptr, capability_count), OK);
	EXPECT_EQ(*capability_count, 3u);
	*capability_count = 3;
	EXPECT_EQ(Audio3dPortGetAttributesSupported(*port0, capabilities, capability_count), OK);
	EXPECT_EQ(*capability_count, 3u);
	EXPECT_EQ(capabilities[0], 1u);
	EXPECT_EQ(capabilities[1], 3u);
	EXPECT_EQ(capabilities[2], 9u);

	auto* defaults = guest.At<Audio3dOpenParameters>(0x700);
	Audio3dGetDefaultOpenParameters(defaults);
	EXPECT_EQ(defaults->size, 0x20u);

	constexpr uint32_t kInvalidPort = 4;
	EXPECT_EQ(Audio3dPortSetAttribute(kInvalidPort, 0x10001, attribute, sizeof(*attribute)), AUDIO3D_ERROR_INVALID_PORT);
	EXPECT_EQ(Audio3dPortGetQueueLevel(kInvalidPort, queue_level, queue_available), AUDIO3D_ERROR_INVALID_PORT);
	EXPECT_EQ(Audio3dPortAdvance(kInvalidPort), AUDIO3D_ERROR_INVALID_PORT);
	EXPECT_EQ(Audio3dPortPush(kInvalidPort, 1), AUDIO3D_ERROR_INVALID_PORT);

	const uint32_t open_port_id = *port0;
	const uint64_t page_size    = Core::VirtualMemory::GetPageSize();
	ASSERT_NE(page_size, 0u);
	CoreGuestMemoryBlock guarded(page_size * 2u);
	ASSERT_TRUE(guarded.IsValid());
	const uint64_t crossing_offset     = page_size - sizeof(Audio3dOpenParameters) + 8u;
	auto*          crossing_parameters = guarded.At<Audio3dOpenParameters>(crossing_offset);
	*crossing_parameters               = *parameters0;
	auto* crossing_defaults            = guarded.At<Audio3dOpenParameters>(page_size - 0x10u);
	crossing_defaults->size            = 0x12345678u;
	auto*    crossing_attribute        = reinterpret_cast<float*>(static_cast<uintptr_t>(guarded.Address() + page_size - sizeof(uint16_t)));
	uint32_t attribute_sentinel        = 0x5a5a1234u;
	std::memcpy(crossing_attribute, &attribute_sentinel, sizeof(uint16_t));
	auto* crossing_id = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(guarded.Address() + page_size - sizeof(uint16_t)));
	std::memcpy(crossing_id, &attribute_sentinel, sizeof(uint16_t));
	auto* guarded_level      = guarded.At<uint32_t>(0x100);
	*guarded_level           = 0xa5a5a5a5u;
	auto* crossing_available = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(guarded.Address() + page_size - sizeof(uint16_t)));
	std::memcpy(crossing_available, &attribute_sentinel, sizeof(uint16_t));
	auto* guarded_capability_count = guarded.At<uint32_t>(0x200);
	*guarded_capability_count      = 3;
	auto* crossing_capabilities    = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(guarded.Address() + page_size - sizeof(uint16_t)));
	std::memcpy(crossing_capabilities, &attribute_sentinel, sizeof(uint16_t));
	ASSERT_TRUE(Core::VirtualMemory::Protect(guarded.Address() + page_size, page_size, Core::VirtualMemory::Mode::NoAccess));

	EXPECT_EQ(Audio3dPortOpen(255, crossing_parameters, port0), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(*port0, open_port_id);
	EXPECT_EQ(Audio3dPortOpen(255, parameters0, crossing_id), AUDIO3D_ERROR_INVALID_PARAMETER);
	uint16_t unchanged_id_prefix = 0;
	std::memcpy(&unchanged_id_prefix, crossing_id, sizeof(unchanged_id_prefix));
	EXPECT_EQ(unchanged_id_prefix, static_cast<uint16_t>(attribute_sentinel));
	EXPECT_EQ(Audio3dPortSetAttribute(*port0, 0x10001, crossing_attribute, sizeof(uint32_t)), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(Audio3dPortGetQueueLevel(*port0, guarded_level, crossing_available), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(*guarded_level, 0xa5a5a5a5u);
	uint16_t unchanged_available_prefix = 0;
	std::memcpy(&unchanged_available_prefix, crossing_available, sizeof(unchanged_available_prefix));
	EXPECT_EQ(unchanged_available_prefix, static_cast<uint16_t>(attribute_sentinel));
	EXPECT_EQ(Audio3dPortGetAttributesSupported(*port0, crossing_capabilities, guarded_capability_count), AUDIO3D_ERROR_INVALID_PARAMETER);
	EXPECT_EQ(*guarded_capability_count, 3u);
	uint16_t unchanged_capability_prefix = 0;
	std::memcpy(&unchanged_capability_prefix, crossing_capabilities, sizeof(unchanged_capability_prefix));
	EXPECT_EQ(unchanged_capability_prefix, static_cast<uint16_t>(attribute_sentinel));
	Audio3dGetDefaultOpenParameters(crossing_defaults);
	EXPECT_EQ(crossing_defaults->size, 0x12345678u);

	ASSERT_TRUE(Core::VirtualMemory::Protect(guarded.Address() + page_size, page_size, Core::VirtualMemory::Mode::ReadWrite));
}

TEST(EmulatorMediaLifetimes, Videodec2RejectsForgedAndStaleComputeQueueHandles)
{
	EnsureGuestMemorySubsystem();
	const auto functions = LoadVideodec2Functions();
	ASSERT_NE(functions.allocate_queue, nullptr);
	ASSERT_NE(functions.release_queue, nullptr);

	CoreGuestMemoryBlock guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());
	auto* config       = guest.At<TestComputeConfigInfo>(0x000);
	*config            = {};
	config->this_size  = sizeof(*config);
	auto* memory       = guest.At<TestComputeMemoryInfo>(0x100);
	*memory            = {};
	memory->this_size  = sizeof(*memory);
	auto* queue_output = guest.At<TestComputeQueueHandle>(0x200);
	*queue_output      = nullptr;

	const auto forged = reinterpret_cast<TestComputeQueueHandle>(static_cast<uintptr_t>(0x10001));
	EXPECT_EQ(functions.release_queue(forged), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.release_queue(nullptr), LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(functions.allocate_queue(config, memory, queue_output), OK);
	ASSERT_NE(*queue_output, nullptr);
	const auto first = *queue_output;
	ASSERT_EQ(functions.release_queue(first), OK);
	EXPECT_EQ(functions.release_queue(first), LibKernel::KERNEL_ERROR_EINVAL);

	*queue_output = nullptr;
	ASSERT_EQ(functions.allocate_queue(config, memory, queue_output), OK);
	ASSERT_NE(*queue_output, nullptr);
	EXPECT_NE(*queue_output, first);
	EXPECT_EQ(functions.release_queue(first), LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(functions.release_queue(*queue_output), OK);
}

TEST(EmulatorMediaLifetimes, Videodec2AcceptsCoreGuestRangesAndRejectsGuardCrossings)
{
	EnsureGuestMemorySubsystem();
	const auto functions = LoadVideodec2Functions();
	ASSERT_NE(functions.allocate_queue, nullptr);
	ASSERT_NE(functions.release_queue, nullptr);

	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	ASSERT_NE(page_size, 0u);
	CoreGuestMemoryBlock guest(page_size * 2u);
	ASSERT_TRUE(guest.IsValid());

	auto* config       = guest.At<TestComputeConfigInfo>();
	*config            = {};
	config->this_size  = sizeof(*config);
	auto* memory       = guest.At<TestComputeMemoryInfo>(0x100);
	*memory            = {};
	memory->this_size  = sizeof(*memory);
	auto* queue_output = guest.At<TestComputeQueueHandle>(0x200);
	*queue_output      = nullptr;

	ASSERT_TRUE(Core::VirtualMemory::IsRangeReadable(guest.Address(), sizeof(*config)));
	ASSERT_TRUE(Core::VirtualMemory::IsRangeWritable(guest.Address() + 0x200, sizeof(*queue_output)));
	ASSERT_EQ(functions.allocate_queue(config, memory, queue_output), OK);
	ASSERT_NE(*queue_output, nullptr);
	ASSERT_EQ(functions.release_queue(*queue_output), OK);

	auto* crossing_config      = guest.At<TestComputeConfigInfo>(page_size - sizeof(uint64_t));
	*crossing_config           = {};
	crossing_config->this_size = sizeof(*crossing_config);

	ASSERT_TRUE(Core::VirtualMemory::Protect(guest.Address() + page_size, page_size, Core::VirtualMemory::Mode::Read));
	EXPECT_TRUE(Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(crossing_config), sizeof(*crossing_config)));
	ASSERT_EQ(functions.allocate_queue(crossing_config, memory, queue_output), OK);
	ASSERT_NE(*queue_output, nullptr);
	ASSERT_EQ(functions.release_queue(*queue_output), OK);

	ASSERT_TRUE(Core::VirtualMemory::Protect(guest.Address() + page_size, page_size, Core::VirtualMemory::Mode::NoAccess));
	EXPECT_FALSE(Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(crossing_config), sizeof(*crossing_config)));
	EXPECT_EQ(functions.allocate_queue(crossing_config, memory, queue_output), LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorMediaLifetimes, Videodec2PublicationTransactionBlocksConcurrentRemap)
{
	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	ASSERT_NE(page_size, 0u);
	CoreGuestMemoryBlock frame_guest(page_size);
	CoreGuestMemoryBlock metadata_guest(page_size);
	ASSERT_TRUE(frame_guest.IsValid());
	ASSERT_TRUE(metadata_guest.IsValid());

	constexpr uint32_t frame_width  = 8;
	constexpr uint32_t frame_height = 4;
	constexpr uint32_t frame_pitch  = 16;
	constexpr uint64_t frame_bytes  = frame_pitch * (frame_height + (frame_height + 1u) / 2u);
	std::array<uint8_t, frame_width*(frame_height + (frame_height + 1u) / 2u)> source {};
	for (size_t i = 0; i < source.size(); ++i)
	{
		source[i] = static_cast<uint8_t>(i + 1u);
	}

	const uint64_t  frame_address      = frame_guest.Address();
	const uint64_t  frame_info_address = metadata_guest.Address() + 0x100u;
	const uint64_t  output_address     = metadata_guest.Address() + 0x200u;
	TestFrameBuffer frame_info {};
	frame_info.this_size         = sizeof(frame_info);
	frame_info.frame_buffer      = reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address));
	frame_info.frame_buffer_size = page_size;
	frame_info.is_accepted       = true;
	TestOutputInfo output_info {};
	output_info.this_size         = sizeof(output_info);
	output_info.is_valid          = true;
	output_info.picture_count     = 1;
	output_info.frame_width       = frame_width;
	output_info.frame_pitch       = frame_pitch;
	output_info.frame_height      = frame_height;
	output_info.frame_buffer      = reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address));
	output_info.frame_buffer_size = frame_bytes;

	Core::VirtualMemory::GuestMappingSnapshot frame_info_mapping {};
	Core::VirtualMemory::GuestMappingSnapshot output_mapping {};
	ASSERT_TRUE(Core::VirtualMemory::CaptureGuestMappingSnapshot(frame_info_address, sizeof(frame_info), &frame_info_mapping));
	ASSERT_TRUE(Core::VirtualMemory::CaptureGuestMappingSnapshot(output_address, sizeof(output_info), &output_mapping));

	const Core::VirtualMemory::WritableGuestTransactionRange ranges[] = {
	    {frame_address, page_size, nullptr},
	    {frame_address, frame_bytes, nullptr},
	    {frame_info_address, sizeof(frame_info), &frame_info_mapping},
	    {output_address, sizeof(output_info), &output_mapping},
	};
	std::promise<void>            visitor_entered_promise;
	auto                          visitor_entered = visitor_entered_promise.get_future();
	std::atomic<bool>             allow_publish {false};
	std::atomic<bool>             frame_written {false};
	TransactionPublicationContext context {frame_address,
	                                       frame_bytes,
	                                       frame_width,
	                                       frame_height,
	                                       frame_pitch,
	                                       source.data(),
	                                       frame_info_address,
	                                       frame_info,
	                                       output_address,
	                                       output_info,
	                                       &visitor_entered_promise,
	                                       &allow_publish,
	                                       &frame_written};
	bool                          transaction_succeeded = false;
	std::thread                   publisher(
	    [&]
	    { transaction_succeeded = Core::VirtualMemory::VisitWritableGuestTransaction(ranges, 4, 1, PublishTransactionFixture, &context); });
	if (visitor_entered.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
	{
		allow_publish.store(true, std::memory_order_release);
		publisher.join();
		ADD_FAILURE() << "the writable guest transaction did not enter its publication callback";
		return;
	}

	std::promise<void> remap_started_promise;
	auto               remap_started = remap_started_promise.get_future();
	std::promise<void> remap_finished_promise;
	auto               remap_finished  = remap_finished_promise.get_future();
	bool               remap_succeeded = false;
	std::thread        remapper(
	    [&]
	    {
		    remap_started_promise.set_value();
		    remap_succeeded = Core::VirtualMemory::FreeRange(frame_address, page_size) &&
		                      Core::VirtualMemory::AllocFixed(frame_address, page_size, Core::VirtualMemory::Mode::ReadWrite);
		    remap_finished_promise.set_value();
	    });
	if (remap_started.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
	{
		allow_publish.store(true, std::memory_order_release);
		publisher.join();
		remapper.join();
		ADD_FAILURE() << "the concurrent remapper did not start";
		return;
	}
	EXPECT_EQ(remap_finished.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	allow_publish.store(true, std::memory_order_release);
	publisher.join();
	remapper.join();

	EXPECT_TRUE(transaction_succeeded);
	EXPECT_TRUE(frame_written.load(std::memory_order_acquire));
	if (!remap_succeeded)
	{
		GTEST_SKIP() << "the host does not support replacing a Core guest mapping at its original address";
	}

	TestFrameBuffer actual_frame_info {};
	TestOutputInfo  actual_output_info {};
	ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(&actual_frame_info, frame_info_address, sizeof(actual_frame_info)));
	ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(&actual_output_info, output_address, sizeof(actual_output_info)));
	EXPECT_TRUE(actual_frame_info.is_accepted);
	EXPECT_TRUE(actual_output_info.is_valid);
	EXPECT_EQ(actual_output_info.frame_buffer, reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address)));
	EXPECT_EQ(actual_output_info.frame_buffer_size, frame_bytes);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_address, frame_pitch), 0u);
}

TEST(EmulatorMediaLifetimes, Videodec2PublishesFramesOnlyAfterLayoutAdmission)
{
	EnsureGuestMemorySubsystem();
	if (!Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "video decoder backend is unavailable";
	}
	const auto functions = LoadVideodec2Functions();
	ASSERT_NE(functions.create, nullptr);
	ASSERT_NE(functions.del, nullptr);
	ASSERT_NE(functions.decode, nullptr);
	ASSERT_NE(functions.flush, nullptr);
	ASSERT_NE(functions.reset, nullptr);

	// Generated constant-color Annex B fixture also used by the backend bounds test.
	static constexpr uint8_t avc_fixture[] = {
	    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdc, 0x97, 0xe2, 0x30, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00,
	    0x03, 0x00, 0x02, 0x8f, 0x12, 0x27, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x00, 0x01, 0x65,
	    0x88, 0x84, 0x3a, 0x11, 0x8a, 0x00, 0x02, 0x19, 0x71, 0xc0, 0x00, 0x40, 0x32, 0x38, 0x00, 0x08, 0x03, 0xc9, 0xd7, 0x5e,
	};
	std::string backend_error;
	auto        reference_decoder =
	    Emulator::AudioVideoBackend::ElementaryVideoDecoder::Open(Emulator::AudioVideoBackend::VideoCodec::Avc, 64, 64, &backend_error);
	ASSERT_NE(reference_decoder, nullptr) << backend_error;
	ASSERT_TRUE(reference_decoder->Send(avc_fixture, sizeof(avc_fixture), 23)) << reference_decoder->LastError();
	ASSERT_TRUE(reference_decoder->Drain()) << reference_decoder->LastError();
	Emulator::AudioVideoBackend::VideoFrame expected_frame;
	int64_t                                 expected_tag = 0;
	ASSERT_TRUE(reference_decoder->Receive(&expected_frame, &expected_tag));
	ASSERT_EQ(expected_frame.width, 32u);
	ASSERT_EQ(expected_frame.height, 18u);
	ASSERT_EQ(expected_frame.data.size(), static_cast<size_t>(expected_frame.width) * expected_frame.height * 3u / 2u);

	CoreGuestMemoryBlock guest(kGuestMapSize);
	CoreGuestMemoryBlock frame_guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());
	ASSERT_TRUE(frame_guest.IsValid());
	auto* config                    = guest.At<TestDecoderConfigInfo>(0x000);
	*config                         = {};
	config->this_size               = sizeof(*config);
	config->codec_type              = 1;
	config->max_frame_width         = 64;
	config->max_frame_height        = 64;
	auto* memory                    = guest.At<TestDecoderMemoryInfo>(0x100);
	*memory                         = {};
	memory->this_size               = sizeof(*memory);
	auto* decoder_output            = guest.At<TestDecoderHandle>(0x200);
	*decoder_output                 = nullptr;
	const int         create_result = functions.create(config, memory, decoder_output);
	TestDecoderHandle handle        = *decoder_output;
	EXPECT_EQ(create_result, OK);
	if (create_result != OK || handle == nullptr)
	{
		ADD_FAILURE() << "Videodec2 could not create the fixture decoder";
		return;
	}

	const uint64_t packet_address = guest.Address() + 0x1000u;
	EXPECT_TRUE(Core::VirtualMemory::CopyToGuest(packet_address, avc_fixture, sizeof(avc_fixture)));
	auto* input                     = guest.At<TestInputData>(0x300);
	*input                          = {};
	input->this_size                = sizeof(*input);
	input->au_data                  = reinterpret_cast<void*>(static_cast<uintptr_t>(packet_address));
	input->au_size                  = sizeof(avc_fixture);
	const uint64_t frame_address    = frame_guest.Address();
	auto*          frame_buffer     = guest.At<TestFrameBuffer>(0x400);
	*frame_buffer                   = {};
	frame_buffer->this_size         = sizeof(*frame_buffer);
	frame_buffer->frame_buffer      = reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address));
	frame_buffer->frame_buffer_size = kGuestMapSize;
	auto* output                    = guest.At<TestOutputInfo>(0x500);
	*output                         = {};
	output->this_size               = sizeof(*output);
	std::array<uint8_t, kGuestMapSize> sentinel {};
	sentinel.fill(0xa5);
	EXPECT_TRUE(Core::VirtualMemory::CopyToGuest(frame_address, sentinel.data(), sentinel.size()));

	ScopedTestFrameLayoutCallback layout_callback(frame_address);
	if (!layout_callback.IsInstalled())
	{
		ADD_FAILURE() << "could not install the frame-layout test callback";
		EXPECT_EQ(functions.del(handle), OK);
		return;
	}
	int decode_result = functions.decode(handle, input, frame_buffer, output);
	EXPECT_EQ(decode_result, OK);
	TestOutputInfo published_output {};
	if (decode_result == OK &&
	    Core::VirtualMemory::CopyFromGuest(&published_output, reinterpret_cast<uint64_t>(output), sizeof(published_output)) &&
	    !published_output.is_valid)
	{
		EXPECT_EQ(functions.flush(handle, frame_buffer, output), OK);
	}
	const bool output_read =
	    Core::VirtualMemory::CopyFromGuest(&published_output, reinterpret_cast<uint64_t>(output), sizeof(published_output));
	EXPECT_TRUE(output_read);
	TestFrameBuffer published_frame_buffer {};
	const bool frame_buffer_read = Core::VirtualMemory::CopyFromGuest(&published_frame_buffer, reinterpret_cast<uint64_t>(frame_buffer),
	                                                                  sizeof(published_frame_buffer));
	EXPECT_TRUE(frame_buffer_read);
	if (output_read && frame_buffer_read)
	{
		EXPECT_TRUE(published_output.is_valid);
		EXPECT_TRUE(published_frame_buffer.is_accepted);
		EXPECT_EQ(published_output.picture_count, 1u);
		EXPECT_EQ(published_output.codec_type, 1u);
		EXPECT_EQ(published_output.frame_width, expected_frame.width);
		EXPECT_EQ(published_output.frame_height, expected_frame.height);
		EXPECT_EQ(published_output.frame_pitch, 256u);
		EXPECT_EQ(published_output.frame_buffer, reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address)));
		EXPECT_EQ(published_output.frame_buffer_size, 256u * (18u + 9u));
		EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_address, 256u), 256u);
		for (uint32_t row = 0; row < expected_frame.height + (expected_frame.height + 1u) / 2u; ++row)
		{
			std::array<uint8_t, 32> actual_row {};
			const bool row_read = Core::VirtualMemory::CopyFromGuest(actual_row.data(), frame_address + row * 256u, actual_row.size());
			EXPECT_TRUE(row_read);
			if (row_read)
			{
				const auto expected_begin = expected_frame.data.begin() + static_cast<size_t>(row) * expected_frame.width;
				EXPECT_TRUE(std::equal(actual_row.begin(), actual_row.end(), expected_begin));
			}
		}
		uint8_t padding = 0;
		EXPECT_TRUE(Core::VirtualMemory::CopyFromGuest(&padding, frame_address + expected_frame.width, sizeof(padding)));
		EXPECT_EQ(padding, 0xa5u);
	}

	const VideoFrameMemory::Callbacks reject_layout_callbacks {RejectTestLinearFrame, UnregisterTestLinearFrame,
	                                                           IgnoreTestFrameHostWrite, nullptr, nullptr, PruneTestLinearFrames};
	g_rejected_frame_registration_count.store(0, std::memory_order_relaxed);
	ASSERT_TRUE(VideoFrameMemory::InstallCallbacks(reject_layout_callbacks));
	ASSERT_EQ(functions.reset(handle), OK);

	std::array<uint8_t, kGuestMapSize> rejected_frame_sentinel {};
	rejected_frame_sentinel.fill(0xa5u);
	ASSERT_TRUE(Core::VirtualMemory::CopyToGuest(frame_address, rejected_frame_sentinel.data(), rejected_frame_sentinel.size()));
	TestFrameBuffer rejected_frame_buffer {};
	rejected_frame_buffer.this_size         = sizeof(rejected_frame_buffer);
	rejected_frame_buffer.frame_buffer      = reinterpret_cast<void*>(static_cast<uintptr_t>(frame_address));
	rejected_frame_buffer.frame_buffer_size = kGuestMapSize;
	rejected_frame_buffer.is_accepted       = true;
	TestOutputInfo rejected_output {};
	rejected_output.this_size       = sizeof(rejected_output);
	rejected_output.is_valid        = true;
	rejected_output.is_error_frame  = true;
	rejected_output.picture_count   = 0x7fu;
	rejected_output.codec_type      = 0x10203040u;
	rejected_output.frame_width     = 0x50607080u;
	rejected_output.frame_pitch     = 0x90a0b0c0u;
	rejected_output.frame_height    = 0xd0e0f001u;
	rejected_output.frame_buffer    = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12340u));
	rejected_output.frame_buffer_size = 0x23456u;
	*frame_buffer = rejected_frame_buffer;
	*output       = rejected_output;

	int rejection_result = functions.decode(handle, input, frame_buffer, output);
	if (rejection_result == OK)
	{
		TestOutputInfo after_decode {};
		ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(&after_decode, reinterpret_cast<uint64_t>(output), sizeof(after_decode)));
		if (!after_decode.is_valid)
		{
			ASSERT_TRUE(Core::VirtualMemory::CopyToGuest(frame_address, rejected_frame_sentinel.data(), rejected_frame_sentinel.size()));
			*frame_buffer = rejected_frame_buffer;
			*output       = rejected_output;
			rejection_result = functions.flush(handle, frame_buffer, output);
		}
	}
	EXPECT_EQ(rejection_result, LibKernel::KERNEL_ERROR_ENOMEM);
	EXPECT_GT(g_rejected_frame_registration_count.load(std::memory_order_relaxed), 0u);
	TestFrameBuffer actual_rejected_frame_buffer {};
	TestOutputInfo  actual_rejected_output {};
	std::array<uint8_t, kGuestMapSize> actual_rejected_frame_bytes {};
	ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(&actual_rejected_frame_buffer, reinterpret_cast<uint64_t>(frame_buffer),
	                                               sizeof(actual_rejected_frame_buffer)));
	ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(&actual_rejected_output, reinterpret_cast<uint64_t>(output),
	                                               sizeof(actual_rejected_output)));
	ASSERT_TRUE(Core::VirtualMemory::CopyFromGuest(actual_rejected_frame_bytes.data(), frame_address,
	                                               actual_rejected_frame_bytes.size()));
	EXPECT_EQ(actual_rejected_frame_buffer.this_size, rejected_frame_buffer.this_size);
	EXPECT_EQ(actual_rejected_frame_buffer.frame_buffer, rejected_frame_buffer.frame_buffer);
	EXPECT_EQ(actual_rejected_frame_buffer.frame_buffer_size, rejected_frame_buffer.frame_buffer_size);
	EXPECT_EQ(actual_rejected_frame_buffer.is_accepted, rejected_frame_buffer.is_accepted);
	EXPECT_EQ(actual_rejected_output.this_size, rejected_output.this_size);
	EXPECT_EQ(actual_rejected_output.is_valid, rejected_output.is_valid);
	EXPECT_EQ(actual_rejected_output.is_error_frame, rejected_output.is_error_frame);
	EXPECT_EQ(actual_rejected_output.picture_count, rejected_output.picture_count);
	EXPECT_EQ(actual_rejected_output.codec_type, rejected_output.codec_type);
	EXPECT_EQ(actual_rejected_output.frame_width, rejected_output.frame_width);
	EXPECT_EQ(actual_rejected_output.frame_pitch, rejected_output.frame_pitch);
	EXPECT_EQ(actual_rejected_output.frame_height, rejected_output.frame_height);
	EXPECT_EQ(actual_rejected_output.frame_buffer, rejected_output.frame_buffer);
	EXPECT_EQ(actual_rejected_output.frame_buffer_size, rejected_output.frame_buffer_size);
	EXPECT_EQ(actual_rejected_frame_bytes, rejected_frame_sentinel);

	EXPECT_EQ(functions.del(handle), OK);
}

TEST(EmulatorMediaLifetimes, GuestTextureLayoutCapsUniqueBasesAndReclaimsStaleMappings)
{
	EnsureGuestMemorySubsystem();
	ScopedTestFrameLayoutCallback layout_callback(0);
	ASSERT_TRUE(layout_callback.IsInstalled());

	constexpr size_t   records_per_owner = Graphics::kGuestTextureLayoutCapacity / 2u;
	constexpr size_t   frame_size        = 64u;
	constexpr uint32_t row_pitch         = 64u;
	constexpr size_t   frame_stride      = 128u;
	const uint64_t     owner_size        = static_cast<uint64_t>(records_per_owner) * frame_stride;

	struct GuestMapping
	{
		uint64_t address = 0;
		explicit GuestMapping(uint64_t size): address(Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite)) {}
		~GuestMapping()
		{
			if (address != 0)
			{
				(void)Core::VirtualMemory::Free(address);
			}
		}
		bool Release()
		{
			if (address == 0 || !Core::VirtualMemory::Free(address))
			{
				return false;
			}
			address = 0;
			return true;
		}
	};

	struct LayoutCleanup
	{
		uint64_t bases[5]      = {};
		size_t   registered[5] = {};
		size_t   owners        = 0;

		~LayoutCleanup()
		{
			for (size_t owner = 0; owner < owners; ++owner)
			{
				for (size_t record = 0; record < registered[owner]; ++record)
				{
					Graphics::GuestTextureLayoutUnregister(bases[owner] + record * 128u);
				}
			}
		}

		size_t AddOwner(uint64_t base)
		{
			const size_t index = owners++;
			bases[index]      = base;
			return index;
		}
	};

	GuestMapping owner_a(owner_size);
	GuestMapping owner_b(owner_size + frame_stride);
	GuestMapping owner_c(owner_size);
	GuestMapping owner_d(owner_size);
	GuestMapping owner_e(owner_size);
	ASSERT_NE(owner_a.address, 0u);
	ASSERT_NE(owner_b.address, 0u);
	ASSERT_NE(owner_c.address, 0u);
	ASSERT_NE(owner_d.address, 0u);
	ASSERT_NE(owner_e.address, 0u);

	const uint64_t owner_a_base = owner_a.address;
	LayoutCleanup cleanup;
	const size_t owner_a_cleanup = cleanup.AddOwner(owner_a.address);
	const size_t owner_b_cleanup = cleanup.AddOwner(owner_b.address);
	const size_t owner_c_cleanup = cleanup.AddOwner(owner_c.address);
	const size_t owner_d_cleanup = cleanup.AddOwner(owner_d.address);
	const size_t owner_e_cleanup = cleanup.AddOwner(owner_e.address);
	auto        register_records = [&](uint64_t base, size_t owner, size_t count) {
		for (size_t record = 0; record < count; ++record)
		{
			const uint64_t frame_base = base + record * frame_stride;
			if (!VideoFrameMemory::RegisterLinearFrame(frame_base, frame_size, static_cast<uint32_t>(row_pitch)))
			{
				return false;
			}
			++cleanup.registered[owner];
		}
		return true;
	};

	Core::VirtualMemory::GuestMappingIdentitySnapshot old_identity {};
	ASSERT_TRUE(Core::VirtualMemory::CaptureGuestMappingIdentitySnapshot(owner_a_base, frame_size, &old_identity));
	ASSERT_TRUE(register_records(owner_a_base, owner_a_cleanup, records_per_owner));
	ASSERT_TRUE(register_records(owner_b.address, owner_b_cleanup, records_per_owner));
	EXPECT_TRUE(VideoFrameMemory::RegisterLinearFrame(owner_a_base, frame_size, 32u));
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_a_base, 32u), 32u);
	EXPECT_TRUE(VideoFrameMemory::RegisterLinearFrame(owner_a_base, frame_size, static_cast<uint32_t>(row_pitch)));
	EXPECT_FALSE(VideoFrameMemory::RegisterLinearFrame(owner_b.address + records_per_owner * frame_stride, frame_size,
	                                                   static_cast<uint32_t>(row_pitch)));
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_a_base, static_cast<uint32_t>(row_pitch)), row_pitch);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_b.address, static_cast<uint32_t>(row_pitch)), row_pitch);

	ASSERT_TRUE(owner_a.Release());
	VideoFrameMemory::PruneStaleLayouts();
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_a_base, static_cast<uint32_t>(row_pitch)), 0u);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_b.address, static_cast<uint32_t>(row_pitch)), row_pitch);
	Core::VirtualMemory::GuestMappingIdentitySnapshot new_identity {};
	ASSERT_TRUE(Core::VirtualMemory::CaptureGuestMappingIdentitySnapshot(owner_c.address, frame_size, &new_identity));
	ASSERT_NE(old_identity.segments[0].identity, new_identity.segments[0].identity);
	ASSERT_TRUE(register_records(owner_c.address, owner_c_cleanup, records_per_owner));
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_c.address, static_cast<uint32_t>(row_pitch)), row_pitch);

	ASSERT_TRUE(owner_b.Release());
	ASSERT_TRUE(owner_c.Release());
	VideoFrameMemory::PruneStaleLayouts();
	ASSERT_TRUE(register_records(owner_d.address, owner_d_cleanup, records_per_owner));
	ASSERT_TRUE(register_records(owner_e.address, owner_e_cleanup, records_per_owner));
	VideoFrameMemory::PruneStaleLayouts();
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_d.address, static_cast<uint32_t>(row_pitch)), row_pitch);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(owner_e.address + (records_per_owner - 1u) * frame_stride,
	                                                        static_cast<uint32_t>(row_pitch)),
	          row_pitch);
}

TEST(EmulatorMediaLifetimes, GuestTextureLayoutRangeInvalidationRemovesWholeOverlaps)
{
	EnsureGuestMemorySubsystem();
	CoreGuestMemoryBlock guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());

	constexpr uint64_t frame_size = 0x100u;
	constexpr uint32_t row_pitch  = 64u;
	const uint64_t     frame_a    = guest.Address() + 0x100u;
	const uint64_t     frame_b    = guest.Address() + 0x200u;
	const uint64_t     frame_c    = guest.Address() + 0x400u;

	struct LayoutCleanup
	{
		std::array<uint64_t, 3> bases {};
		size_t                  count = 0;

		~LayoutCleanup()
		{
			for (size_t i = 0; i < count; ++i)
			{
				Graphics::GuestTextureLayoutUnregister(bases[i]);
			}
		}

		void Add(uint64_t base) { bases[count++] = base; }
	};

	LayoutCleanup cleanup;
	auto register_layout = [&](uint64_t base) {
		Core::VirtualMemory::GuestMappingIdentitySnapshot identity {};
		if (!Core::VirtualMemory::CaptureGuestMappingIdentitySnapshot(base, frame_size, &identity) ||
		    !Graphics::GuestTextureLayoutRegisterLinear(base, static_cast<size_t>(frame_size), row_pitch, identity))
		{
			return false;
		}
		cleanup.Add(base);
		return true;
	};

	ASSERT_TRUE(register_layout(frame_a));
	ASSERT_TRUE(register_layout(frame_b));
	ASSERT_TRUE(register_layout(frame_c));
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_a, row_pitch), row_pitch);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_b, row_pitch), row_pitch);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_c, row_pitch), row_pitch);

	Graphics::GuestTextureLayoutInvalidateRange(frame_a, frame_b + frame_size / 2u - frame_a);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_a, row_pitch), 0u);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_b, row_pitch), 0u);

	Graphics::GuestTextureLayoutInvalidateRange(frame_c + frame_size, 16u);
	Graphics::GuestTextureLayoutInvalidateRange(0, 1u);
	Graphics::GuestTextureLayoutInvalidateRange(frame_c, 0);
	Graphics::GuestTextureLayoutInvalidateRange(UINT64_MAX - 4u, 8u);
	EXPECT_EQ(Graphics::GuestTextureLayoutGetLinearRowPitch(frame_c, row_pitch), row_pitch);
}

TEST(EmulatorMediaLifetimes, Videodec2RejectsForgedDecoderHandles)
{
	EnsureGuestMemorySubsystem();
	const auto functions = LoadVideodec2Functions();
	ASSERT_NE(functions.del, nullptr);
	ASSERT_NE(functions.decode, nullptr);
	ASSERT_NE(functions.flush, nullptr);
	ASSERT_NE(functions.reset, nullptr);
	CoreGuestMemoryBlock guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());
	auto* input             = guest.At<TestInputData>(0x000);
	*input                  = {};
	input->this_size        = sizeof(*input);
	auto* frame_buffer      = guest.At<TestFrameBuffer>(0x100);
	*frame_buffer           = {};
	frame_buffer->this_size = sizeof(*frame_buffer);
	auto* output            = guest.At<TestOutputInfo>(0x200);
	*output                 = {};
	output->this_size       = sizeof(*output);

	const auto forged = reinterpret_cast<TestDecoderHandle>(static_cast<uintptr_t>(0x10001));
	EXPECT_EQ(functions.del(forged), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.reset(forged), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.decode(forged, input, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.flush(forged, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorMediaLifetimes, Videodec2RetiresDeletedHandlesAndValidatesAccessUnitRanges)
{
	EnsureGuestMemorySubsystem();
	if (!Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "video decoder backend is unavailable";
	}
	const auto functions = LoadVideodec2Functions();
	ASSERT_NE(functions.allocate_queue, nullptr);
	ASSERT_NE(functions.release_queue, nullptr);
	ASSERT_NE(functions.create, nullptr);
	ASSERT_NE(functions.del, nullptr);
	ASSERT_NE(functions.decode, nullptr);
	ASSERT_NE(functions.reset, nullptr);

	CoreGuestMemoryBlock guest(kGuestMapSize);
	ASSERT_TRUE(guest.IsValid());

	auto* config               = guest.At<TestDecoderConfigInfo>(0x000);
	*config                    = {};
	config->this_size          = sizeof(*config);
	config->codec_type         = 1;
	config->max_frame_width    = 64;
	config->max_frame_height   = 64;
	auto* memory               = guest.At<TestDecoderMemoryInfo>(0x100);
	*memory                    = {};
	memory->this_size          = sizeof(*memory);
	auto* decoder_output       = guest.At<TestDecoderHandle>(0x200);
	*decoder_output            = nullptr;
	auto* compute_config       = guest.At<TestComputeConfigInfo>(0x600);
	*compute_config            = {};
	compute_config->this_size  = sizeof(*compute_config);
	auto* compute_memory       = guest.At<TestComputeMemoryInfo>(0x700);
	*compute_memory            = {};
	compute_memory->this_size  = sizeof(*compute_memory);
	auto* compute_queue_output = guest.At<TestComputeQueueHandle>(0x800);
	*compute_queue_output      = nullptr;
	ASSERT_EQ(functions.allocate_queue(compute_config, compute_memory, compute_queue_output), OK);
	config->compute_queue = *compute_queue_output;

	const int  create_result = functions.create(config, memory, decoder_output);
	const auto queue_handle  = *compute_queue_output;
	EXPECT_EQ(functions.del(reinterpret_cast<TestDecoderHandle>(queue_handle)), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.reset(reinterpret_cast<TestDecoderHandle>(queue_handle)), LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(functions.release_queue(queue_handle), OK);
	*compute_queue_output = nullptr;
	ASSERT_EQ(create_result, OK);
	ASSERT_NE(*decoder_output, nullptr);
	const auto handle = *decoder_output;
	EXPECT_NE(reinterpret_cast<uintptr_t>(handle), reinterpret_cast<uintptr_t>(queue_handle));
	EXPECT_EQ(functions.release_queue(reinterpret_cast<TestComputeQueueHandle>(handle)), LibKernel::KERNEL_ERROR_EINVAL);
	// CreateDecoder does not retain or dereference compute_queue; queue lifetime is independent.

	auto* input                     = guest.At<TestInputData>(0x300);
	*input                          = {};
	input->this_size                = sizeof(*input);
	input->au_data                  = reinterpret_cast<void*>(guest.Address() + kGuestMapSize - 1u);
	input->au_size                  = 2;
	auto* frame_buffer              = guest.At<TestFrameBuffer>(0x400);
	*frame_buffer                   = {};
	frame_buffer->this_size         = sizeof(*frame_buffer);
	frame_buffer->frame_buffer      = nullptr;
	frame_buffer->frame_buffer_size = 0;
	auto* output                    = guest.At<TestOutputInfo>(0x500);
	*output                         = {};
	output->this_size               = sizeof(*output);
	EXPECT_EQ(functions.decode(handle, input, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);

	auto* partial_input      = guest.At<TestInputData>(kGuestMapSize - sizeof(uint64_t));
	partial_input->this_size = sizeof(*partial_input);
	EXPECT_EQ(functions.decode(handle, partial_input, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);

	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	ASSERT_NE(page_size, 0u);
	CoreGuestMemoryBlock guarded(page_size * 2u);
	ASSERT_TRUE(guarded.IsValid());
	auto* guarded_frame        = guarded.At<TestFrameBuffer>(0x100);
	*guarded_frame             = {};
	guarded_frame->this_size   = sizeof(*guarded_frame);
	auto* crossing_frame       = guarded.At<TestFrameBuffer>(page_size - sizeof(uint64_t));
	*crossing_frame            = {};
	crossing_frame->this_size  = sizeof(*crossing_frame);
	auto* crossing_output      = guarded.At<TestOutputInfo>(page_size - sizeof(uint64_t));
	*crossing_output           = {};
	crossing_output->this_size = sizeof(*crossing_output);
	ASSERT_TRUE(Core::VirtualMemory::Protect(guarded.Address() + page_size, page_size, Core::VirtualMemory::Mode::NoAccess));
	EXPECT_EQ(functions.flush(handle, crossing_frame, output), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.flush(handle, guarded_frame, crossing_output), LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_TRUE(Core::VirtualMemory::Protect(guarded.Address() + page_size, page_size, Core::VirtualMemory::Mode::ReadWrite));

	EXPECT_EQ(functions.flush(handle, frame_buffer, output), OK);
	EXPECT_FALSE(output->is_valid);
	EXPECT_FALSE(frame_buffer->is_accepted);

	EXPECT_EQ(functions.reset(handle), OK);
	ASSERT_EQ(functions.del(handle), OK);
	EXPECT_EQ(functions.del(handle), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.reset(handle), LibKernel::KERNEL_ERROR_EINVAL);
	input->au_data = nullptr;
	input->au_size = 0;
	EXPECT_EQ(functions.decode(handle, input, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.flush(handle, frame_buffer, output), LibKernel::KERNEL_ERROR_EINVAL);

	*decoder_output = nullptr;
	ASSERT_EQ(functions.create(config, memory, decoder_output), OK);
	ASSERT_NE(*decoder_output, nullptr);
	EXPECT_NE(*decoder_output, handle);
	EXPECT_EQ(functions.reset(handle), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(functions.reset(*decoder_output), OK);
	ASSERT_EQ(functions.del(*decoder_output), OK);
}

UT_END();
