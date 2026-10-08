#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Audio.h"
#include "Emulator/AudioNgs2Sampler.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/AudioPcm.h"
#include "Emulator/Config.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include "SDL.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <thread>
#include <vector>

UT_BEGIN(EmulatorAudio);

using namespace Libs::Audio;

namespace {

class GuestReadableBlock
{
public:
	explicit GuestReadableBlock(size_t size):
	    m_size(size), m_address(Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite))
	{
	}
	~GuestReadableBlock()
	{
		if (m_address != 0)
		{
			(void)Core::VirtualMemory::Free(m_address);
		}
	}

	GuestReadableBlock(const GuestReadableBlock&)            = delete;
	GuestReadableBlock& operator=(const GuestReadableBlock&) = delete;

	[[nodiscard]] bool IsValid() const { return m_address != 0; }
	[[nodiscard]] void* Data() const { return reinterpret_cast<void*>(m_address); }
	[[nodiscard]] bool Protect(Core::VirtualMemory::Mode mode) const
	{
		return m_address != 0 && m_size != 0 && Core::VirtualMemory::Protect(m_address, m_size, mode);
	}
	void               Release() { m_address = 0; }

private:
	size_t   m_size    = 0;
	uint64_t m_address = 0;
};

template <typename T>
class GuestValue
{
public:
	GuestValue(): m_storage(sizeof(T)) {}

	[[nodiscard]] bool IsValid() const { return m_storage.IsValid(); }
	[[nodiscard]] T*   Data() const { return static_cast<T*>(m_storage.Data()); }

private:
	GuestReadableBlock m_storage;
};

// Generic host stand-in for the guest runtime: it calls the target as an ordinary
// function with the forwarded arguments, so AvPlayer file and event callbacks run
// in-process. This is the same dispatch the real runtime performs, not a bypass.
inline uint64_t KYTY_SYSV_ABI HostRuntimeInvoke(uint64_t target, uint64_t a0, uint64_t a1, uint64_t a2)
{
	return reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(uint64_t, uint64_t, uint64_t)>(target)(a0, a1, a2);
}

inline uint64_t KYTY_SYSV_ABI HostRuntimeInvoke4(uint64_t target, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	return reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t)>(target)(a0, a1, a2, a3);
}

inline bool HostRuntimeIsExecutable(uint64_t /*address*/)
{
	return true;
}

// Installs the host runtime dispatch for the lifetime of the scope so the guest
// callbacks the player invokes actually run.
class ScopedHostRuntime
{
public:
	ScopedHostRuntime()
	{
		::Kyty::Emulator::GuestRuntimePort::Provider provider {};
		provider.invoke                = HostRuntimeInvoke;
		provider.invoke4               = HostRuntimeInvoke4;
		provider.is_executable_address = HostRuntimeIsExecutable;
		::Kyty::Emulator::GuestRuntimePort::Install(provider);
	}
	~ScopedHostRuntime() { ::Kyty::Emulator::GuestRuntimePort::Install({}); }
	ScopedHostRuntime(const ScopedHostRuntime&)            = delete;
	ScopedHostRuntime& operator=(const ScopedHostRuntime&) = delete;
};

// A host file exposed through the AvPlayer file-replacement callbacks, which is
// the production hook the guest uses to supply media bytes. open receives the URI
// after the player sanitizes it, records it, and opens exactly that name, so a
// sanitizer regression fails both the open and the recorded-URI assertion.
struct HostFileReplacement
{
	std::string opened_uri;
	std::FILE*  file = nullptr;
};

inline int KYTY_SYSV_ABI HostFileOpen(void* object, const char* uri)
{
	auto* self       = static_cast<HostFileReplacement*>(object);
	self->opened_uri = uri != nullptr ? uri : "";
	self->file       = uri != nullptr ? std::fopen(uri, "rb") : nullptr;
	return self->file != nullptr ? 0 : -1;
}

inline int KYTY_SYSV_ABI HostFileClose(void* object)
{
	auto* self = static_cast<HostFileReplacement*>(object);
	if (self->file != nullptr)
	{
		std::fclose(self->file);
		self->file = nullptr;
	}
	return 0;
}

inline int KYTY_SYSV_ABI HostFileReadOffset(void* object, uint8_t* destination, uint64_t offset, uint32_t size)
{
	auto* self = static_cast<HostFileReplacement*>(object);
	if (self->file == nullptr || destination == nullptr || std::fseek(self->file, static_cast<long>(offset), SEEK_SET) != 0)
	{
		return -1;
	}
	return static_cast<int>(std::fread(destination, 1, size, self->file));
}

inline uint64_t KYTY_SYSV_ABI HostFileSize(void* object)
{
	auto* self = static_cast<HostFileReplacement*>(object);
	if (self->file == nullptr || std::fseek(self->file, 0, SEEK_END) != 0)
	{
		return 0;
	}
	const long end = std::ftell(self->file);
	std::fseek(self->file, 0, SEEK_SET);
	return end > 0 ? static_cast<uint64_t>(end) : 0;
}

inline AvPlayer::AvPlayerFileReplacement MakeHostFileReplacement(HostFileReplacement* object)
{
	AvPlayer::AvPlayerFileReplacement file {};
	file.object_pointer = object;
	file.open           = HostFileOpen;
	file.close          = HostFileClose;
	file.read_offset    = HostFileReadOffset;
	file.size           = HostFileSize;
	return file;
}

} // namespace

TEST(EmulatorAudio, AudioPropagationQueryCreatesAndReleasesCallerOwnedSystem)
{
	struct SystemAttribute
	{
		uint32_t id;
		uint32_t unused;
		uint64_t value;
		uint64_t size;
	};
	static_assert(sizeof(SystemAttribute) == 0x18);

	GuestValue<AudioPropagation::SystemOption> option_storage;
	GuestValue<AudioPropagation::SystemMemory> memory_storage;
	GuestValue<uint64_t>                       system_storage;
	GuestValue<uint64_t>                       room_storage;
	GuestReadableBlock                         material_storage(0x40);
	GuestValue<uint64_t>                       material_handle_storage;
	GuestValue<SystemAttribute>                attribute_storage;
	GuestValue<uint64_t>                       attribute_value_storage;
	GuestReadableBlock                         rays_storage(64 * 0x58);
	GuestValue<uint32_t>                       ray_count_storage;
	ASSERT_TRUE(option_storage.IsValid() && memory_storage.IsValid() && system_storage.IsValid() && room_storage.IsValid() &&
	            material_storage.IsValid() && material_handle_storage.IsValid() && attribute_storage.IsValid() &&
	            attribute_value_storage.IsValid() && rays_storage.IsValid() && ray_count_storage.IsValid());

	auto* option           = option_storage.Data();
	auto* memory           = memory_storage.Data();
	*option                = {};
	option->desc.id        = 0x010107d5;
	option->desc.size      = 0x38;
	option->max_sources    = 64;
	option->max_materials  = 1;
	option->max_raycasts   = 6;
	option->max_bounces    = 1;
	option->update_grain   = 512;
	option->speed_of_sound = 343.0f;
	option->max_distance   = 150.0f;
	*memory                = {};
	memory->desc.id        = 0x010107d4;
	memory->desc.size      = 0x30;

	ASSERT_EQ(AudioPropagation::SystemQueryMemory(option, memory), 0);
	EXPECT_EQ(memory->desc.id, 0x010107d4u);
	EXPECT_EQ(memory->desc.size, 0x30u);
	EXPECT_GE(memory->cpu_memory_size, sizeof(AudioPropagation::SystemOption));
	EXPECT_EQ(memory->cpu_memory_size % 16, 0u);
	EXPECT_EQ(memory->gpu_memory_size, 0u);
	GuestReadableBlock workspace(memory->cpu_memory_size);
	ASSERT_TRUE(workspace.IsValid());
	memory->cpu_memory          = workspace.Data();
	const uint64_t queried_size = memory->cpu_memory_size;
	memory->cpu_memory_size     = queried_size - 1;
	EXPECT_EQ(AudioPropagation::SystemCreate(option, memory, system_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	memory->cpu_memory_size = queried_size;
	ASSERT_EQ(AudioPropagation::SystemCreate(option, memory, system_storage.Data()), 0);
	EXPECT_NE(*system_storage.Data(), 0u);
	EXPECT_EQ(AudioPropagation::RoomCreate(0, room_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(AudioPropagation::RoomCreate(*system_storage.Data(), room_storage.Data()), 0);
	EXPECT_NE(*room_storage.Data(), 0u);
	std::memset(material_storage.Data(), 0, 0x40);
	auto* material_desc = static_cast<AudioPropagation::StructDescriptor*>(material_storage.Data());
	material_desc->id   = 0x010107d1;
	material_desc->size = 0x40;
	*material_handle_storage.Data() = 0;
	EXPECT_EQ(AudioPropagation::SystemRegisterMaterial(0, material_storage.Data(), material_handle_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*material_handle_storage.Data(), 0u);
	ASSERT_EQ(AudioPropagation::SystemRegisterMaterial(*system_storage.Data(), material_storage.Data(), material_handle_storage.Data()), 0);
	EXPECT_NE(*material_handle_storage.Data(), 0u);
	EXPECT_NE(*material_handle_storage.Data(), *room_storage.Data());
	const uint64_t material_handle = *material_handle_storage.Data();
	EXPECT_EQ(AudioPropagation::SystemRegisterMaterial(*system_storage.Data(), material_storage.Data(), material_handle_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_ENOMEM);
	EXPECT_EQ(*material_handle_storage.Data(), material_handle);
	*attribute_storage.Data() = SystemAttribute {.id = 0x20000,
	                                            .unused = 0xdeadbeef,
	                                            .value = reinterpret_cast<uint64_t>(attribute_value_storage.Data()),
	                                            .size = sizeof(uint64_t)};
	*attribute_value_storage.Data() = material_handle;
	EXPECT_EQ(AudioPropagation::SystemSetAttributes(*system_storage.Data(), attribute_storage.Data(), 1),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	*attribute_value_storage.Data() = *room_storage.Data();
	EXPECT_EQ(AudioPropagation::SystemSetAttributes(0, attribute_storage.Data(), 1), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(AudioPropagation::SystemSetAttributes(*system_storage.Data(), attribute_storage.Data(), 1), 0);
	std::memset(rays_storage.Data(), 0x5a, 64 * 0x58);
	*ray_count_storage.Data() = 64;
	EXPECT_EQ(AudioPropagation::SystemGetRays(0, rays_storage.Data(), ray_count_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*ray_count_storage.Data(), 64u);
	ASSERT_EQ(AudioPropagation::SystemGetRays(*system_storage.Data(), rays_storage.Data(), ray_count_storage.Data()), 0);
	EXPECT_EQ(*ray_count_storage.Data(), 0u);
	EXPECT_EQ(*static_cast<uint8_t*>(rays_storage.Data()), 0x5au);
	EXPECT_EQ(AudioPropagation::RoomDestroy(0, *room_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioPropagation::RoomDestroy(*system_storage.Data(), *room_storage.Data()), 0);
	EXPECT_EQ(AudioPropagation::SystemSetAttributes(*system_storage.Data(), attribute_storage.Data(), 1),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioPropagation::RoomDestroy(*system_storage.Data(), *room_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioPropagation::SystemDestroy(*system_storage.Data()), 0);
	EXPECT_EQ(AudioPropagation::SystemRegisterMaterial(*system_storage.Data(), material_storage.Data(), material_handle_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioPropagation::SystemDestroy(*system_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorAudio, AudioOut2UserCreateUsesTwoArgumentPointerSizedHandleAbi)
{
	using ExpectedCreate = int(KYTY_SYSV_ABI*)(uint32_t, uintptr_t*);
	using ExpectedDestroy = int(KYTY_SYSV_ABI*)(uintptr_t);

	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2UserCreate), ExpectedCreate>));
	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2UserDestroy), ExpectedDestroy>));
}

TEST(EmulatorAudio, AudioOut2UserCreateRejectsReadOnlyOutputWithoutWriting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EXIT(
	    {
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		GuestReadableBlock output_storage(sizeof(uintptr_t));
		if (!output_storage.IsValid())
		{
			std::_Exit(2);
		}
		*static_cast<uintptr_t*>(output_storage.Data()) = UINTPTR_MAX;
		if (!output_storage.Protect(Core::VirtualMemory::Mode::Read))
		{
			std::_Exit(3);
		}
		const int result = AudioOut2::AudioOut2UserCreate(255, static_cast<uintptr_t*>(output_storage.Data()));
		std::_Exit(result == Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorAudio, AudioOut2GetSpeakerInfoMatchesEvidencedTwoArgumentAbi)
{
	using ExpectedGetSpeakerInfo = int(KYTY_SYSV_ABI*)(void*, uint32_t);
	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2GetSpeakerInfo), ExpectedGetSpeakerInfo>));
}

TEST(EmulatorAudio, AudioOut2GetSpeakerInfoWritesStereoHostRecord)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// Independent copy of the observed record: type @0, available_bits @4,
	// flags @8, then 16 {azimuth, elevation} int16 degree pairs at @0x10.
	struct SpeakerAngle
	{
		int16_t azimuth;
		int16_t elevation;
	};
	struct SpeakerInfo
	{
		uint8_t      type;
		uint8_t      reserved1;
		uint16_t     reserved2;
		uint32_t     available_bits;
		uint32_t     flags;
		uint32_t     reserved3;
		SpeakerAngle speaker_angle[16];
	};
	static_assert(sizeof(SpeakerInfo) == 0x50);
	static_assert(offsetof(SpeakerInfo, available_bits) == 0x4);
	static_assert(offsetof(SpeakerInfo, flags) == 0x8);
	static_assert(offsetof(SpeakerInfo, speaker_angle) == 0x10);

	// Observed call sites pass selector 0 and 1 against one host output.
	for (uint32_t selector: {0u, 1u})
	{
		GuestReadableBlock storage(0x100);
		ASSERT_TRUE(storage.IsValid());
		std::memset(storage.Data(), 0xa5, 0x100);
		ASSERT_EQ(AudioOut2::AudioOut2GetSpeakerInfo(storage.Data(), selector), 0);

		const auto* info = static_cast<const SpeakerInfo*>(storage.Data());
		EXPECT_EQ(info->type, 0u);
		EXPECT_EQ(info->reserved1, 0u);
		EXPECT_EQ(info->reserved2, 0u);
		EXPECT_EQ(info->available_bits, 0x3u);
		EXPECT_EQ(info->flags, 0u);
		EXPECT_EQ(info->reserved3, 0u);
		EXPECT_EQ(info->speaker_angle[0].azimuth, 30);
		EXPECT_EQ(info->speaker_angle[0].elevation, 0);
		EXPECT_EQ(info->speaker_angle[1].azimuth, -30);
		EXPECT_EQ(info->speaker_angle[1].elevation, 0);
		for (size_t i = 2; i < 16; i++)
		{
			EXPECT_EQ(info->speaker_angle[i].azimuth, 0);
			EXPECT_EQ(info->speaker_angle[i].elevation, 0);
		}
		// The observed caller keeps the record on its stack: nothing past the
		// 0x50-byte extent may be touched.
		const auto* tail = static_cast<const uint8_t*>(storage.Data()) + 0x50;
		for (size_t i = 0; i < 0x100 - 0x50; i++)
		{
			ASSERT_EQ(tail[i], 0xa5u) << i;
		}
	}
}

TEST(EmulatorAudio, AudioOut2GetSpeakerInfoRejectsInvalidOutputWithoutWriting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	EXPECT_EQ(AudioOut2::AudioOut2GetSpeakerInfo(nullptr, 0), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	ASSERT_EXIT(
	    {
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		GuestReadableBlock storage(0x100);
		if (!storage.IsValid())
		{
			std::_Exit(2);
		}
		std::memset(storage.Data(), 0xa5, 0x100);
		if (!storage.Protect(Core::VirtualMemory::Mode::Read))
		{
			std::_Exit(3);
		}
		const int   result    = AudioOut2::AudioOut2GetSpeakerInfo(storage.Data(), 0);
		const auto* bytes     = static_cast<const uint8_t*>(storage.Data());
		bool        unchanged = true;
		for (size_t i = 0; i < 0x100; i++)
		{
			if (bytes[i] != 0xa5)
			{
				unchanged = false;
				break;
			}
		}
		std::_Exit(result == Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL && unchanged ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorAudio, AudioInCloseReleasesTheHostInputSlot)
{
	// The HLE owns the guest-visible handle, while HostAudio owns the slot.
	// A second close is therefore the observable regression boundary.
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	EXPECT_EQ(SDL_setenv("SDL_AUDIODRIVER", "dummy", 0), 0);

	auto* subsystem = AudioSubsystem::Instance();
	subsystem->Destroy(Core::SubsystemsList::Instance());
	subsystem->Init(Core::SubsystemsList::Instance());

	const int handle = AudioIn::AudioInOpen(255, 1, 0, 256, 48'000, 2);
	EXPECT_GT(handle, 0);
	if (handle > 0)
	{
		EXPECT_EQ(AudioIn::AudioInClose(handle), 0);
		EXPECT_EQ(AudioIn::AudioInClose(handle), AUDIO_IN_ERROR_INVALID_HANDLE);
	}

	subsystem->Destroy(Core::SubsystemsList::Instance());
}

TEST(EmulatorAudio, AudioOut2ContextLifecycleAcceptsObservedGen5Profile)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr size_t     kContextParamBytes = 0x40;
	constexpr size_t     kContextBytes      = 0x10000;
	GuestReadableBlock   context_param_storage(kContextParamBytes);
	GuestReadableBlock   context_workspace(kContextBytes);
	GuestValue<uint64_t> memory_size_storage;
	GuestValue<int32_t>  context_storage;
	ASSERT_TRUE(context_param_storage.IsValid());
	ASSERT_TRUE(context_workspace.IsValid());
	ASSERT_TRUE(memory_size_storage.IsValid());
	ASSERT_TRUE(context_storage.IsValid());
	std::memset(context_param_storage.Data(), 0, kContextParamBytes);
	std::memset(context_workspace.Data(), 0xa5, kContextBytes);
	*memory_size_storage.Data() = UINT64_MAX;
	*context_storage.Data()     = INT32_MAX;

	auto* context_param = context_param_storage.Data();
	EXPECT_EQ(AudioOut2::AudioOut2ContextResetParam(nullptr), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(AudioOut2::AudioOut2ContextResetParam(context_param), 0);
	const uint64_t observed_profile[] = {0x0000008000000012ull, 0x0000000100000000ull, 0x0000000100000100ull};
	std::memcpy(context_param, observed_profile, sizeof(observed_profile));
	EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(nullptr, memory_size_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(AudioOut2::AudioOut2ContextQueryMemory(context_param, memory_size_storage.Data()), 0);
	EXPECT_EQ(*memory_size_storage.Data(), kContextBytes);
	EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(nullptr, context_workspace.Data(), kContextBytes, context_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(context_param, context_workspace.Data(), kContextBytes - 1, context_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	ASSERT_EQ(AudioOut2::AudioOut2ContextCreate(context_param, context_workspace.Data(), kContextBytes, context_storage.Data()), 0);
	EXPECT_GT(*context_storage.Data(), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(*context_storage.Data()), 0);
	for (size_t i = sizeof(observed_profile); i < kContextParamBytes; ++i)
	{
		EXPECT_EQ(static_cast<uint8_t*>(context_param_storage.Data())[i], 0);
	}
	EXPECT_TRUE(std::all_of(static_cast<uint8_t*>(context_workspace.Data()),
	                        static_cast<uint8_t*>(context_workspace.Data()) + kContextBytes, [](uint8_t value) { return value == 0xa5; }));
}

TEST(EmulatorAudio, AudioOut2ContextLifecycleAcceptsSecondCapturedProfile)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	GuestReadableBlock   parameter_storage(0x40);
	GuestValue<uint64_t> memory_size_storage;
	GuestValue<int32_t>  context_storage;
	ASSERT_TRUE(parameter_storage.IsValid());
	ASSERT_TRUE(memory_size_storage.IsValid());
	ASSERT_TRUE(context_storage.IsValid());
	const uint64_t observed_configuration[8] = {0x0000020000000014ull, 0x0000000100000000ull, 0x0000000100000200ull};
	std::memcpy(parameter_storage.Data(), observed_configuration, sizeof(observed_configuration));
	*memory_size_storage.Data() = 0;
	*context_storage.Data()     = 0;
	ASSERT_EQ(AudioOut2::AudioOut2ContextQueryMemory(parameter_storage.Data(), memory_size_storage.Data()), 0);
	// This is Kyty's existing opaque HLE workspace policy, not a measured
	// native workspace layout or native QueryMemory result for this profile.
	ASSERT_EQ(*memory_size_storage.Data(), 0x10000u);
	GuestReadableBlock workspace(*memory_size_storage.Data());
	ASSERT_TRUE(workspace.IsValid());
	ASSERT_EQ(
	    AudioOut2::AudioOut2ContextCreate(parameter_storage.Data(), workspace.Data(), *memory_size_storage.Data(), context_storage.Data()),
	    0);
	EXPECT_GT(*context_storage.Data(), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(*context_storage.Data()), 0);

	for (const size_t offset: {0u, 4u, 16u, 24u, 63u})
	{
		auto* bytes = static_cast<uint8_t*>(parameter_storage.Data());
		bytes[offset] ^= 1u;
		*memory_size_storage.Data() = UINT64_MAX;
		*context_storage.Data()     = INT32_MAX;
		EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(bytes, memory_size_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
		EXPECT_EQ(*memory_size_storage.Data(), UINT64_MAX);
		EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(bytes, workspace.Data(), 0x10000, context_storage.Data()),
		          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
		EXPECT_EQ(*context_storage.Data(), INT32_MAX);
		bytes[offset] ^= 1u;
	}
}

TEST(EmulatorAudio, AudioOut2SecondCapturedProfileReadsAComplete512FrameGrain)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	GuestReadableBlock   parameter_storage(0x40);
	GuestReadableBlock   workspace(0x10000);
	GuestValue<int32_t>  context_storage;
	GuestValue<uint64_t> memory_size_storage;
	ASSERT_TRUE(parameter_storage.IsValid());
	ASSERT_TRUE(workspace.IsValid());
	ASSERT_TRUE(context_storage.IsValid());
	ASSERT_TRUE(memory_size_storage.IsValid());
	const uint64_t observed_configuration[8] = {0x0000020000000014ull, 0x0000000100000000ull, 0x0000000100000200ull};
	std::memcpy(parameter_storage.Data(), observed_configuration, sizeof(observed_configuration));
	ASSERT_EQ(AudioOut2::AudioOut2ContextQueryMemory(parameter_storage.Data(), memory_size_storage.Data()), 0);
	ASSERT_EQ(*memory_size_storage.Data(), 0x10000u);
	ASSERT_EQ(AudioOut2::AudioOut2ContextCreate(parameter_storage.Data(), workspace.Data(), 0x10000, context_storage.Data()), 0);
	const int32_t context = *context_storage.Data();

	GuestReadableBlock  port_param_storage(16);
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_param_storage.IsValid());
	ASSERT_TRUE(port_storage.IsValid());
	const uint32_t port_param[4] = {0, 0x800, 48000, 0}; // MAIN, F32, 8 channels.
	std::memcpy(port_param_storage.Data(), port_param, sizeof(port_param));
	ASSERT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param_storage.Data(), port_storage.Data()), 0);
	const int32_t port = *port_storage.Data();

	GuestReadableBlock   pcm_storage(0x8000);
	GuestReadableBlock   attribute_storage(0x18);
	GuestValue<uint64_t> pcm_pointer;
	ASSERT_TRUE(pcm_storage.IsValid());
	ASSERT_TRUE(attribute_storage.IsValid());
	ASSERT_TRUE(pcm_pointer.IsValid());
	const auto pcm_address = reinterpret_cast<uint64_t>(pcm_storage.Data());
	std::memset(pcm_storage.Data(), 0, 0x8000);
	ASSERT_TRUE(Core::VirtualMemory::Protect(pcm_address + 0x4000, 0x4000, Core::VirtualMemory::Mode::NoAccess));
	const uint64_t attribute[3] = {0, reinterpret_cast<uint64_t>(pcm_pointer.Data()), sizeof(uint64_t)};
	std::memcpy(attribute_storage.Data(), attribute, sizeof(attribute));

	// Only 256 F32/8-channel frames are readable at this address. A context
	// which accidentally kept the legacy grain would wrongly accept it.
	*pcm_pointer.Data() = pcm_address + 0x2000;
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(port, attribute_storage.Data(), 1), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	// 512 * 8 * sizeof(float) bytes end exactly at the guard page.
	*pcm_pointer.Data() = pcm_address;
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(port, attribute_storage.Data(), 1), 0);
	EXPECT_EQ(AudioOut2::AudioOut2PortDestroy(port), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2HostStatePushPreservesPcmQueueAndSinkAcrossFailure)
{
	// Isolate host queue/sink failure handling from the bounded ContextCreate
	// parameter profiles, which are covered separately above.
	using ExpectedPush    = int(KYTY_SYSV_ABI*)(int32_t, uint32_t);
	using ExpectedSetAttr = int(KYTY_SYSV_ABI*)(int32_t, const void*, uint32_t);
	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2ContextPush), ExpectedPush>));
	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2PortSetAttributes), ExpectedSetAttr>));

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	EXPECT_EQ(SDL_setenv("SDL_AUDIODRIVER", "dummy", 1), 0);

	auto* subsystem = AudioSubsystem::Instance();
	subsystem->Destroy(Core::SubsystemsList::Instance());
	subsystem->Init(Core::SubsystemsList::Instance());
	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);

	// MAIN port, float stereo (data_format 0x200).
	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint16_t*>(port_param + 0)  = 0;      // type MAIN
	*reinterpret_cast<uint32_t*>(port_param + 4)  = 0x200u; // f32 stereo
	*reinterpret_cast<uint32_t*>(port_param + 8)  = 48000u;
	*reinterpret_cast<uint32_t*>(port_param + 12) = 0;
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	*port_storage.Data() = 0;
	ASSERT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), 0);
	const int32_t port = *port_storage.Data();
	ASSERT_GT(port, 0);

	GuestReadableBlock state_storage(0x20);
	ASSERT_TRUE(state_storage.IsValid());
	auto* state = static_cast<uint8_t*>(state_storage.Data());
	ASSERT_EQ(AudioOut2::AudioOut2PortGetState(port, state), 0);
	EXPECT_EQ(state[2], 2u); // channels

	constexpr uint32_t kGrain = 256;
	GuestReadableBlock pcm_storage(sizeof(float) * kGrain * 2 + sizeof(uintptr_t) + 0x18);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* guest_bytes = static_cast<uint8_t*>(pcm_storage.Data());
	auto* grain       = reinterpret_cast<float*>(guest_bytes);
	for (uint32_t i = 0; i < kGrain * 2; i++)
	{
		grain[i] = 0.25f;
	}
	auto* pcm_ptr = reinterpret_cast<uintptr_t*>(guest_bytes + sizeof(float) * kGrain * 2);
	*pcm_ptr      = reinterpret_cast<uintptr_t>(grain);

	// Attribute entry: {u32 id=0, u32 pad, void* value, size_t value_size=8}
	// value points at a qword that holds the PCM address (double-indirect).
	auto* attr = guest_bytes + sizeof(float) * kGrain * 2 + sizeof(uintptr_t);
	std::memset(attr, 0, 0x18);
	*reinterpret_cast<uint32_t*>(attr + 0)  = 0;
	*reinterpret_cast<uint64_t*>(attr + 8)  = reinterpret_cast<uint64_t>(pcm_ptr);
	*reinterpret_cast<uint64_t*>(attr + 16) = sizeof(*pcm_ptr);
	ASSERT_EQ(AudioOut2::AudioOut2PortSetAttributes(port, attr, 1), 0);

	AudioOut2::HostStateTest::FailNextSubmit();
	// A failed enqueue must leave the published PCM, queue accounting and the
	// already-open sink available for the next retry.
	EXPECT_EQ(AudioOut2::AudioOut2ContextPush(context, 0), AUDIO_OUT_ERROR_INVALID_PORT);
	const int sink_after_failure = AudioOut2::HostStateTest::GetContextSinkHandle(context);
	EXPECT_GT(sink_after_failure, 0);

	GuestValue<uint32_t> used_storage;
	GuestValue<uint32_t> available_storage;
	ASSERT_TRUE(used_storage.IsValid());
	ASSERT_TRUE(available_storage.IsValid());
	ASSERT_EQ(AudioOut2::AudioOut2ContextGetQueueLevel(context, used_storage.Data(), available_storage.Data()), 0);
	EXPECT_EQ(*used_storage.Data(), 0u);
	EXPECT_EQ(*available_storage.Data(), 4u);

	ASSERT_EQ(AudioOut2::AudioOut2ContextPush(context, 0), 0);
	EXPECT_EQ(AudioOut2::HostStateTest::GetContextSinkHandle(context), sink_after_failure);
	ASSERT_EQ(AudioOut2::AudioOut2ContextGetQueueLevel(context, used_storage.Data(), available_storage.Data()), 0);
	EXPECT_EQ(*used_storage.Data(), 1u);
	EXPECT_EQ(*available_storage.Data(), 3u);

	ASSERT_EQ(AudioOut2::AudioOut2PortDestroy(port), 0);
	ASSERT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
	subsystem->Destroy(Core::SubsystemsList::Instance());
}

TEST(EmulatorAudio, AudioOut2PortCreateRejectsUnsupportedDataFormat)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);

	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x202u; // two channels, unsupported sample type 2
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	*port_storage.Data() = 0;
	EXPECT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), AUDIO_OUT_ERROR_INVALID_FORMAT);
	EXPECT_EQ(*port_storage.Data(), 0);
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x300u; // three channels are not an evidenced AudioOut layout
	EXPECT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), AUDIO_OUT_ERROR_INVALID_FORMAT);
	EXPECT_EQ(*port_storage.Data(), 0);

	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2PortCreateRejectsUnknownContext)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x200u;
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	*port_storage.Data() = 0;
	EXPECT_EQ(AudioOut2::AudioOut2PortCreate(16, port_param, port_storage.Data()), AUDIO_OUT_ERROR_INVALID_PORT);
	EXPECT_EQ(*port_storage.Data(), 0);
}

TEST(EmulatorAudio, AudioOut2RejectsUnreadablePortBlocks)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);

	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	*port_storage.Data() = 0;
	EXPECT_EQ(AudioOut2::AudioOut2PortCreate(context, reinterpret_cast<const void*>(uintptr_t {1}), port_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x200u;
	ASSERT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), 0);
	const int32_t port = *port_storage.Data();
	EXPECT_EQ(AudioOut2::AudioOut2PortGetState(port, reinterpret_cast<void*>(uintptr_t {1})),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(AudioOut2::AudioOut2PortDestroy(port), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2RejectsUnreadableOutputBlocks)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(nullptr, reinterpret_cast<uint64_t*>(uintptr_t {1})),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	GuestReadableBlock context_workspace(64);
	ASSERT_TRUE(context_workspace.IsValid());
	EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(nullptr, context_workspace.Data(), 64,
	                                             reinterpret_cast<int32_t*>(uintptr_t {1})),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextGetQueueLevel(context, reinterpret_cast<uint32_t*>(uintptr_t {1}), nullptr),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x200u;
	EXPECT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, reinterpret_cast<int32_t*>(uintptr_t {1})),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2UserCreate(255, reinterpret_cast<uintptr_t*>(uintptr_t {1})),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2NonBlockingPushReportsFullQueue)
{
	// Host-state regression only: public ContextCreate remains intentionally
	// unsupported until its guest ABI is measured.
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);
	AudioOut2::HostStateTest::FillContextQueue(context);
	EXPECT_EQ(AudioOut2::AudioOut2ContextPush(context, 0), AUDIO_OUT_ERROR_PORT_FULL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2BlockingEmptyPushPacesOneGrain)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);
	const auto start = std::chrono::steady_clock::now();
	EXPECT_EQ(AudioOut2::AudioOut2ContextPush(context, 1), 0);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	// The default host-state context has a 256-frame grain at 48 kHz.
	EXPECT_GE(elapsed, std::chrono::milliseconds(4));
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2PortSetAttributesRejectsMalformedPcmPointerEntry)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);

	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x200u;
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	ASSERT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), 0);
	const int32_t port = *port_storage.Data();

	GuestReadableBlock attr_storage(sizeof(uintptr_t) + 0x18);
	ASSERT_TRUE(attr_storage.IsValid());
	auto* attr_bytes = static_cast<uint8_t*>(attr_storage.Data());
	auto* pcm_ptr    = reinterpret_cast<uintptr_t*>(attr_bytes);
	*pcm_ptr         = 1;
	auto* attr = attr_bytes + sizeof(*pcm_ptr);
	std::memset(attr, 0, 0x18);
	*reinterpret_cast<uint32_t*>(attr + 0)  = 0;
	*reinterpret_cast<uint64_t*>(attr + 8)  = reinterpret_cast<uint64_t>(pcm_ptr);
	*reinterpret_cast<uint64_t*>(attr + 16) = sizeof(*pcm_ptr);
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(port, attr, 1), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(AudioOut2::AudioOut2PortDestroy(port), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2PortSetAttributesRejectsOversizedEntryCount)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(context, 0);

	GuestReadableBlock port_param_storage(16);
	ASSERT_TRUE(port_param_storage.IsValid());
	auto* port_param = static_cast<uint8_t*>(port_param_storage.Data());
	*reinterpret_cast<uint32_t*>(port_param + 4) = 0x200u;
	GuestValue<int32_t> port_storage;
	ASSERT_TRUE(port_storage.IsValid());
	ASSERT_EQ(AudioOut2::AudioOut2PortCreate(context, port_param, port_storage.Data()), 0);
	const int32_t port = *port_storage.Data();

	GuestReadableBlock attrs_storage(33 * 0x18);
	ASSERT_TRUE(attrs_storage.IsValid());
	auto* attrs = static_cast<uint8_t*>(attrs_storage.Data());
	std::memset(attrs, 0, 33 * 0x18);
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(port, attrs, 33), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(AudioOut2::AudioOut2PortDestroy(port), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(context), 0);
}

TEST(EmulatorAudio, AudioOut2PushRejectsContextRecreatedWhileBlocking)
{
	// Host-state regression only: this exercises the close/recreate race without
	// widening the intentionally unsupported public ContextCreate contract.
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EQ(AudioOut2::AudioOut2Initialize(), 0);
	const int32_t first_context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(first_context, 0);
	AudioOut2::HostStateTest::FillContextQueue(first_context);

	std::atomic<int> blocking_result {0};
	std::thread blocker([&] { blocking_result.store(AudioOut2::AudioOut2ContextPush(first_context, 1)); });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));

	ASSERT_EQ(AudioOut2::AudioOut2ContextDestroy(first_context), 0);
	const int32_t replacement_context = AudioOut2::HostStateTest::CreateContext();
	ASSERT_GT(replacement_context, 0);
	EXPECT_EQ(replacement_context, first_context);

	blocker.join();
	EXPECT_EQ(blocking_result.load(), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(replacement_context), 0);
}

TEST(EmulatorAudio, OpensDecoderThroughCanonicalNamespaceWithoutPrivateMedia)
{
	std::string error;
	auto decoder = ::Kyty::Emulator::AudioVideoBackend::Decoder::Open("", &error);

	EXPECT_EQ(decoder, nullptr);
	EXPECT_FALSE(error.empty());
}

TEST(EmulatorAudio, DecodesConfiguredAvPlayerMedia)
{
	const char* media_path = std::getenv("KYTY_AVPLAYER_TEST_MEDIA");
	if (media_path == nullptr || media_path[0] == '\0' || !::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}

	std::string error;
	auto decoder = ::Kyty::Emulator::AudioVideoBackend::Decoder::Open(media_path, &error);
	ASSERT_NE(decoder, nullptr) << error;
	const auto& stream = decoder->GetStreamInfo();
	ASSERT_TRUE(stream.has_video);
	EXPECT_GT(stream.video_width, 0u);
	EXPECT_GT(stream.video_height, 0u);

	::Kyty::Emulator::AudioVideoBackend::VideoFrame video;
	ASSERT_TRUE(decoder->ReadVideoFrame(&video));
	EXPECT_EQ(video.width, stream.video_width);
	EXPECT_EQ(video.height, stream.video_height);
	EXPECT_EQ(video.pitch, stream.video_pitch);
	EXPECT_EQ(video.data.size(), static_cast<size_t>(video.width) * video.height +
	                                    static_cast<size_t>(video.pitch) * ((video.height + 1u) / 2u));

	if (stream.has_audio)
	{
		::Kyty::Emulator::AudioVideoBackend::AudioFrame audio;
		ASSERT_TRUE(decoder->ReadAudioFrame(&audio));
		EXPECT_EQ(audio.channels, stream.audio_channels);
		EXPECT_EQ(audio.sample_rate, stream.audio_sample_rate);
		EXPECT_FALSE(audio.data.empty());
	}
}

TEST(EmulatorAudio, VideoEndOfStreamDoesNotWaitForPendingAudio)
{
	const char* media_path = std::getenv("KYTY_AVPLAYER_TEST_MEDIA");
	if (media_path == nullptr || media_path[0] == '\0' || !::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}

	std::string error;
	auto decoder = ::Kyty::Emulator::AudioVideoBackend::Decoder::Open(media_path, &error);
	ASSERT_NE(decoder, nullptr) << error;
	ASSERT_TRUE(decoder->GetStreamInfo().has_video);

	::Kyty::Emulator::AudioVideoBackend::VideoFrame video;
	size_t frames = 0;
	uint64_t previous_timestamp = 0;
	while (decoder->ReadVideoFrame(&video))
	{
		if (frames == 0)
		{
			EXPECT_LE(video.timestamp_ms, 1u);
		}
		else
		{
			EXPECT_GE(video.timestamp_ms, previous_timestamp);
			EXPECT_LE(video.timestamp_ms - previous_timestamp, 50u);
		}
		previous_timestamp = video.timestamp_ms;
		++frames;
	}

	EXPECT_GT(frames, 0u);
	EXPECT_EQ(decoder->LastStatus(), ::Kyty::Emulator::AudioVideoBackend::Status::Ok);
	EXPECT_TRUE(decoder->EndOfStream());
}

TEST(EmulatorAudio, BoundsDecodeAheadWhenVideoConsumerIsPaused)
{
	const char* media_path = std::getenv("KYTY_AVPLAYER_TEST_MEDIA");
	if (media_path == nullptr || media_path[0] == '\0' || !::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}

	std::string error;
	auto decoder = ::Kyty::Emulator::AudioVideoBackend::Decoder::Open(media_path, &error);
	ASSERT_NE(decoder, nullptr) << error;

	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	::Kyty::Emulator::AudioVideoBackend::VideoFrame video;
	ASSERT_TRUE(decoder->ReadVideoFrame(&video));
	EXPECT_LT(video.timestamp_ms, 1000u);
}

TEST(EmulatorAudio, AvPlayerUsesConfiguredMediaBackend)
{
	const char* media_path = std::getenv("KYTY_AVPLAYER_TEST_MEDIA");
	if (media_path == nullptr || media_path[0] == '\0' || !::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// The media is supplied through the production file-replacement callbacks, so
	// the raw host path never has to pass the guest filesystem policy.
	HostFileReplacement host_file;
	ScopedHostRuntime   runtime;

	AvPlayer::AvPlayerInitDataEx init {};
	init.this_size        = sizeof(init);
	init.file_replacement = MakeHostFileReplacement(&host_file);
	AvPlayer::AvPlayerInternal* handle = nullptr;
	ASSERT_EQ(AvPlayer::AvPlayerInitEx(&init, &handle), 0);
	ASSERT_NE(handle, nullptr);
	ASSERT_EQ(AvPlayer::AvPlayerAddSource(handle, media_path), 0);
	// The file callback received the name the player passed on, unchanged.
	EXPECT_EQ(host_file.opened_uri, std::string(media_path));
	AvPlayer::AvPlayerStreamInfoEx stream {};
	ASSERT_EQ(AvPlayer::AvPlayerGetStreamInfoEx(handle, 0, &stream), 0);
	// Dimensions come from the fixture's own stream, not a hardcoded resolution.
	EXPECT_GT(stream.details.video.width, 0u);
	EXPECT_GT(stream.details.video.height, 0u);
	ASSERT_EQ(AvPlayer::AvPlayerStart(handle), 0);
	AvPlayer::AvPlayerFrameInfoEx frame {};
	const auto frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool got_frame = false;
	while (!got_frame && std::chrono::steady_clock::now() < frame_deadline)
	{
		got_frame = AvPlayer::AvPlayerGetVideoDataEx(handle, &frame) != 0;
		if (got_frame)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(got_frame, 1);
	ASSERT_NE(frame.data, nullptr);
	EXPECT_EQ(frame.details.video.width, stream.details.video.width);
	EXPECT_EQ(frame.details.video.height, stream.details.video.height);
	ASSERT_EQ(AvPlayer::AvPlayerClose(handle), 0);
}

TEST(EmulatorAudio, AppliesGuestChannelVolumesWithoutChangingLayout)
{
	const int16_t        source[]  = {12000, -12000, 8000, -8000};
	const int            volumes[] = {16384, 8192};
	std::vector<uint8_t> output;
	ASSERT_TRUE(AudioPcmApplyChannelVolumes(source, 2, 2, AudioPcmFormat::Signed16, volumes, &output));
	ASSERT_EQ(output.size(), sizeof(source));
	const auto* samples = reinterpret_cast<const int16_t*>(output.data());
	EXPECT_EQ(samples[0], 6000);
	EXPECT_EQ(samples[1], -3000);
	EXPECT_EQ(samples[2], 4000);
	EXPECT_EQ(samples[3], -2000);
}

TEST(EmulatorAudio, ComputesHostQueueCapacityFromTimeAndFormat)
{
	EXPECT_EQ(AudioPcmQueueBytes(48000, 2, AudioPcmFormat::Signed16, 60), 11520u);
	EXPECT_EQ(AudioPcmQueueBytes(48000, 8, AudioPcmFormat::Float32, 60), 92160u);
	EXPECT_EQ(AudioPcmQueueBytes(0, 2, AudioPcmFormat::Signed16, 60), 0u);
}

TEST(EmulatorAudio, UnregistersCapturedAjmCodecModule)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(context, 1u);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	EXPECT_EQ(Ajm::AjmModuleUnregister(context, 1), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, AcceptsGen5AjmInitializationFlagsAndCodecModules)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context = 0;
	ASSERT_EQ(Ajm::AjmInitialize(INT64_C(0x300000000), &context), 0);
	ASSERT_NE(context, 0u);
	uint8_t registered_page[4096] {};
	EXPECT_EQ(Ajm::AjmMemoryRegister(context, registered_page, 1), 0);
	uint8_t batch_storage[0x708] {};
	uint8_t batch_control[64] {};
	EXPECT_EQ(Ajm::AjmBatchInitializeBuffer(batch_storage, sizeof(batch_storage), batch_control), 0);
	EXPECT_EQ(Ajm::AjmModuleRegister(context, 24, 0), 0);
	EXPECT_EQ(Ajm::AjmModuleRegister(context, 14, 0), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, TracksAjmInstanceLifecycle)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr auto invalid_instance     = static_cast<int32_t>(0x80930003u);
	constexpr auto codec_not_registered = static_cast<int32_t>(0x8093000au);
	constexpr auto wrong_revision       = static_cast<int32_t>(0x8093000bu);

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	EXPECT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), codec_not_registered);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	EXPECT_EQ(Ajm::AjmInstanceCreate(context, 1, 0, &instance), wrong_revision);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);
	EXPECT_EQ(instance >> 14u, 1u);
	EXPECT_NE(instance & 0x3fffu, 0u);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), invalid_instance);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, SerializesAjmControlBatchJob)
{
	alignas(uint64_t) uint8_t batch[64] {};
	alignas(uint64_t) uint8_t sideband_input[16] {};
	alignas(uint64_t) uint8_t sideband_output[32] {};
	constexpr uint32_t        instance = 0x4001u;
	constexpr uint64_t        flags    = 0x000060000000c007ull;

	auto* next = static_cast<uint8_t*>(Ajm::AjmBatchJobControlBufferRa(batch, instance, flags, sideband_input, sizeof(sideband_input),
	                                                                   sideband_output, sizeof(sideband_output), nullptr));
	ASSERT_EQ(next, batch + 48u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 0u), instance << 6u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 4u), 40u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 8u), 2u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 12u), sizeof(sideband_input));
	EXPECT_EQ(*reinterpret_cast<void**>(batch + 16u), sideband_input);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 24u), 3u | (0x6000u << 6u));
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 28u), 0x0000c007u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 32u), 18u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(batch + 36u), sizeof(sideband_output));
	EXPECT_EQ(*reinterpret_cast<void**>(batch + 40u), sideband_output);
}

TEST(EmulatorAudio, CompletesAjmBufferBatchWithBoundedOutputs)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);

	alignas(uint64_t) uint8_t batch[128] {};
	uint8_t                   input[8] {};
	uint8_t                   output[16];
	alignas(uint64_t) uint8_t sideband[32];
	std::memset(output, 0xa5, sizeof(output));
	std::memset(sideband, 0xa5, sizeof(sideband));
	auto* end = static_cast<uint8_t*>(Ajm::AjmBatchJobRunBufferRa(batch, instance, (1ull << 47u) | 1u, input, sizeof(input), output,
	                                                              sizeof(output), sideband, sizeof(sideband), nullptr));
	ASSERT_NE(end, nullptr);

	Ajm::AjmBatchError error {};
	uint32_t           batch_id = 0;
	ASSERT_EQ(Ajm::AjmBatchStartBuffer(context, batch, static_cast<uint32_t>(end - batch), 0, &error, &batch_id), 0);
	EXPECT_NE(batch_id, 0u);
	EXPECT_EQ(error.error_code, 0);
	for (auto value: output)
	{
		EXPECT_EQ(value, 0u);
	}
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 0u), 0);
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 8u), static_cast<int32_t>(sizeof(input)));
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 12u), static_cast<int32_t>(sizeof(output)));
	EXPECT_EQ(Ajm::AjmBatchWait(context, batch_id, UINT32_MAX, &error), 0);
	EXPECT_EQ(Ajm::AjmBatchWait(context, batch_id, UINT32_MAX, &error), static_cast<int32_t>(0x80930004u));
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, WritesCombinedAjmSidebandsInAbiOrder)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);

	alignas(uint64_t) uint8_t batch[128] {};
	uint8_t                   input[256] {};
	uint8_t                   output[4096] {};
	alignas(uint64_t) uint8_t sideband[56];
	std::memset(sideband, 0xa5, sizeof(sideband));
	constexpr uint64_t flags = (1ull << 46u) | (1ull << 47u) | (1ull << 12u) | 1u;
	auto* end = static_cast<uint8_t*>(Ajm::AjmBatchJobRunBufferRa(batch, instance, flags, input, sizeof(input), output, sizeof(output),
	                                                              sideband, sizeof(sideband), nullptr));
	ASSERT_NE(end, nullptr);

	Ajm::AjmBatchError error {};
	uint32_t           batch_id = 0;
	ASSERT_EQ(Ajm::AjmBatchStartBuffer(context, batch, static_cast<uint32_t>(end - batch), 40, &error, &batch_id), 0);
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 0u), 0);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(sideband + 8u), 2u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(sideband + 12u), 3u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(sideband + 16u), 48000u);
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 32u), static_cast<int32_t>(sizeof(input)));
	EXPECT_EQ(*reinterpret_cast<int32_t*>(sideband + 36u), static_cast<int32_t>(sizeof(output)));
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(sideband + 40u), sizeof(output) / 4u);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(sideband + 48u), 1u);
	EXPECT_EQ(Ajm::AjmBatchWait(context, batch_id, UINT32_MAX, &error), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, CompletesQueuedAjmDecodeJobs)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);

	alignas(uint64_t) uint8_t batch[64] {};
	uint8_t                   config[8] {0xfe, 0x72, 0x09, 0xf0};
	uint8_t                   init_result[8];
	uint8_t                   input[32] {};
	uint8_t                   output[128];
	alignas(uint64_t) uint8_t decode_result[32];
	std::memset(init_result, 0xa5, sizeof(init_result));
	std::memset(output, 0xa5, sizeof(output));
	std::memset(decode_result, 0xa5, sizeof(decode_result));

	ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, instance, config, sizeof(config), init_result), 0);
	ASSERT_EQ(
	    Ajm::AjmBatchJobDecode(batch, instance, input, sizeof(input), output, sizeof(output), decode_result, nullptr, 0, decode_result), 0);
	Ajm::AjmBatchError error {};
	uint32_t           batch_id = 0;
	ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, &error, &batch_id), 0);
	EXPECT_NE(batch_id, 0u);
	for (auto value: output)
	{
		EXPECT_EQ(value, 0u);
	}
	EXPECT_EQ(*reinterpret_cast<int32_t*>(decode_result + 0u), 0);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(decode_result + 8u), sizeof(input));
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(decode_result + 12u), sizeof(output));
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(decode_result + 16u), sizeof(output) / 4u);
	EXPECT_EQ(Ajm::AjmBatchWait(context, batch_id, UINT32_MAX, &error), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, AcceptsExtendedAudio3dOpenParameters)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	alignas(uint64_t) uint8_t parameters[0x28]      = {};
	*reinterpret_cast<uint64_t*>(parameters + 0x00) = sizeof(parameters);
	*reinterpret_cast<uint32_t*>(parameters + 0x08) = 0x200;
	*reinterpret_cast<uint32_t*>(parameters + 0x0c) = 0;
	*reinterpret_cast<uint32_t*>(parameters + 0x10) = 128;
	*reinterpret_cast<uint32_t*>(parameters + 0x14) = 4;
	*reinterpret_cast<uint32_t*>(parameters + 0x18) = 2;
	*reinterpret_cast<uint32_t*>(parameters + 0x20) = 2;

	uint32_t port = UINT32_MAX;
	ASSERT_EQ(Audio3d::Audio3dInitialize(0), 0);
	ASSERT_EQ(Audio3d::Audio3dPortOpen(255, reinterpret_cast<const Audio3d::Audio3dOpenParameters*>(parameters), &port), 0);
	EXPECT_LT(port, 4u);

	uint32_t queue_level     = UINT32_MAX;
	uint32_t queue_available = 0;
	EXPECT_EQ(Audio3d::Audio3dPortGetQueueLevel(port, &queue_level, &queue_available), 0);
	EXPECT_EQ(queue_level, 0u);
	EXPECT_EQ(queue_available, 4u);

	uint32_t capability_count = 0;
	ASSERT_EQ(Audio3d::Audio3dPortGetAttributesSupported(port, nullptr, &capability_count), 0);
	ASSERT_EQ(capability_count, 3u);
	uint32_t capabilities[3] = {};
	ASSERT_EQ(Audio3d::Audio3dPortGetAttributesSupported(port, capabilities, &capability_count), 0);
	EXPECT_EQ(capability_count, 3u);
	EXPECT_EQ(capabilities[0], 1u);
	EXPECT_EQ(capabilities[1], 3u);
	EXPECT_EQ(capabilities[2], 9u);

	EXPECT_EQ(Audio3d::Audio3dAudioOutOpen(port, 255, 126, 0, 256, 48000, 1), static_cast<int32_t>(0x80ea0004u));
	EXPECT_EQ(Audio3d::Audio3dAudioOutOutput(1, nullptr), static_cast<int32_t>(0x80ea0004u));
	EXPECT_EQ(Audio3d::Audio3dAudioOutClose(999), static_cast<int32_t>(0x80ea0002u));
}

TEST(EmulatorAudio, QueriesDefaultNgs2SystemBufferContract)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(info_storage.IsValid());
	auto* raw_info = static_cast<uint64_t*>(info_storage.Data());
	for (size_t i = 0; i < 8; i++)
	{
		raw_info[i] = UINT64_MAX;
	}

	auto* info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info);
	EXPECT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, info), 0);
	EXPECT_EQ(raw_info[0], 0u);
	EXPECT_GT(raw_info[1], 0u);
	for (int i = 2; i < 7; i++)
	{
		EXPECT_EQ(raw_info[i], 0u);
	}
	EXPECT_EQ(raw_info[7], UINT64_MAX);

	EXPECT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, nullptr), static_cast<int32_t>(0x804a0053u));
}

TEST(EmulatorAudio, Ngs2QueryRejectsReadOnlyOutputWithoutWriting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ASSERT_EXIT(
	    {
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		GuestReadableBlock output_storage(sizeof(uint64_t) * 8);
		if (!output_storage.IsValid())
		{
			std::_Exit(2);
		}
		std::memset(output_storage.Data(), 0xa5, sizeof(uint64_t) * 8);
		if (!output_storage.Protect(Core::VirtualMemory::Mode::Read))
		{
			std::_Exit(3);
		}
		const int result = Ngs2::Ngs2SystemQueryBufferSize(
		    nullptr, static_cast<Ngs2::Ngs2ContextBufferInfo*>(output_storage.Data()));
		std::_Exit(result == static_cast<int32_t>(0x804a0053u) ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorAudio, Ngs2SystemCreateRejectsOversizedGrainWithoutUsingWorkspace)
{
	constexpr auto kInvalidOption = static_cast<int32_t>(0x804a0081u);
	constexpr auto kSentinel      = UINTPTR_MAX;
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock option_storage(64);
	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	GuestReadableBlock workspace_storage(0x1000);
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(option_storage.IsValid());
	ASSERT_TRUE(info_storage.IsValid());
	ASSERT_TRUE(workspace_storage.IsValid());
	ASSERT_TRUE(handle_storage.IsValid());

	auto* option = static_cast<uint8_t*>(option_storage.Data());
	std::memset(option, 0, 64);
	*reinterpret_cast<size_t*>(option + 0)     = 64;
	*reinterpret_cast<uint32_t*>(option + 28)  = 8192;
	*reinterpret_cast<uint32_t*>(option + 32)  = 8193;
	*reinterpret_cast<uint32_t*>(option + 36)  = 48000;

	auto* workspace = static_cast<uint8_t*>(workspace_storage.Data());
	std::memset(workspace, 0xa5, 0x1000);
	auto* info = static_cast<uint64_t*>(info_storage.Data());
	std::memset(info, 0, sizeof(uint64_t) * 8);
	info[0]                  = reinterpret_cast<uintptr_t>(workspace_storage.Data());
	info[1]                  = 0x1000;
	*handle_storage.Data()   = kSentinel;

	EXPECT_EQ(Ngs2::Ngs2SystemCreate(reinterpret_cast<const Ngs2::Ngs2SystemOption*>(option),
	                                 reinterpret_cast<const Ngs2::Ngs2ContextBufferInfo*>(info), handle_storage.Data()),
	          kInvalidOption);
	EXPECT_EQ(*handle_storage.Data(), kSentinel);
	for (size_t i = 0; i < 0x1000; ++i)
	{
		EXPECT_EQ(workspace[i], 0xa5);
	}
}

TEST(EmulatorAudio, RejectsNgs2SamplerRackQueryWithoutAnEvidencedOption)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(info_storage.IsValid());
	auto* raw_info = static_cast<uint64_t*>(info_storage.Data());
	std::memset(raw_info, 0, sizeof(uint64_t) * 8);
	auto* info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info);

	raw_info[0] = UINT64_MAX;
	raw_info[1] = UINT64_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x1000u, nullptr, info), static_cast<int32_t>(0x804a0081u));
	EXPECT_EQ(raw_info[0], UINT64_MAX);
	EXPECT_EQ(raw_info[1], UINT64_MAX);
	EXPECT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x1000u, reinterpret_cast<const Ngs2::Ngs2RackOption*>(uintptr_t {1}), info),
	          static_cast<int32_t>(0x804a0081u));
	EXPECT_EQ(raw_info[0], UINT64_MAX);
	EXPECT_EQ(raw_info[1], UINT64_MAX);
}

TEST(EmulatorAudio, Ngs2RejectsUnreadableDescriptorAndHandleBlocks)
{
	constexpr auto kInvalidOut = static_cast<int32_t>(0x804a0053u);
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(handle_storage.IsValid());
	*handle_storage.Data() = UINTPTR_MAX;

	EXPECT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(uintptr_t {1})),
	          kInvalidOut);
	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, reinterpret_cast<const Ngs2::Ngs2ContextBufferInfo*>(uintptr_t {1}),
	                                  handle_storage.Data()),
	          static_cast<int32_t>(0x804a0206u));
	EXPECT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(uintptr_t {1}),
	                                         reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(uintptr_t {1})),
	          kInvalidOut);
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(0, 0, reinterpret_cast<uintptr_t*>(uintptr_t {1})), kInvalidOut);
	EXPECT_EQ(*handle_storage.Data(), UINTPTR_MAX);
}

TEST(EmulatorAudio, Ngs2PanOperationsRejectUnmeasuredContracts)
{
	uint32_t params[4] = {};
	float    matrix[2] = {-1.0f, -1.0f};

	EXPECT_EQ(Ngs2::Ngs2PanGetVolumeMatrix(nullptr, nullptr, 0, 0, nullptr), static_cast<int32_t>(0x804a0309u));
	EXPECT_EQ(Ngs2::Ngs2PanGetVolumeMatrix(nullptr, params, 1, 0, matrix), static_cast<int32_t>(0x804a0309u));
	EXPECT_FLOAT_EQ(matrix[0], -1.0f);
	EXPECT_FLOAT_EQ(matrix[1], -1.0f);
	EXPECT_EQ(Ngs2::Ngs2PanInit(nullptr), static_cast<int32_t>(0x804a0309u));
}

TEST(EmulatorAudio, Ngs2RejectsUnmeasuredAllocatorAndGeometryContractsWithoutWrites)
{
	constexpr auto kUnsupported = static_cast<int32_t>(0x804a0309u);
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(handle_storage.IsValid());
	*handle_storage.Data() = UINTPTR_MAX;
	uint8_t        output[256];
	std::memset(output, 0xa5, sizeof(output));

	EXPECT_EQ(Ngs2::Ngs2SystemCreateWithAllocator(reinterpret_cast<const Ngs2::Ngs2SystemOption*>(uintptr_t {1}),
	                                               reinterpret_cast<const Ngs2::Ngs2BufferAllocator*>(uintptr_t {1}), handle_storage.Data()),
	          kUnsupported);
	EXPECT_EQ(*handle_storage.Data(), UINTPTR_MAX);
	EXPECT_EQ(Ngs2::Ngs2RackCreateWithAllocator(1, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(uintptr_t {1}),
	                                             reinterpret_cast<const Ngs2::Ngs2BufferAllocator*>(uintptr_t {1}), handle_storage.Data()),
	          kUnsupported);
	EXPECT_EQ(*handle_storage.Data(), UINTPTR_MAX);

	EXPECT_EQ(Ngs2::Ngs2GeomResetSourceParam(output), kUnsupported);
	EXPECT_EQ(Ngs2::Ngs2GeomResetListenerParam(output), kUnsupported);
	EXPECT_EQ(Ngs2::Ngs2GeomCalcListener(output, output, 0), kUnsupported);
	EXPECT_EQ(Ngs2::Ngs2GeomApply(output, output, output, 0), kUnsupported);
	for (const auto value: output)
	{
		EXPECT_EQ(value, 0xa5);
	}
}

TEST(EmulatorAudio, Ngs2InfoQueriesRejectMissingHandles)
{
	uint8_t info[32] = {};

	EXPECT_EQ(Ngs2::Ngs2RackGetInfo(0, info, sizeof(info)), static_cast<int32_t>(0x804a0261u));
	EXPECT_EQ(Ngs2::Ngs2VoiceGetPortInfo(0, 0, info, sizeof(info)), static_cast<int32_t>(0x804a0300u));
	EXPECT_EQ(Ngs2::Ngs2VoiceQueryInfo(0, 0, nullptr, info), static_cast<int32_t>(0x804a0300u));
}

TEST(EmulatorAudio, Ngs2RackGetInfoRejectsAnUnmeasuredLayoutWithoutWriting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock system_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(system_info_storage.IsValid());
	auto* raw_system_info = static_cast<uint64_t*>(system_info_storage.Data());
	std::memset(raw_system_info, 0, sizeof(uint64_t) * 8);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_system_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info), 0);
	GuestReadableBlock system_storage(raw_system_info[1]);
	ASSERT_TRUE(system_storage.IsValid());
	raw_system_info[0] = reinterpret_cast<uintptr_t>(system_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, system_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	GuestReadableBlock option_storage(0x518);
	ASSERT_TRUE(option_storage.IsValid());
	auto* raw_option                                = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(raw_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	GuestReadableBlock rack_storage(raw_rack_info[1]);
	ASSERT_TRUE(rack_storage.IsValid());
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();

	GuestReadableBlock info_storage(1);
	ASSERT_TRUE(info_storage.IsValid());
	auto* info = static_cast<uint8_t*>(info_storage.Data());
	info[0]    = 0xa5;
	EXPECT_EQ(Ngs2::Ngs2RackGetInfo(rack, info, 1), static_cast<int32_t>(0x804a0309u));
	EXPECT_EQ(info[0], 0xa5);

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, Ngs2SystemRenderRejectsInvalidBuffers)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock system_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(system_info_storage.IsValid());
	auto* raw_system_info = static_cast<uint64_t*>(system_info_storage.Data());
	std::memset(raw_system_info, 0, sizeof(uint64_t) * 8);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_system_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info), 0);

	GuestReadableBlock system_storage(raw_system_info[1]);
	ASSERT_TRUE(system_storage.IsValid());
	raw_system_info[0] = reinterpret_cast<uintptr_t>(system_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, system_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	GuestReadableBlock render_storage(sizeof(uint64_t) * 3);
	ASSERT_TRUE(render_storage.IsValid());
	auto* render_info = static_cast<uint64_t*>(render_storage.Data());
	render_info[0]    = 0;
	render_info[1]    = 0;
	render_info[2]    = (uint64_t {2} << 32u) | 24u;
	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, nullptr, 1), static_cast<int32_t>(0x804a0206u));
	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 0),
	          static_cast<int32_t>(0x804a0206u));
	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1),
	          static_cast<int32_t>(0x804a0207u));

	alignas(uint64_t) float output[2] = {};
	render_info[0] = reinterpret_cast<uintptr_t>(output);
	render_info[1] = sizeof(output);
	render_info[2] = (uint64_t {2} << 32u) | 24u;
	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1),
	          static_cast<int32_t>(0x804a0209u));

	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, Ngs2SystemRenderRejectsMultipleBuffersWithoutWritingThem)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock system_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(system_info_storage.IsValid());
	auto* raw_system_info = static_cast<uint64_t*>(system_info_storage.Data());
	std::memset(raw_system_info, 0, sizeof(uint64_t) * 8);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_system_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info), 0);
	GuestReadableBlock system_storage(raw_system_info[1]);
	ASSERT_TRUE(system_storage.IsValid());
	raw_system_info[0] = reinterpret_cast<uintptr_t>(system_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, system_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	constexpr size_t   kGrainBytes = 256 * 2 * sizeof(float);
	GuestReadableBlock guest_storage(kGrainBytes * 2 + sizeof(uint64_t) * 6);
	ASSERT_TRUE(guest_storage.IsValid());
	auto* bytes   = static_cast<uint8_t*>(guest_storage.Data());
	auto* output0 = reinterpret_cast<float*>(bytes);
	auto* output1 = reinterpret_cast<float*>(bytes + kGrainBytes);
	std::fill_n(output0, 256 * 2, 0.25f);
	std::fill_n(output1, 256 * 2, 0.5f);
	auto* render_infos = reinterpret_cast<uint64_t*>(bytes + kGrainBytes * 2);
	render_infos[0]    = reinterpret_cast<uintptr_t>(output0);
	render_infos[1]    = kGrainBytes;
	render_infos[2]    = (uint64_t {2} << 32u) | 24u;
	render_infos[3]    = reinterpret_cast<uintptr_t>(output1);
	render_infos[4]    = kGrainBytes;
	render_infos[5]    = (uint64_t {2} << 32u) | 24u;

	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_infos), 2),
	          static_cast<int32_t>(0x804a0206u));
	EXPECT_FLOAT_EQ(output0[0], 0.25f);
	EXPECT_FLOAT_EQ(output1[0], 0.5f);

	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, Ngs2ConcurrentDestroyRenderAndWorkspaceHandleReuse)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(info_storage.IsValid());
	auto* info_words = static_cast<uint64_t*>(info_storage.Data());
	std::memset(info_words, 0, sizeof(uint64_t) * 8);
	auto* info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(info_words);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, info), 0);

	GuestReadableBlock system_workspace(info_words[1]);
	ASSERT_TRUE(system_workspace.IsValid());
	info_words[0] = reinterpret_cast<uintptr_t>(system_workspace.Data());
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(handle_storage.IsValid());

	GuestReadableBlock render_storage(sizeof(float) * 256 * 2 + sizeof(uint64_t) * 3);
	ASSERT_TRUE(render_storage.IsValid());
	auto* output = static_cast<float*>(render_storage.Data());
	auto* render_info = reinterpret_cast<uint64_t*>(output + 256 * 2);
	render_info[0]    = reinterpret_cast<uintptr_t>(output);
	render_info[1]    = sizeof(float) * 256 * 2;
	render_info[2]    = (uint64_t {2} << 32u) | 24u;

	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, handle_storage.Data()), 0);
	const uintptr_t first_handle = *handle_storage.Data();
	ASSERT_NE(first_handle, 0u);
	GuestReadableBlock rack_option_storage(0x518);
	ASSERT_TRUE(rack_option_storage.IsValid());
	auto* rack_option = static_cast<uint8_t*>(rack_option_storage.Data());
	std::memset(rack_option, 0, 0x518);
	*reinterpret_cast<size_t*>(rack_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(rack_option + 0x50) = 1;
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* rack_info_words = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(rack_info_words, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(rack_info_words);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(rack_option), rack_info), 0);
	GuestReadableBlock rack_workspace(rack_info_words[1]);
	ASSERT_TRUE(rack_workspace.IsValid());
	rack_info_words[0] = reinterpret_cast<uintptr_t>(rack_workspace.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(first_handle, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(rack_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t first_rack_handle = *rack_handle_storage.Data();

	std::atomic_bool start {false};
	std::atomic<int> render_result {Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL};
	std::thread render_thread([&]
	                          {
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			int result = 0;
			for (uint32_t i = 0; i < 64; ++i)
			{
				result = Ngs2::Ngs2SystemRender(first_handle, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1);
				if (result != 0)
				{
					break;
				}
			}
			render_result.store(result, std::memory_order_release);
	                          });
	start.store(true, std::memory_order_release);
	const int destroy_result = Ngs2::Ngs2SystemDestroy(first_handle);
	render_thread.join();
	ASSERT_EQ(destroy_result, 0);
	EXPECT_TRUE(render_result.load(std::memory_order_acquire) == 0 ||
	            render_result.load(std::memory_order_acquire) == static_cast<int32_t>(0x804a0201u));
	EXPECT_EQ(Ngs2::Ngs2SystemRender(first_handle, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1),
	          static_cast<int32_t>(0x804a0201u));
	GuestValue<uintptr_t> old_voice_output;
	ASSERT_TRUE(old_voice_output.IsValid());
	*old_voice_output.Data() = UINTPTR_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(first_rack_handle, 0, old_voice_output.Data()), static_cast<int32_t>(0x804a0261u));
	EXPECT_EQ(*old_voice_output.Data(), 0u);

	// Reuse the identical guest workspace. The registry must attach a fresh
	// generation rather than reviving state held by the destroyed system.
	*handle_storage.Data() = 0;
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, handle_storage.Data()), 0);
	EXPECT_EQ(*handle_storage.Data(), first_handle);
	EXPECT_EQ(Ngs2::Ngs2SystemRender(*handle_storage.Data(), reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	ASSERT_EQ(Ngs2::Ngs2RackCreate(*handle_storage.Data(), 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(rack_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	EXPECT_EQ(*rack_handle_storage.Data(), first_rack_handle);
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(*rack_handle_storage.Data(), nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(*handle_storage.Data()), 0);
}

TEST(EmulatorAudio, CreatesNgs2SystemInProvidedBuffer)
{
	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(info_storage.IsValid());
	auto* raw_info = static_cast<uint64_t*>(info_storage.Data());
	std::memset(raw_info, 0, sizeof(uint64_t) * 8);
	auto* info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, info), 0);

	GuestReadableBlock storage(raw_info[1]);
	ASSERT_TRUE(storage.IsValid());
	raw_info[0]      = reinterpret_cast<uintptr_t>(storage.Data());
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(handle_storage.IsValid());

	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, handle_storage.Data()), 0);
	EXPECT_EQ(*handle_storage.Data(), reinterpret_cast<uintptr_t>(storage.Data()));
	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, nullptr, handle_storage.Data()), static_cast<int32_t>(0x804a0206u));
	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, nullptr), static_cast<int32_t>(0x804a0053u));

	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(*handle_storage.Data()), 0);
}

TEST(EmulatorAudio, Ngs2CreationKeepsGuestWorkspacesOpaque)
{
	GuestReadableBlock system_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(system_info_storage.IsValid());
	auto* system_info_words = static_cast<uint64_t*>(system_info_storage.Data());
	std::memset(system_info_words, 0, sizeof(uint64_t) * 8);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(system_info_words);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info), 0);

	GuestReadableBlock system_workspace(system_info_words[1]);
	ASSERT_TRUE(system_workspace.IsValid());
	auto* system_bytes = static_cast<uint8_t*>(system_workspace.Data());
	std::memset(system_bytes, 0xa5, system_info_words[1]);
	system_info_words[0] = reinterpret_cast<uintptr_t>(system_workspace.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, system_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();
	for (size_t i = 0; i < system_info_words[1]; ++i)
	{
		EXPECT_EQ(system_bytes[i], 0xa5);
	}

	GuestReadableBlock option_storage(0x518);
	ASSERT_TRUE(option_storage.IsValid());
	auto* option = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(option)          = 0x518;
	*reinterpret_cast<uint32_t*>(option + 0x50) = 1;
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* rack_info_words = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(rack_info_words, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(rack_info_words);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(option), rack_info), 0);

	GuestReadableBlock rack_workspace(rack_info_words[1]);
	ASSERT_TRUE(rack_workspace.IsValid());
	auto* rack_bytes = static_cast<uint8_t*>(rack_workspace.Data());
	std::memset(rack_bytes, 0xa5, rack_info_words[1]);
	rack_info_words[0] = reinterpret_cast<uintptr_t>(rack_workspace.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();
	for (size_t i = 0; i < rack_info_words[1]; ++i)
	{
		EXPECT_EQ(rack_bytes[i], 0xa5);
	}

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, RejectsNullNgs2RackHandle)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestValue<uintptr_t> voice_handle_storage;
	ASSERT_TRUE(voice_handle_storage.IsValid());
	*voice_handle_storage.Data() = UINTPTR_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(0, 0, voice_handle_storage.Data()), static_cast<int32_t>(0x804a0261u));
	EXPECT_EQ(*voice_handle_storage.Data(), 0u);
}

TEST(EmulatorAudio, ReadsGen5CustomRackVoiceCountFromCommonOptionBlock)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock option_one_storage(0x518);
	GuestReadableBlock option_two_storage(0x518);
	ASSERT_TRUE(option_one_storage.IsValid());
	ASSERT_TRUE(option_two_storage.IsValid());
	auto* raw_option_one = static_cast<uint8_t*>(option_one_storage.Data());
	auto* raw_option_two = static_cast<uint8_t*>(option_two_storage.Data());
	*reinterpret_cast<size_t*>(raw_option_one)          = 0x518;
	*reinterpret_cast<size_t*>(raw_option_two)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option_one + 0x50) = 1;
	*reinterpret_cast<uint32_t*>(raw_option_two + 0x50) = 2;

	// This field is part of the custom extension, not the common max-voices field.
	*reinterpret_cast<uint32_t*>(raw_option_one + 0xb8) = 7;
	*reinterpret_cast<uint32_t*>(raw_option_two + 0xb8) = 7;

	GuestReadableBlock info_one_storage(sizeof(uint64_t) * 8);
	GuestReadableBlock info_two_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(info_one_storage.IsValid());
	ASSERT_TRUE(info_two_storage.IsValid());
	auto* raw_info_one = static_cast<uint64_t*>(info_one_storage.Data());
	auto* raw_info_two = static_cast<uint64_t*>(info_two_storage.Data());
	std::memset(raw_info_one, 0, sizeof(uint64_t) * 8);
	std::memset(raw_info_two, 0, sizeof(uint64_t) * 8);
	auto* info_one = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info_one);
	auto* info_two = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info_two);

	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option_one), info_one), 0);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option_two), info_two), 0);
	EXPECT_GT(raw_info_two[1], raw_info_one[1]);
}

// Captured dual-strict: reverb rack 0x2001 option size 0xb8 has max_voices=0 at
// the classic +0x20 field but max_voices=16 at the Gen5 extended +0x50 field.
// Query/Create must size and index voices from +0x50 so GetVoiceHandle(id) works.
TEST(EmulatorAudio, ReadsGen5ExtendedReverbRackVoiceCountAndHandles)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock sys_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(sys_info_storage.IsValid());
	auto* raw_sys_info = static_cast<uint64_t*>(sys_info_storage.Data());
	std::memset(raw_sys_info, 0, sizeof(uint64_t) * 8);
	auto* sys_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	GuestReadableBlock sys_storage(raw_sys_info[1]);
	ASSERT_TRUE(sys_storage.IsValid());
	raw_sys_info[0] = reinterpret_cast<uintptr_t>(sys_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	// Classic max_voices at +0x20 left 0; extended field at +0x50 is the real count.
	GuestReadableBlock option_storage(0xb8);
	ASSERT_TRUE(option_storage.IsValid());
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(raw_option)          = 0xb8;
	*reinterpret_cast<uint32_t*>(raw_option + 0x20) = 0;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 16;

	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x2001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	// Must allocate room for 16 voices, not zero.
	EXPECT_GT(raw_rack_info[1], sizeof(void*) * 8u);

	GuestReadableBlock rack_storage(raw_rack_info[1]);
	ASSERT_TRUE(rack_storage.IsValid());
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x2001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();

	GuestValue<uintptr_t> voice0_storage;
	GuestValue<uintptr_t> voice15_storage;
	GuestValue<uintptr_t> voice16_storage;
	ASSERT_TRUE(voice0_storage.IsValid());
	ASSERT_TRUE(voice15_storage.IsValid());
	ASSERT_TRUE(voice16_storage.IsValid());
	*voice16_storage.Data() = UINTPTR_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice0_storage.Data()), 0);
	EXPECT_NE(*voice0_storage.Data(), 0u);
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 15, voice15_storage.Data()), 0);
	EXPECT_NE(*voice15_storage.Data(), 0u);
	EXPECT_NE(*voice15_storage.Data(), *voice0_storage.Data());
	// Out of range is a guest error, not a process exit.
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 16, voice16_storage.Data()), static_cast<int32_t>(0x804a0300u));
	EXPECT_EQ(*voice16_storage.Data(), 0u);

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, AcceptsOnlyEvidencedCustomSamplerControls)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// System buffer.
	GuestReadableBlock sys_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(sys_info_storage.IsValid());
	auto* raw_sys_info = static_cast<uint64_t*>(sys_info_storage.Data());
	std::memset(raw_sys_info, 0, sizeof(uint64_t) * 8);
	auto* sys_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	GuestReadableBlock sys_storage(raw_sys_info[1]);
	ASSERT_TRUE(sys_storage.IsValid());
	raw_sys_info[0] = reinterpret_cast<uintptr_t>(sys_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	// Gen5 custom-sampler rack option (0x518) with max_voices at offset 0x50.
	GuestReadableBlock option_storage(0x518);
	ASSERT_TRUE(option_storage.IsValid());
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(raw_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;

	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	GuestReadableBlock rack_storage(raw_rack_info[1]);
	ASSERT_TRUE(rack_storage.IsValid());
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();

	GuestValue<uintptr_t> voice_handle_storage;
	ASSERT_TRUE(voice_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice_handle_storage.Data()), 0);
	const uintptr_t voice = *voice_handle_storage.Data();
	ASSERT_NE(voice, 0u);

	// Observed frontier param: id 0x40010000 (rack class 0x4001), size 40, next 0.
	// Layout matches Ngs2VoiceParamHeader: size, next, id — then opaque payload.
	GuestReadableBlock format_storage(40);
	ASSERT_TRUE(format_storage.IsValid());
	auto* param_blob = static_cast<uint8_t*>(format_storage.Data());
	*reinterpret_cast<uint16_t*>(param_blob + 0)  = 40;
	*reinterpret_cast<int16_t*>(param_blob + 2)   = 0;
	*reinterpret_cast<uint32_t*>(param_blob + 4)  = 0x40010000u;
	*reinterpret_cast<uint32_t*>(param_blob + 8)  = 0x12u;
	*reinterpret_cast<uint32_t*>(param_blob + 12) = 2;
	*reinterpret_cast<uint32_t*>(param_blob + 16) = 44100;

	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param_blob)), 0);
	EXPECT_EQ(Ngs2::Ngs2VoiceRunCommands(voice, nullptr, 0), static_cast<int32_t>(0x804a0309u));

	// The callback class was observed, but no guest callback lifetime or dispatch
	// contract is available, so it cannot be accepted as a no-op.
	GuestReadableBlock callback_storage(32);
	ASSERT_TRUE(callback_storage.IsValid());
	auto* callback_blob = static_cast<uint8_t*>(callback_storage.Data());
	*reinterpret_cast<uint16_t*>(callback_blob + 0) = 32;
	*reinterpret_cast<int16_t*>(callback_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(callback_blob + 4) = 0x00000007u;
	*reinterpret_cast<uintptr_t*>(callback_blob + 8)  = static_cast<uintptr_t>(0x1000);
	*reinterpret_cast<uintptr_t*>(callback_blob + 16) = static_cast<uintptr_t>(0x2000);
	*reinterpret_cast<uint32_t*>(callback_blob + 24)  = 0x3u;

	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(callback_blob)),
	          static_cast<int32_t>(0x804a0309u));

	// The class was observed, but its guest-visible effect remains unmeasured.
	GuestReadableBlock custom_module_storage(48);
	ASSERT_TRUE(custom_module_storage.IsValid());
	auto* custom_module_blob = static_cast<uint8_t*>(custom_module_storage.Data());
	*reinterpret_cast<uint16_t*>(custom_module_blob + 0) = 48;
	*reinterpret_cast<int16_t*>(custom_module_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(custom_module_blob + 4) = 0x40001300u;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(custom_module_blob)),
	          static_cast<int32_t>(0x804a0309u));

	// Additional custom-sampler module controls are rejected until their
	// guest-visible semantics are captured.
	GuestReadableBlock opaque_sampler_storage(8);
	ASSERT_TRUE(opaque_sampler_storage.IsValid());
	auto* opaque_sampler_blob = static_cast<uint8_t*>(opaque_sampler_storage.Data());
	*reinterpret_cast<uint16_t*>(opaque_sampler_blob + 0) = 8;
	*reinterpret_cast<uint32_t*>(opaque_sampler_blob + 4) = 0x40010005u;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(opaque_sampler_blob)),
	          static_cast<int32_t>(0x804a0309u));

	// Observed GetState size 48 for CustomSampler (same block as Sampler, not 80).
	GuestReadableBlock state_storage(48 + sizeof(uint32_t));
	ASSERT_TRUE(state_storage.IsValid());
	auto* state_blob = static_cast<uint8_t*>(state_storage.Data());
	std::memset(state_blob, 0xa5, 48);
	EXPECT_EQ(Ngs2::Ngs2VoiceGetState(voice, reinterpret_cast<Ngs2::Ngs2VoiceState*>(state_blob), 48), 0);
	// state_flags at offset 0 should be written (Empty → 0).
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(state_blob), 0u);

	auto* state_flags = reinterpret_cast<uint32_t*>(state_blob + 48);
	*state_flags      = UINT32_MAX;
	EXPECT_EQ(Ngs2::Ngs2VoiceGetStateFlags(voice, state_flags), 0);
	EXPECT_EQ(*state_flags, 0u);

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, RendersCapturedCustomSamplerPcmIntoStereoGrain)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock sys_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(sys_info_storage.IsValid());
	auto* raw_sys_info = static_cast<uint64_t*>(sys_info_storage.Data());
	std::memset(raw_sys_info, 0, sizeof(uint64_t) * 8);
	auto* sys_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	GuestReadableBlock sys_storage(raw_sys_info[1]);
	ASSERT_TRUE(sys_storage.IsValid());
	raw_sys_info[0] = reinterpret_cast<uintptr_t>(sys_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	GuestReadableBlock option_storage(0x518);
	ASSERT_TRUE(option_storage.IsValid());
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(raw_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	GuestReadableBlock rack_storage(raw_rack_info[1]);
	ASSERT_TRUE(rack_storage.IsValid());
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();
	GuestValue<uintptr_t> voice_handle_storage;
	ASSERT_TRUE(voice_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice_handle_storage.Data()), 0);
	const uintptr_t voice = *voice_handle_storage.Data();

	GuestReadableBlock format_storage(40);
	ASSERT_TRUE(format_storage.IsValid());
	auto* format_param = static_cast<uint8_t*>(format_storage.Data());
	*reinterpret_cast<uint16_t*>(format_param + 0)  = 40;
	*reinterpret_cast<uint32_t*>(format_param + 4)  = 0x40010000u;
	*reinterpret_cast<uint32_t*>(format_param + 8)  = 0x12u;
	*reinterpret_cast<uint32_t*>(format_param + 12) = 1;
	*reinterpret_cast<uint32_t*>(format_param + 16) = 44100;
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(format_param)), 0);

	constexpr uint64_t kSourceFrames = 512;
	GuestReadableBlock pcm_storage(sizeof(int16_t) * kSourceFrames + sizeof(uint64_t) * 5);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* source = static_cast<int16_t*>(pcm_storage.Data());
	for (uint64_t i = 0; i < kSourceFrames; i++)
	{
		source[i] = 16384;
	}
	auto* waveform_context = reinterpret_cast<uint64_t*>(source + kSourceFrames);
	waveform_context[0]    = 0;
	waveform_context[1]    = sizeof(int16_t) * kSourceFrames;
	waveform_context[2]    = 0;
	waveform_context[3]    = kSourceFrames;
	waveform_context[4]    = 0;
	GuestReadableBlock waveform_param_storage(32);
	ASSERT_TRUE(waveform_param_storage.IsValid());
	auto* waveform_param = static_cast<uint8_t*>(waveform_param_storage.Data());
	*reinterpret_cast<uint16_t*>(waveform_param + 0)   = 32;
	*reinterpret_cast<uint32_t*>(waveform_param + 4)   = 0x40010001u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 8)  = reinterpret_cast<uintptr_t>(source);
	*reinterpret_cast<uint32_t*>(waveform_param + 16)  = 0x11u;
	*reinterpret_cast<uint32_t*>(waveform_param + 20)  = 1u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 24) = reinterpret_cast<uintptr_t>(waveform_context);
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)), 0);

	// VoiceControl must snapshot the evidenced PCM block. The guest may reuse
	// its source memory before the later render call.
	std::fill_n(source, kSourceFrames, int16_t {0});

	GuestReadableBlock command_storage(sizeof(uint32_t) * 3);
	ASSERT_TRUE(command_storage.IsValid());
	auto* play_command = static_cast<uint32_t*>(command_storage.Data());
	play_command[0]    = 2;
	play_command[1]    = 0x400;
	play_command[2]    = 1;
	ASSERT_EQ(Ngs2::Ngs2VoiceRunCommands(voice, play_command, 1), 0);

	GuestReadableBlock render_storage(sizeof(float) * 256 * 2 + sizeof(uint64_t) * 3);
	ASSERT_TRUE(render_storage.IsValid());
	auto* output = static_cast<float*>(render_storage.Data());
	auto* render_info = reinterpret_cast<uint64_t*>(output + 256 * 2);
	render_info[0]    = reinterpret_cast<uintptr_t>(output);
	render_info[1]    = sizeof(float) * 256 * 2;
	render_info[2]    = (uint64_t {2} << 32u) | 24u;
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_NE(output[0], 0.0f);
	EXPECT_FLOAT_EQ(output[0], output[1]);

	GuestReadableBlock destroyed_rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(destroyed_rack_info_storage.IsValid());
	auto* destroyed_rack_info = static_cast<uint64_t*>(destroyed_rack_info_storage.Data());
	std::memset(destroyed_rack_info, 0, sizeof(uint64_t) * 8);
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(destroyed_rack_info)), 0);
	EXPECT_EQ(destroyed_rack_info[0], rack);
	EXPECT_EQ(destroyed_rack_info[1], raw_rack_info[1]);

	std::fill_n(output, 256 * 2, 1.0f);
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_FLOAT_EQ(output[0], 0.0f);
	EXPECT_FLOAT_EQ(output[1], 0.0f);

	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, Ngs2CustomSamplerRejectsUnreadablePcmRange)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock sys_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(sys_info_storage.IsValid());
	auto* raw_sys_info = static_cast<uint64_t*>(sys_info_storage.Data());
	std::memset(raw_sys_info, 0, sizeof(uint64_t) * 8);
	auto* sys_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	GuestReadableBlock sys_storage(raw_sys_info[1]);
	ASSERT_TRUE(sys_storage.IsValid());
	raw_sys_info[0] = reinterpret_cast<uintptr_t>(sys_storage.Data());
	GuestValue<uintptr_t> system_handle_storage;
	ASSERT_TRUE(system_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, system_handle_storage.Data()), 0);
	const uintptr_t system = *system_handle_storage.Data();

	GuestReadableBlock option_storage(0x518);
	ASSERT_TRUE(option_storage.IsValid());
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	*reinterpret_cast<size_t*>(raw_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	ASSERT_TRUE(rack_info_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	GuestReadableBlock rack_storage(raw_rack_info[1]);
	ASSERT_TRUE(rack_storage.IsValid());
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
	GuestValue<uintptr_t> rack_handle_storage;
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               rack_handle_storage.Data()),
	          0);
	const uintptr_t rack = *rack_handle_storage.Data();
	GuestValue<uintptr_t> voice_handle_storage;
	ASSERT_TRUE(voice_handle_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice_handle_storage.Data()), 0);
	const uintptr_t voice = *voice_handle_storage.Data();

	GuestReadableBlock format_storage(40);
	ASSERT_TRUE(format_storage.IsValid());
	auto* format_param = static_cast<uint8_t*>(format_storage.Data());
	*reinterpret_cast<uint16_t*>(format_param + 0)  = 40;
	*reinterpret_cast<uint32_t*>(format_param + 4)  = 0x40010000u;
	*reinterpret_cast<uint32_t*>(format_param + 8)  = 0x12u;
	*reinterpret_cast<uint32_t*>(format_param + 12) = 1;
	*reinterpret_cast<uint32_t*>(format_param + 16) = 44100;
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(format_param)), 0);

	GuestReadableBlock waveform_context_storage(sizeof(uint64_t) * 5);
	ASSERT_TRUE(waveform_context_storage.IsValid());
	auto* waveform_context = static_cast<uint64_t*>(waveform_context_storage.Data());
	waveform_context[0]    = 0;
	waveform_context[1]    = sizeof(int16_t);
	waveform_context[2]    = 0;
	waveform_context[3]    = 1;
	waveform_context[4]    = 0;
	GuestReadableBlock waveform_param_storage(32);
	ASSERT_TRUE(waveform_param_storage.IsValid());
	auto* waveform_param = static_cast<uint8_t*>(waveform_param_storage.Data());
	*reinterpret_cast<uint16_t*>(waveform_param + 0)   = 32;
	*reinterpret_cast<uint32_t*>(waveform_param + 4)   = 0x40010001u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 8)  = 1;
	*reinterpret_cast<uint32_t*>(waveform_param + 16)  = 0x11u;
	*reinterpret_cast<uint32_t*>(waveform_param + 20)  = 1u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 24) = reinterpret_cast<uintptr_t>(waveform_context);
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)),
	          static_cast<int32_t>(0x804a0309u));

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}

TEST(EmulatorAudio, AvPlayerInitExReturnsZeroAndPopulatesHandle)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	AvPlayer::AvPlayerInitDataEx init_ex {};
	init_ex.this_size                     = sizeof(init_ex);
	init_ex.num_output_video_framebuffers = 2;

	AvPlayer::AvPlayerInternal* handle = nullptr;
	ASSERT_EQ(AvPlayer::AvPlayerInitEx(&init_ex, &handle), 0);
	ASSERT_NE(handle, nullptr);
	EXPECT_EQ(AvPlayer::AvPlayerClose(handle), 0);
}

// Written by the player's event callback, possibly from its own thread.
static std::atomic<int> g_avplayer_test_ready_events {0};
static std::atomic<int> g_avplayer_test_stop_events {0};

static void KYTY_SYSV_ABI avplayer_test_event_cb(void* obj_ptr, uint32_t event_id, int32_t source_id, void* data)
{
	(void)obj_ptr;
	(void)source_id;
	(void)data;
	if (event_id == 0x02)
	{
		g_avplayer_test_ready_events++;
	} else if (event_id == 0x01)
	{
		g_avplayer_test_stop_events++;
	}
}

TEST(EmulatorAudio, AvPlayerSanitizesFileUriAndFiresEvents)
{
	const char* media_path = std::getenv("KYTY_AVPLAYER_TEST_MEDIA");
	if (media_path == nullptr || media_path[0] == '\0' || !::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	g_avplayer_test_ready_events.store(0);
	g_avplayer_test_stop_events.store(0);

	// The event and file callbacks run through the host runtime dispatch; the file
	// callback supplies the owned fixture, so the encoded URI only exercises the
	// player's sanitizer, not the guest filesystem.
	HostFileReplacement host_file;
	ScopedHostRuntime   runtime;

	AvPlayer::AvPlayerInitDataEx init_ex {};
	init_ex.this_size                        = sizeof(init_ex);
	init_ex.file_replacement                 = MakeHostFileReplacement(&host_file);
	init_ex.event_replacement.event_callback = avplayer_test_event_cb;
	init_ex.num_output_video_framebuffers    = 2;

	AvPlayer::AvPlayerInternal* handle = nullptr;
	ASSERT_EQ(AvPlayer::AvPlayerInitEx(&init_ex, &handle), 0);
	ASSERT_NE(handle, nullptr);

	AvPlayer::AvPlayerSourceDetails details {};
	std::string encoded_url = "file://";
	for (const char character: std::string(media_path))
	{
		if (character == ' ')
		{
			encoded_url += "%20";
		} else
		{
			encoded_url += character;
		}
	}
	details.uri.name   = encoded_url.c_str();
	details.uri.length = static_cast<uint32_t>(encoded_url.size());
	details.source_type = AvPlayer::AvPlayerSourceFileMp4;

	ASSERT_EQ(AvPlayer::AvPlayerAddSourceEx(handle, AvPlayer::AvPlayerUriTypeSource, &details), 0);
	// Functional sanitizer contract: the file:// prefix is removed and %20 is
	// decoded, so the file callback receives exactly the original host name.
	EXPECT_EQ(host_file.opened_uri, std::string(media_path));
	EXPECT_EQ(g_avplayer_test_ready_events.load(), 1);
	EXPECT_EQ(AvPlayer::AvPlayerStreamCount(handle), 2);

	AvPlayer::AvPlayerStreamInfoEx video_stream {};
	ASSERT_EQ(AvPlayer::AvPlayerGetStreamInfoEx(handle, 0, &video_stream), 0);
	EXPECT_EQ(video_stream.type, AvPlayer::AvPlayerStreamVideo);
	// Dimensions are read from the fixture's stream, not hardcoded.
	const uint32_t stream_width  = video_stream.details.video.width;
	const uint32_t stream_height = video_stream.details.video.height;
	EXPECT_GT(stream_width, 0u);
	EXPECT_GT(stream_height, 0u);

	ASSERT_EQ(AvPlayer::AvPlayerStart(handle), 0);
	EXPECT_EQ(AvPlayer::AvPlayerIsActive(handle), 1);

	// Decoding is asynchronous: a pull that returns "not ready" is not the end of
	// the stream. Poll until the player reports itself inactive, pulling video and
	// audio so neither bounded decode queue holds the other back. The bound is the
	// stream duration plus a margin, capped at 10 seconds (the owned fixture is 6 s).
	const uint64_t duration_ms = video_stream.duration;
	const auto     budget      = std::chrono::milliseconds(duration_ms == 0 ? 10000 : std::min<uint64_t>(duration_ms + 3000, 10000));
	const auto     deadline    = std::chrono::steady_clock::now() + budget;
	AvPlayer::AvPlayerFrameInfoEx frame {};
	AvPlayer::AvPlayerFrameInfo   audio {};
	uint32_t                      frame_count = 0;
	while (AvPlayer::AvPlayerIsActive(handle) != 0 && std::chrono::steady_clock::now() < deadline)
	{
		bool progressed = false;
		if (AvPlayer::AvPlayerGetVideoDataEx(handle, &frame) != 0)
		{
			EXPECT_EQ(frame.details.video.width, stream_width);
			EXPECT_EQ(frame.details.video.height, stream_height);
			frame_count++;
			progressed = true;
		}
		if (AvPlayer::AvPlayerGetAudioData(handle, &audio) != 0)
		{
			progressed = true;
		}
		if (!progressed)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	// The end event may follow the inactive state from the player's thread.
	while (g_avplayer_test_stop_events.load() == 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool reached_end = AvPlayer::AvPlayerIsActive(handle) == 0;
	if (!reached_end)
	{
		// Reclaim only this player on timeout, then fail.
		ADD_FAILURE() << "player still active after " << budget.count() << " ms";
		EXPECT_EQ(AvPlayer::AvPlayerStop(handle), 0);
	}

	EXPECT_TRUE(reached_end);
	EXPECT_GT(frame_count, 0u);
	EXPECT_EQ(g_avplayer_test_stop_events.load(), 1);

	EXPECT_EQ(AvPlayer::AvPlayerClose(handle), 0);
}

TEST(EmulatorAudio, AudioOutRejectsUnknownFormatsAndNullPointersBeforeTouchingTheHost)
{
	// 99 is not one of the eight PCM/float layouts: a format error, not a full port table.
	EXPECT_EQ(AudioOut::AudioOutOpen(255, 0, 0, 256, 48000, 99u), AUDIO_OUT_ERROR_INVALID_FORMAT);
	// A null volume array is refused instead of being dereferenced by the port.
	EXPECT_EQ(AudioOut::AudioOutSetVolume(1, 0u, nullptr), AUDIO_OUT_ERROR_INVALID_POINTER);
}


namespace {

// Standard sampler red fixture. Layouts and IDs are candidates that are not
// yet verified on a guest.
constexpr uint32_t kSamplerRackId         = 0x1000;
constexpr size_t   kSamplerOptionSize     = 0xd8;
constexpr uint32_t kSamplerGrainFrames    = 256;
constexpr uint32_t kSamplerSetupId        = 0x10000000u;
constexpr uint32_t kSamplerBlocksId       = 0x10000001u;
constexpr uint32_t kSamplerPlayEvent      = 1;
constexpr uint32_t kCallbackParamId       = 0x7u;
constexpr uint32_t kCallbackFlagEnd       = 0x1u;
constexpr uint32_t kCallbackFlagRepeat    = 0x2u;
// The blocks flag a native call site passes for a one-shot waveform (reset).
constexpr uint32_t kBlocksFlagReset       = 0x4u;
constexpr uint32_t kWaveformPcmI16        = 0x12u;
constexpr uint32_t kWaveformPcmF32        = 0x18u;
constexpr uint32_t kMaxSamplerCallbacks   = 4;
constexpr int32_t  kSamplerInvalidControl = static_cast<int32_t>(0x804a0309u);

// Ngs2WaveformBlock: 40 bytes, repeat count at +0x10.
struct SamplerBlock
{
	uint64_t  data_offset;
	uint64_t  data_size;
	uint32_t  num_repeats;
	uint32_t  num_skip_samples;
	uint32_t  num_samples;
	uint32_t  reserved;
	uintptr_t user_data;
};
static_assert(sizeof(SamplerBlock) == 40);

struct SamplerHeaderParam
{
	uint16_t size;
	int16_t  next;
	uint32_t id;
};
static_assert(sizeof(SamplerHeaderParam) == 8);

// Setup param: header, the 24-byte format at +8, then a caller word at +0x20 and
// a zero word at +0x24. The guest's builder writes size 0x28 (40).
struct SamplerSetupParam
{
	SamplerHeaderParam header;
	uint32_t           waveform_type;
	uint32_t           num_channels;
	uint32_t           sample_rate;
	uint32_t           config_data;
	uint32_t           frame_offset;
	uint32_t           frame_margin;
	uint32_t           flags;
	uint32_t           reserved;
};
static_assert(sizeof(SamplerSetupParam) == 40);

struct SamplerBlocksParam
{
	SamplerHeaderParam  header;
	const void*         data;
	uint32_t            flags;
	uint32_t            num_blocks;
	const SamplerBlock* blocks;
};
static_assert(sizeof(SamplerBlocksParam) == 32);

struct SamplerCallbackParam
{
	SamplerHeaderParam header;
	uintptr_t          callback;
	uintptr_t          callback_data;
	uint32_t           flags;
	uint32_t           reserved;
};
static_assert(sizeof(SamplerCallbackParam) == 32);

// Ngs2VoiceCallbackInfo: 56 bytes.
struct SamplerCallbackInfo
{
	uintptr_t   callback_data;
	uintptr_t   voice_handle;
	uint32_t    flag;
	uint32_t    reserved;
	uintptr_t   user_data;
	const void* block_data;
	uint32_t    block_size;
	uint32_t    num_repeated;
	uint32_t    attributes;
	uint32_t    reserved2;
};
static_assert(sizeof(SamplerCallbackInfo) == 56);

struct SamplerCallbackRecord
{
	uint32_t  flag         = 0;
	uint32_t  num_repeated = 0;
	uintptr_t user_data    = 0;
	uint32_t  block_size   = 0;
};

SamplerCallbackRecord g_sampler_callbacks[kMaxSamplerCallbacks] {};
uint32_t              g_sampler_callback_count = 0;

void KYTY_SYSV_ABI SamplerRecordCallback(const SamplerCallbackInfo* info)
{
	if (info != nullptr && g_sampler_callback_count < kMaxSamplerCallbacks)
	{
		g_sampler_callbacks[g_sampler_callback_count++] = {info->flag, info->num_repeated, info->user_data, info->block_size};
	}
}

// Stands in for the guest runtime: the handler receives the info pointer.
uint64_t KYTY_SYSV_ABI SamplerInvokeHandler(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	(void)arg1;
	(void)arg2;
	reinterpret_cast<void(KYTY_SYSV_ABI*)(const SamplerCallbackInfo*)>(target)(reinterpret_cast<const SamplerCallbackInfo*>(arg0));
	return 0;
}

// Owns a sampler system, its rack and the first voice. The rack and system are
// destroyed before their guest workspaces are released.
struct SamplerRig
{
	std::unique_ptr<GuestReadableBlock> system_workspace;
	std::unique_ptr<GuestReadableBlock> rack_workspace;
	uintptr_t                           system = 0;
	uintptr_t                           rack   = 0;
	uintptr_t                           voice  = 0;

	SamplerRig()                             = default;
	SamplerRig(const SamplerRig&)            = delete;
	SamplerRig& operator=(const SamplerRig&) = delete;
	~SamplerRig()
	{
		if (rack != 0)
		{
			(void)Ngs2::Ngs2RackDestroy(rack, nullptr);
		}
		if (system != 0)
		{
			(void)Ngs2::Ngs2SystemDestroy(system);
		}
	}
};

bool CreateSamplerRigVoices(SamplerRig* rig, uint32_t voice_count)
{
	GuestReadableBlock    option_storage(kSamplerOptionSize);
	GuestReadableBlock    system_info_storage(sizeof(uint64_t) * 8);
	GuestReadableBlock    rack_info_storage(sizeof(uint64_t) * 8);
	GuestValue<uintptr_t> system_handle_storage;
	GuestValue<uintptr_t> rack_handle_storage;
	GuestValue<uintptr_t> voice_handle_storage;
	if (!option_storage.IsValid() || !system_info_storage.IsValid() || !rack_info_storage.IsValid() || !system_handle_storage.IsValid() ||
	    !rack_handle_storage.IsValid() || !voice_handle_storage.IsValid())
	{
		return false;
	}

	// Candidate option layout: byte size at +0 and max_voices at +0x50.
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	std::memset(raw_option, 0, kSamplerOptionSize);
	*reinterpret_cast<size_t*>(raw_option)          = kSamplerOptionSize;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = voice_count;
	const auto* option                              = reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option);

	auto* raw_system_info = static_cast<uint64_t*>(system_info_storage.Data());
	std::memset(raw_system_info, 0, sizeof(uint64_t) * 8);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_system_info);
	if (Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info) != 0)
	{
		return false;
	}
	rig->system_workspace = std::make_unique<GuestReadableBlock>(raw_system_info[1]);
	if (!rig->system_workspace->IsValid())
	{
		return false;
	}
	raw_system_info[0] = reinterpret_cast<uintptr_t>(rig->system_workspace->Data());
	if (Ngs2::Ngs2SystemCreate(nullptr, system_info, system_handle_storage.Data()) != 0)
	{
		return false;
	}
	rig->system = *system_handle_storage.Data();

	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	if (Ngs2::Ngs2RackQueryBufferSize(kSamplerRackId, option, rack_info) != 0)
	{
		return false;
	}
	rig->rack_workspace = std::make_unique<GuestReadableBlock>(raw_rack_info[1]);
	if (!rig->rack_workspace->IsValid())
	{
		return false;
	}
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rig->rack_workspace->Data());
	if (Ngs2::Ngs2RackCreate(rig->system, kSamplerRackId, option, rack_info, rack_handle_storage.Data()) != 0)
	{
		return false;
	}
	rig->rack = *rack_handle_storage.Data();
	if (Ngs2::Ngs2RackGetVoiceHandle(rig->rack, 0, voice_handle_storage.Data()) != 0)
	{
		return false;
	}
	rig->voice = *voice_handle_storage.Data();
	return rig->voice != 0;
}

bool CreateSamplerRig(SamplerRig* rig)
{
	return CreateSamplerRigVoices(rig, 1);
}

// Every parameter is copied into guest-readable storage first, as the guest does.
template <typename T> int32_t VoiceControlWith(uintptr_t voice, const T& param)
{
	GuestReadableBlock storage(sizeof(T));
	if (!storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	*static_cast<T*>(storage.Data()) = param;
	return Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(storage.Data()));
}

SamplerHeaderParam MakeSamplerHeader(size_t size, uint32_t id)
{
	return {static_cast<uint16_t>(size), 0, id};
}

int32_t SetSamplerFormat(uintptr_t voice, uint32_t waveform_type, uint32_t channels, uint32_t sample_rate, uint32_t frame_offset = 0)
{
	SamplerSetupParam param {};
	param.header        = MakeSamplerHeader(sizeof(param), kSamplerSetupId);
	param.waveform_type = waveform_type;
	param.num_channels  = channels;
	param.sample_rate   = sample_rate;
	param.frame_offset  = frame_offset;
	return VoiceControlWith(voice, param);
}

int32_t AddSamplerBlocks(uintptr_t voice, const void* data, uint32_t flags, const SamplerBlock* blocks, uint32_t count)
{
	GuestReadableBlock block_storage(sizeof(SamplerBlock) * count);
	if (!block_storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	std::memcpy(block_storage.Data(), blocks, sizeof(SamplerBlock) * count);
	SamplerBlocksParam param {};
	param.header     = MakeSamplerHeader(sizeof(param), kSamplerBlocksId);
	param.data       = data;
	param.flags      = flags;
	param.num_blocks = count;
	param.blocks     = static_cast<const SamplerBlock*>(block_storage.Data());
	return VoiceControlWith(voice, param);
}

int32_t AddSamplerBlock(uintptr_t voice, const void* data, uint32_t flags, const SamplerBlock& block)
{
	return AddSamplerBlocks(voice, data, flags, &block, 1);
}

int32_t SetSamplerCallbackHandler(uintptr_t voice, uintptr_t handler, uint32_t flags)
{
	SamplerCallbackParam param {};
	param.header        = MakeSamplerHeader(sizeof(param), kCallbackParamId);
	param.callback      = handler;
	param.callback_data = 0x1234;
	param.flags         = flags;
	return VoiceControlWith(voice, param);
}

int32_t SetSamplerCallback(uintptr_t voice, uint32_t flags)
{
	return SetSamplerCallbackHandler(voice, reinterpret_cast<uintptr_t>(&SamplerRecordCallback), flags);
}

int32_t RunSamplerEvent(uintptr_t voice, uint32_t event)
{
	GuestReadableBlock command_storage(sizeof(uint32_t) * 3);
	if (!command_storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* command = static_cast<uint32_t*>(command_storage.Data());
	command[0]    = 2;     // event command
	command[1]    = 0x400; // unsigned value type
	command[2]    = event;
	return Ngs2::Ngs2VoiceRunCommands(voice, command, 1);
}

// Renders one system grain of stereo float into guest memory.
int32_t RenderSamplerGrain(uintptr_t system, void* output)
{
	GuestReadableBlock info_storage(sizeof(uint64_t) * 3);
	if (!info_storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* info = static_cast<uint64_t*>(info_storage.Data());
	info[0]    = reinterpret_cast<uintptr_t>(output);
	info[1]    = sizeof(float) * kSamplerGrainFrames * 2;
	info[2]    = (uint64_t {2} << 32u) | kWaveformPcmF32;
	return Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(info), 1);
}

} // namespace

TEST(EmulatorAudio, StandardSamplerRendersLinearlyResampledPcm)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));

	// Mono ramp at 24 kHz: sample i is i / 8 of full scale.
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 8);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	for (int16_t i = 0; i < 8; i++)
	{
		pcm[i] = static_cast<int16_t>(i * 4096);
	}
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 24000), 0);
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * 8;
	block.num_samples = 8;
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());

	// Output frame k reads source position k / 2. Odd frames interpolate toward
	// the next sample; the last sample has no successor and repeats itself.
	for (uint32_t k = 0; k < 16; k++)
	{
		const auto  half     = static_cast<float>(k / 2);
		const float expected = (k % 2 == 1 && k < 15 ? half + 0.5f : half) * 0.125f;
		EXPECT_NEAR(output[2 * k], expected, 1e-5f);
		EXPECT_NEAR(output[2 * k + 1], expected, 1e-5f);
	}
	// The voice ends after its last sample, so the rest of the grain is silent.
	for (uint32_t k = 16; k < kSamplerGrainFrames; k++)
	{
		EXPECT_EQ(output[2 * k], 0.0f);
		EXPECT_EQ(output[2 * k + 1], 0.0f);
	}
}

TEST(EmulatorAudio, StandardSamplerRefusesRepeatCallbacksAndReportsTheEndAfterRepeats)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));

	// Four samples at the system rate, played twice before the block ends.
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 4);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	pcm[0]    = 1000;
	pcm[1]    = 2000;
	pcm[2]    = 3000;
	pcm[3]    = 4000;
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000), 0);
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * 4;
	block.num_repeats = 1;
	block.num_samples = 4;
	block.user_data   = 0x5a5a;
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, block), 0);

	// The repeat flag and its callback layout are not confirmed on a guest, so a
	// registration asking for it is refused rather than accepted and dropped.
	EXPECT_EQ(SetSamplerCallback(rig.voice, kCallbackFlagEnd | kCallbackFlagRepeat), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerCallback(rig.voice, kCallbackFlagRepeat), kSamplerInvalidControl);
	ASSERT_EQ(SetSamplerCallback(rig.voice, kCallbackFlagEnd), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	::Kyty::Emulator::GuestRuntimePort::Provider provider {};
	provider.invoke = SamplerInvokeHandler;
	::Kyty::Emulator::GuestRuntimePort::Install(provider);
	g_sampler_callback_count    = 0;
	const int32_t play_result   = RunSamplerEvent(rig.voice, kSamplerPlayEvent);
	const int32_t render_result = RenderSamplerGrain(rig.system, output_storage.Data());
	::Kyty::Emulator::GuestRuntimePort::Install({});
	ASSERT_EQ(play_result, 0);
	ASSERT_EQ(render_result, 0);

	// The repeat pass plays; the end of the block is reported once. flag (+0x10)
	// and user data (+0x18) are at confirmed offsets. num_repeated (+0x2c) and
	// block_size (+0x28) sit at inferred classic offsets: this checks Kyty's own
	// round-trip of that layout, not a confirmed native ABI.
	ASSERT_EQ(g_sampler_callback_count, 1u);
	EXPECT_EQ(g_sampler_callbacks[0].flag, kCallbackFlagEnd);
	EXPECT_EQ(g_sampler_callbacks[0].num_repeated, 1u);
	EXPECT_EQ(g_sampler_callbacks[0].user_data, 0x5a5au);
	EXPECT_EQ(g_sampler_callbacks[0].block_size, sizeof(int16_t) * 4);

	const auto* output = static_cast<const float*>(output_storage.Data());
	for (uint32_t k = 0; k < 8; k++)
	{
		const float expected = static_cast<float>(pcm[k % 4]) / 32768.0f;
		EXPECT_NEAR(output[2 * k], expected, 1e-5f);
		EXPECT_NEAR(output[2 * k + 1], expected, 1e-5f);
	}
	EXPECT_EQ(output[2 * 8], 0.0f);
}

TEST(EmulatorAudio, StandardSamplerRejectsUnsupportedContractsWithoutChangingVoice)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));

	GuestReadableBlock pcm_storage(sizeof(int16_t) * 4);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	pcm[0]    = 1000;
	pcm[1]    = 2000;
	pcm[2]    = 3000;
	pcm[3]    = 4000;
	SamplerBlock valid {};
	valid.data_size   = sizeof(int16_t) * 4;
	valid.num_samples = 4;

	// Setup accepts PCM16 and PCM float with one or two channels, a nonzero rate,
	// no frame offset and zero trailing words.
	EXPECT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmF32, 3, 48000), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerFormat(rig.voice, 0x13u, 1, 48000), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 0, 48000), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 3, 48000), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 0), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000, 16), kSamplerInvalidControl);
	SamplerSetupParam flagged {};
	flagged.header        = MakeSamplerHeader(sizeof(flagged), kSamplerSetupId);
	flagged.waveform_type = kWaveformPcmI16;
	flagged.num_channels  = 1;
	flagged.sample_rate   = 48000;
	flagged.flags         = 1;
	EXPECT_EQ(VoiceControlWith(rig.voice, flagged), kSamplerInvalidControl);

	// Blocks need a configured format.
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, valid), kSamplerInvalidControl);
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000), 0);

	// Only the reset flag has a native caller; other flag values are refused.
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), 0, valid), kSamplerInvalidControl);
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), 0x1, valid), kSamplerInvalidControl);
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), 0x2, valid), kSamplerInvalidControl);
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset | 0x1, valid), kSamplerInvalidControl);

	// Blocks must be non-empty spans inside readable data.
	SamplerBlock empty = valid;
	empty.num_samples  = 0;
	empty.data_size    = 0;
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, empty), kSamplerInvalidControl);
	SamplerBlock overrun = valid;
	overrun.num_samples  = 5;
	EXPECT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, overrun), kSamplerInvalidControl);
	EXPECT_EQ(AddSamplerBlock(rig.voice, reinterpret_cast<const void*>(uintptr_t {1}), kBlocksFlagReset, valid), kSamplerInvalidControl);

	// Play without queued samples fails.
	EXPECT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), kSamplerInvalidControl);

	// Rejected calls leave the queue unchanged: the one valid block plays exactly.
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, valid), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());
	for (uint32_t k = 0; k < 4; k++)
	{
		EXPECT_NEAR(output[2 * k], static_cast<float>(pcm[k]) / 32768.0f, 1e-5f);
	}
	EXPECT_EQ(output[2 * 4], 0.0f);

	// Unknown sampler controls fail instead of being ignored.
	const SamplerHeaderParam unknown = MakeSamplerHeader(sizeof(SamplerHeaderParam), 0x10000002u);
	EXPECT_EQ(VoiceControlWith(rig.voice, unknown), kSamplerInvalidControl);
}

namespace {

namespace AudioVideoBackend = ::Kyty::Emulator::AudioVideoBackend;

constexpr uint32_t kSamplerPitchId = 0x10000005u;
constexpr uint32_t kWaveformAtrac9 = 0x40u;
// Mono, 48 kHz (rate index 7), 256-byte frames, four frames per superframe. The
// decoder derives 1024 samples per 1024-byte superframe from it.
constexpr uint8_t  kSyntheticAtrac9Config[4]  = {0xfe, 0x70, 0x1f, 0xf0};
constexpr uint32_t kSyntheticAtrac9ConfigData = 0xf01f70feu; // config bytes as a little-endian word
constexpr uint32_t kSyntheticAtrac9Rate       = 48000;
constexpr uint32_t kSyntheticAtrac9Superframe = 1024;

struct SamplerPitchParam
{
	SamplerHeaderParam header;
	float              ratio;
	uint32_t           reserved;
};
static_assert(sizeof(SamplerPitchParam) == 16);

int32_t SetSamplerPitch(uintptr_t voice, float ratio)
{
	SamplerPitchParam param {};
	param.header = MakeSamplerHeader(sizeof(param), kSamplerPitchId);
	param.ratio  = ratio;
	return VoiceControlWith(voice, param);
}

int32_t RunSamplerGain(uintptr_t voice, float gain)
{
	GuestReadableBlock command_storage(sizeof(uint32_t) * 3);
	if (!command_storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* command = static_cast<uint32_t*>(command_storage.Data());
	command[0]    = 6;     // port volume
	command[1]    = 0x100; // float value type
	std::memcpy(&command[2], &gain, sizeof(gain));
	return Ngs2::Ngs2VoiceRunCommands(voice, command, 1);
}

void AppendLe(std::vector<uint8_t>* out, uint64_t value, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++)
	{
		out->push_back(static_cast<uint8_t>(value >> (8u * i)));
	}
}

void AppendBytes(std::vector<uint8_t>* out, const void* data, size_t size)
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	out->insert(out->end(), bytes, bytes + size);
}

// RIFF/WAVE with an ATRAC9 extensible fmt (52 bytes), a fact chunk and an empty
// data chunk. The 12-byte format tail is version, config, reserved.
std::vector<uint8_t> BuildSyntheticAtrac9Riff(uint32_t channels, uint32_t rate, uint32_t fact_samples)
{
	const uint8_t kGuid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	std::vector<uint8_t> riff;
	AppendBytes(&riff, "RIFF", 4);
	AppendLe(&riff, 0, 4);
	AppendBytes(&riff, "WAVE", 4);
	AppendBytes(&riff, "fmt ", 4);
	AppendLe(&riff, 52, 4);
	AppendLe(&riff, 0xfffe, 2);
	AppendLe(&riff, channels, 2);
	AppendLe(&riff, rate, 4);
	AppendLe(&riff, 0, 4);
	AppendLe(&riff, kSyntheticAtrac9Superframe, 2);
	AppendLe(&riff, 0, 2);
	AppendLe(&riff, 34, 2);
	AppendLe(&riff, kSyntheticAtrac9Superframe, 2);
	AppendLe(&riff, 0, 4);
	AppendBytes(&riff, kGuid, sizeof(kGuid));
	AppendLe(&riff, 0, 4);
	AppendBytes(&riff, kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	AppendLe(&riff, 0, 4);
	AppendBytes(&riff, "fact", 4);
	AppendLe(&riff, 12, 4);
	AppendLe(&riff, fact_samples, 4);
	AppendLe(&riff, 0, 4);
	AppendLe(&riff, 0, 4);
	AppendBytes(&riff, "data", 4);
	AppendLe(&riff, 0, 4);
	const auto riff_size = static_cast<uint64_t>(riff.size() - 8);
	for (size_t i = 0; i < 4; i++)
	{
		riff[4 + i] = static_cast<uint8_t>(riff_size >> (8u * i));
	}
	return riff;
}

// Writes bits most significant first, as the ATRAC9 bitstream reads them.
class BitWriter
{
public:
	void Put(uint32_t value, uint32_t count)
	{
		for (uint32_t i = count; i-- > 0;)
		{
			m_bits.push_back(static_cast<uint8_t>((value >> i) & 1u));
		}
	}
	void Align()
	{
		while (m_bits.size() % 8 != 0)
		{
			m_bits.push_back(0);
		}
	}
	[[nodiscard]] std::vector<uint8_t> Bytes() const
	{
		std::vector<uint8_t> bytes(m_bits.size() / 8, 0);
		for (size_t i = 0; i < bytes.size() * 8; i++)
		{
			bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | (m_bits[i] << (7u - i % 8u)));
		}
		return bytes;
	}

private:
	std::vector<uint8_t> m_bits;
};

// One silent mono frame, built from the decoder's public bitstream rules: new
// block parameters with three bands, no band extension, a gradient whose curve
// starts past the ten coded units (gradient 0 everywhere), fixed-length
// scalefactors of 9 (coarse precision 9, so coefficients are 10-bit fixed
// fields), and 24 zero coefficients.
void PutSilentAtrac9Frame(BitWriter* writer)
{
	writer->Put(0, 1);  // first block in the packet
	writer->Put(0, 1);  // parameters not reused
	writer->Put(0, 4);  // band count 3
	writer->Put(0, 1);  // no band extension
	writer->Put(0, 2);  // gradient mode 0
	writer->Put(30, 6); // gradient range start
	writer->Put(30, 6); // gradient range end - 1
	writer->Put(0, 5);  // gradient value start
	writer->Put(0, 5);  // gradient value end
	writer->Put(0, 4);  // gradient boundary
	writer->Put(0, 1);  // no band extension data
	writer->Put(1, 2);  // scalefactors: fixed length
	writer->Put(2, 2);  // length 4
	writer->Put(9, 5);  // base 9
	for (int unit = 0; unit < 10; unit++)
	{
		writer->Put(0, 4);
	}
	for (int coefficient = 0; coefficient < 24; coefficient++)
	{
		writer->Put(0, 10);
	}
	writer->Align();
}

std::vector<uint8_t> BuildSilentAtrac9Superframe()
{
	BitWriter writer;
	for (int frame = 0; frame < 4; frame++)
	{
		PutSilentAtrac9Frame(&writer);
	}
	auto bytes = writer.Bytes();
	bytes.resize(kSyntheticAtrac9Superframe, 0);
	return bytes;
}

int32_t SetSamplerAtrac9Format(uintptr_t voice, uint32_t channels, uint32_t sample_rate, uint32_t config_data)
{
	SamplerSetupParam param {};
	param.header        = MakeSamplerHeader(sizeof(param), kSamplerSetupId);
	param.waveform_type = kWaveformAtrac9;
	param.num_channels  = channels;
	param.sample_rate   = sample_rate;
	param.config_data   = config_data;
	return VoiceControlWith(voice, param);
}

// Targets of callbacks that destroy their own objects from the dispatch step.
struct SamplerReentryTarget
{
	uintptr_t system = 0;
	uintptr_t rack   = 0;
};

SamplerReentryTarget g_sampler_reentry {};

void KYTY_SYSV_ABI SamplerDestroySystemCallback(const SamplerCallbackInfo* info)
{
	SamplerRecordCallback(info);
	(void)Ngs2::Ngs2SystemDestroy(g_sampler_reentry.system);
}

void KYTY_SYSV_ABI SamplerDestroyRackCallback(const SamplerCallbackInfo* info)
{
	SamplerRecordCallback(info);
	(void)Ngs2::Ngs2RackDestroy(g_sampler_reentry.rack, nullptr);
}

// Queues two 2-frame blocks (user data 1 and 2) in one reset add, so a single
// grain ends both and queues two callbacks for handler, then renders that grain
// with the guest route installed. Returns the render result.
int32_t RenderTwoBlockEnds(SamplerRig* rig, uintptr_t handler, GuestReadableBlock* pcm_storage, GuestReadableBlock* output_storage)
{
	auto* pcm = static_cast<int16_t*>(pcm_storage->Data());
	pcm[0]    = 1000;
	pcm[1]    = 2000;
	pcm[2]    = 3000;
	pcm[3]    = 4000;
	SamplerBlock blocks[2] {};
	blocks[0].data_offset = 0;
	blocks[0].data_size   = sizeof(int16_t) * 2;
	blocks[0].num_samples = 2;
	blocks[0].user_data   = 1;
	blocks[1].data_offset = sizeof(int16_t) * 2;
	blocks[1].data_size   = sizeof(int16_t) * 2;
	blocks[1].num_samples = 2;
	blocks[1].user_data   = 2;
	if (SetSamplerFormat(rig->voice, kWaveformPcmI16, 1, 48000) != 0 ||
	    AddSamplerBlocks(rig->voice, pcm_storage->Data(), kBlocksFlagReset, blocks, 2) != 0 ||
	    SetSamplerCallbackHandler(rig->voice, handler, kCallbackFlagEnd) != 0 || RunSamplerEvent(rig->voice, kSamplerPlayEvent) != 0)
	{
		return kSamplerInvalidControl;
	}
	g_sampler_reentry        = {rig->system, rig->rack};
	g_sampler_callback_count = 0;
	::Kyty::Emulator::GuestRuntimePort::Provider provider {};
	provider.invoke = SamplerInvokeHandler;
	::Kyty::Emulator::GuestRuntimePort::Install(provider);
	const int32_t result = RenderSamplerGrain(rig->system, output_storage->Data());
	::Kyty::Emulator::GuestRuntimePort::Install({});
	g_sampler_reentry = {};
	return result;
}

} // namespace

TEST(EmulatorAudio, StandardSamplerDeliversEveryBlockEndInQueueOrder)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 4);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(pcm_storage.IsValid() && output_storage.IsValid());
	ASSERT_EQ(RenderTwoBlockEnds(&rig, reinterpret_cast<uintptr_t>(&SamplerRecordCallback), &pcm_storage, &output_storage), 0);

	// Without any destroy both ends are delivered, in order, with their own user data.
	ASSERT_EQ(g_sampler_callback_count, 2u);
	EXPECT_EQ(g_sampler_callbacks[0].user_data, 1u);
	EXPECT_EQ(g_sampler_callbacks[1].user_data, 2u);
	const auto* output = static_cast<const float*>(output_storage.Data());
	EXPECT_NEAR(output[2 * 2], 3000.0f / 32768.0f, 1e-5f);
}

TEST(EmulatorAudio, StandardSamplerSkipsQueuedCallbacksAfterACallbackDestroysTheSystem)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 4);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(pcm_storage.IsValid() && output_storage.IsValid());
	const int32_t result =
	    RenderTwoBlockEnds(&rig, reinterpret_cast<uintptr_t>(&SamplerDestroySystemCallback), &pcm_storage, &output_storage);

	// The output reached the guest before dispatch. The first callback destroyed
	// the system, so the second queued callback did not run.
	EXPECT_EQ(result, 0);
	ASSERT_EQ(g_sampler_callback_count, 1u);
	EXPECT_EQ(g_sampler_callbacks[0].user_data, 1u);
	const auto* output = static_cast<const float*>(output_storage.Data());
	EXPECT_NEAR(output[0], 1000.0f / 32768.0f, 1e-5f);
	EXPECT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), static_cast<int32_t>(0x804a0201u));
	rig.system = 0;
	rig.rack   = 0;
}

TEST(EmulatorAudio, StandardSamplerSkipsQueuedCallbacksAfterACallbackDestroysTheRack)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 4);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(pcm_storage.IsValid() && output_storage.IsValid());
	const int32_t result =
	    RenderTwoBlockEnds(&rig, reinterpret_cast<uintptr_t>(&SamplerDestroyRackCallback), &pcm_storage, &output_storage);

	EXPECT_EQ(result, 0);
	ASSERT_EQ(g_sampler_callback_count, 1u);
	EXPECT_EQ(g_sampler_callbacks[0].user_data, 1u);
	GuestValue<uintptr_t> voice_storage;
	ASSERT_TRUE(voice_storage.IsValid());
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rig.rack, 0, voice_storage.Data()), static_cast<int32_t>(0x804a0261u));
	// The system outlives its destroyed rack and still renders.
	EXPECT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	rig.rack = 0;
}

TEST(EmulatorAudio, StandardSamplerBoundsPitchAndKeepsTheMixFinite)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmF32, 1, 48000), 0);

	// A finite pitch such as FLT_MAX would mean about 1e38 source frames per output
	// frame. The host limit is 16.
	EXPECT_EQ(SetSamplerPitch(rig.voice, std::numeric_limits<float>::max()), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerPitch(rig.voice, 16.5f), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerPitch(rig.voice, -1.0f), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerPitch(rig.voice, std::numeric_limits<float>::quiet_NaN()), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerPitch(rig.voice, 16.0f), 0);
	EXPECT_EQ(SetSamplerPitch(rig.voice, 1.0f), 0);

	// Non-finite float samples are refused when the block is added.
	GuestReadableBlock float_storage(sizeof(float) * 4);
	ASSERT_TRUE(float_storage.IsValid());
	auto* samples = static_cast<float*>(float_storage.Data());
	SamplerBlock block {};
	block.data_size   = sizeof(float) * 4;
	block.num_samples = 4;
	samples[0]        = 0.5f;
	samples[1]        = std::numeric_limits<float>::quiet_NaN();
	samples[2]        = 0.0f;
	samples[3]        = 0.0f;
	EXPECT_EQ(AddSamplerBlock(rig.voice, float_storage.Data(), kBlocksFlagReset, block), kSamplerInvalidControl);
	samples[1] = std::numeric_limits<float>::infinity();
	EXPECT_EQ(AddSamplerBlock(rig.voice, float_storage.Data(), kBlocksFlagReset, block), kSamplerInvalidControl);

	// A finite gain far above full scale still yields a finite, clamped mix.
	samples[0] = 0.5f;
	samples[1] = 0.5f;
	samples[2] = -0.5f;
	samples[3] = -0.5f;
	ASSERT_EQ(AddSamplerBlock(rig.voice, float_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerGain(rig.voice, std::numeric_limits<float>::max()), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());
	for (uint32_t i = 0; i < kSamplerGrainFrames * 2; i++)
	{
		ASSERT_TRUE(std::isfinite(output[i]));
	}
	EXPECT_EQ(output[0], 1.0f);
	EXPECT_EQ(output[2 * 2], -1.0f);
}

TEST(EmulatorAudio, StandardSamplerRefusesWaveformsAboveTheSampleBudgetAtomically)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 2, 48000), 0);
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 8);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	for (int i = 0; i < 8; i++)
	{
		pcm[i] = static_cast<int16_t>(1000 * (i + 1));
	}
	SamplerBlock small {};
	small.data_size   = sizeof(int16_t) * 8;
	small.num_samples = 4;
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, small), 0);

	// Three blocks of 1.5M stereo frames take 36 MiB of float frames, above the
	// 32 MiB voice budget. The headers alone decide it; the add changes nothing.
	SamplerBlock large[3] {};
	for (auto& block: large)
	{
		block.num_samples = 1572864;
		block.data_size   = static_cast<uint64_t>(block.num_samples) * 2 * sizeof(int16_t);
	}
	EXPECT_EQ(AddSamplerBlocks(rig.voice, pcm_storage.Data(), kBlocksFlagReset, large, 3), kSamplerInvalidControl);

	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());
	EXPECT_NEAR(output[0], 1000.0f / 32768.0f, 1e-5f);
	EXPECT_NEAR(output[1], 2000.0f / 32768.0f, 1e-5f);
}

TEST(EmulatorAudio, Ngs2SamplerPlaybackResamplesToTheGivenOutputRate)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 48000));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {0.0f, 0.25f, 0.5f, 0.75f};
	blocks[0].num_samples = 4;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// A 48 kHz source mixed at 24 kHz advances two source frames per output frame.
	std::vector<double>             stereo(16, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(8, 24000, stereo.data(), &events));
	EXPECT_DOUBLE_EQ(stereo[0], 0.0);
	EXPECT_DOUBLE_EQ(stereo[2], 0.5);
	EXPECT_DOUBLE_EQ(stereo[3], 0.5);
	EXPECT_EQ(stereo[4], 0.0);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].user_data, 0u);
	EXPECT_TRUE(playback.Ended());
	EXPECT_EQ(playback.DecodedFrames(), 4u);
}

TEST(EmulatorAudio, Ngs2SamplerPlaybackRefusesUnboundedSteps)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 192000));
	EXPECT_FALSE(playback.SetPitch(std::numeric_limits<float>::max()));
	EXPECT_FALSE(playback.SetPitch(std::numeric_limits<float>::infinity()));
	EXPECT_FALSE(playback.SetPitch(std::numeric_limits<float>::quiet_NaN()));
	ASSERT_TRUE(playback.SetPitch(16.0f));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {0.25f, 0.25f, 0.25f, 0.25f};
	blocks[0].num_samples = 4;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// 192 kHz at pitch 16 is 64 source frames per output frame at 48 kHz, the
	// limit. At 24 kHz it would be 128, so nothing is rendered and no state moves.
	std::vector<double>             stereo(8, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	EXPECT_FALSE(playback.Render(4, 24000, stereo.data(), &events));
	EXPECT_TRUE(events.empty());
	EXPECT_EQ(playback.DecodedFrames(), 0u);
	EXPECT_EQ(stereo[0], 0.0);

	ASSERT_TRUE(playback.Render(4, 48000, stereo.data(), &events));
	EXPECT_DOUBLE_EQ(stereo[0], 0.25);
	EXPECT_EQ(stereo[2], 0.0);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_TRUE(playback.Ended());
}

TEST(EmulatorAudio, Ngs2Atrac9ParseReportsSuperframeInfoFromRiffHeader)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const auto riff = BuildSyntheticAtrac9Riff(1, kSyntheticAtrac9Rate, 2048);
	GuestReadableBlock data_storage(riff.size());
	ASSERT_TRUE(data_storage.IsValid());
	std::memcpy(data_storage.Data(), riff.data(), riff.size());
	GuestReadableBlock info_storage(232);
	ASSERT_TRUE(info_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2ParseWaveformData(data_storage.Data(), riff.size(), reinterpret_cast<Ngs2::Ngs2WaveformInfo*>(info_storage.Data())), 0);

	// Ngs2WaveformInfo offsets: format at 0, num_samples 0x28, audio unit 0x2c,
	// audio frame 0x38 and 0x3c, num_blocks 0x44.
	const auto* words = static_cast<const uint32_t*>(info_storage.Data());
	EXPECT_EQ(words[0], kWaveformAtrac9);
	EXPECT_EQ(words[1], 1u);
	EXPECT_EQ(words[2], kSyntheticAtrac9Rate);
	EXPECT_EQ(words[3], kSyntheticAtrac9ConfigData);
	EXPECT_EQ(words[0x28 / 4], 2048u);
	EXPECT_EQ(words[0x2c / 4], 256u);
	EXPECT_EQ(words[0x38 / 4], kSyntheticAtrac9Superframe);
	EXPECT_EQ(words[0x3c / 4], kSyntheticAtrac9Superframe);
	EXPECT_EQ(words[0x44 / 4], 1u);
}

TEST(EmulatorAudio, ElementaryAudioDecoderOpensOnlyWithAMatchingTwelveByteConfig)
{
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		return;
	}
	using AudioVideoBackend::AudioCodec;
	using AudioVideoBackend::ElementaryAudioDecoder;

	const uint8_t extradata[12] = {0, 0, 0, 0, 0xfe, 0x70, 0x1f, 0xf0, 0, 0, 0, 0};
	std::string   error;
	auto          decoder = ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 1024, 1, kSyntheticAtrac9Rate, &error);
	EXPECT_TRUE(decoder != nullptr) << error;
	if (decoder != nullptr)
	{
		std::vector<float> pcm;
		EXPECT_FALSE(decoder->Decode(extradata, 11, &pcm));
	}

	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, 11, 1024, 1, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, 13, 1024, 1, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 0, 1, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 8193, 1, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 1024, 2, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 1024, 9, kSyntheticAtrac9Rate, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 1024, 1, 44100, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), 1024, 1, 192001, &error));

	uint8_t bad_version[12] = {};
	std::memcpy(bad_version, extradata, sizeof(extradata));
	bad_version[0] = 3;
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, bad_version, sizeof(bad_version), 1024, 1, kSyntheticAtrac9Rate, &error));

	uint8_t bad_sync[12] = {};
	std::memcpy(bad_sync, extradata, sizeof(extradata));
	bad_sync[4] = 0x00;
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, bad_sync, sizeof(bad_sync), 1024, 1, kSyntheticAtrac9Rate, &error));
}

TEST(EmulatorAudio, ElementaryAudioDecoderDecodesAGeneratedSilentSuperframe)
{
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		return;
	}
	using AudioVideoBackend::AudioCodec;
	using AudioVideoBackend::ElementaryAudioDecoder;

	const uint8_t extradata[12] = {0, 0, 0, 0, 0xfe, 0x70, 0x1f, 0xf0, 0, 0, 0, 0};
	std::string   error;
	auto decoder = ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata, sizeof(extradata), kSyntheticAtrac9Superframe, 1,
	                                            kSyntheticAtrac9Rate, &error);
	ASSERT_TRUE(decoder != nullptr) << error;

	const auto         superframe = BuildSilentAtrac9Superframe();
	std::vector<float> pcm;
	ASSERT_TRUE(decoder->Decode(superframe.data(), superframe.size(), &pcm)) << decoder->LastError();
	ASSERT_EQ(pcm.size(), static_cast<size_t>(kSyntheticAtrac9Superframe));
	for (const float sample: pcm)
	{
		ASSERT_EQ(sample, 0.0f);
	}
	// After a reset the same superframe starts a new stream and decodes the same way.
	decoder->Reset();
	ASSERT_TRUE(decoder->Decode(superframe.data(), superframe.size(), &pcm)) << decoder->LastError();
	EXPECT_EQ(pcm.size(), static_cast<size_t>(kSyntheticAtrac9Superframe));

	// Bytes that are not a superframe fail instead of decoding to silence.
	const std::vector<uint8_t> zeros(kSyntheticAtrac9Superframe, 0);
	EXPECT_FALSE(decoder->Decode(zeros.data(), zeros.size(), &pcm));
}

TEST(EmulatorAudio, StandardSamplerAtrac9SetupRequiresConfigThatMatchesTheFormat)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		return;
	}

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	EXPECT_EQ(SetSamplerAtrac9Format(rig.voice, 2, kSyntheticAtrac9Rate, kSyntheticAtrac9ConfigData), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerAtrac9Format(rig.voice, 1, 44100, kSyntheticAtrac9ConfigData), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerAtrac9Format(rig.voice, 1, kSyntheticAtrac9Rate, 0u), kSamplerInvalidControl);
	EXPECT_EQ(SetSamplerAtrac9Format(rig.voice, 1, kSyntheticAtrac9Rate, kSyntheticAtrac9ConfigData), 0);
}

TEST(EmulatorAudio, StandardSamplerPlaysGeneratedAtrac9Superframes)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		return;
	}

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	ASSERT_EQ(SetSamplerAtrac9Format(rig.voice, 1, kSyntheticAtrac9Rate, kSyntheticAtrac9ConfigData), 0);

	const auto         superframe = BuildSilentAtrac9Superframe();
	GuestReadableBlock data_storage(superframe.size() * 3);
	ASSERT_TRUE(data_storage.IsValid());
	for (size_t i = 0; i < 3; i++)
	{
		std::memcpy(static_cast<uint8_t*>(data_storage.Data()) + i * superframe.size(), superframe.data(), superframe.size());
	}

	// Skip 100 frames and play 1500: the block needs two superframes. The frames
	// are silent, so this exercises the two-superframe decode path and frame count;
	// it does not prove the skip lands at the right sample (content is all zero).
	SamplerBlock block {};
	block.data_size        = superframe.size() * 3;
	block.num_skip_samples = 100;
	block.num_samples      = 1500;
	ASSERT_EQ(AddSamplerBlock(rig.voice, data_storage.Data(), kBlocksFlagReset, block), 0);

	// A block needing more superframes than its data holds is refused, and the
	// queued waveform stays.
	SamplerBlock too_long = block;
	too_long.num_samples  = 4000;
	EXPECT_EQ(AddSamplerBlock(rig.voice, data_storage.Data(), kBlocksFlagReset, too_long), kSamplerInvalidControl);

	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());
	for (uint32_t i = 0; i < kSamplerGrainFrames * 2; i++)
	{
		ASSERT_EQ(output[i], 0.0f);
	}

	// The voice state reports the decoded frames at +0x10.
	GuestReadableBlock state_storage(48);
	ASSERT_TRUE(state_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2VoiceGetState(rig.voice, static_cast<Ngs2::Ngs2VoiceState*>(state_storage.Data()), 48), 0);
	uint64_t decoded = 0;
	std::memcpy(&decoded, static_cast<const uint8_t*>(state_storage.Data()) + 0x10, sizeof(decoded));
	EXPECT_EQ(decoded, static_cast<uint64_t>(kSamplerGrainFrames));
}

TEST(EmulatorAudio, StandardSamplerPlaysAtrac9FixtureWhenProvided)
{
	const char* fixture_path = std::getenv("KYTY_NGS2_ATRAC9_TEST_WAVEFORM");
	if (fixture_path == nullptr || fixture_path[0] == '\0' || !AudioVideoBackend::Decoder::IsAvailable())
	{
		return;
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	std::ifstream file(fixture_path, std::ios::binary | std::ios::ate);
	ASSERT_TRUE(file.is_open());
	const auto        size = static_cast<size_t>(file.tellg());
	std::vector<char> host(size);
	file.seekg(0);
	ASSERT_TRUE(static_cast<bool>(file.read(host.data(), static_cast<std::streamsize>(size))));
	GuestReadableBlock data_storage(size);
	ASSERT_TRUE(data_storage.IsValid());
	std::memcpy(data_storage.Data(), host.data(), size);

	GuestReadableBlock info_storage(232);
	ASSERT_TRUE(info_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2ParseWaveformData(data_storage.Data(), size, reinterpret_cast<Ngs2::Ngs2WaveformInfo*>(info_storage.Data())), 0);
	const auto* words = static_cast<const uint32_t*>(info_storage.Data());
	ASSERT_EQ(words[0], kWaveformAtrac9);

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	ASSERT_EQ(SetSamplerAtrac9Format(rig.voice, words[1], words[2], words[3]), 0);
	SamplerBlock block {};
	block.data_offset = words[0x18 / 4];
	block.data_size   = words[0x1c / 4];
	block.num_samples = std::min<uint32_t>(words[0x28 / 4], 4096);
	ASSERT_EQ(AddSamplerBlock(rig.voice, data_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	float peak = 0.0f;
	for (int grain = 0; grain < 96; grain++)
	{
		ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
		const auto* output = static_cast<const float*>(output_storage.Data());
		for (uint32_t i = 0; i < kSamplerGrainFrames * 2; i++)
		{
			ASSERT_TRUE(std::isfinite(output[i]));
			peak = std::max(peak, std::abs(output[i]));
		}
	}
	EXPECT_GT(peak, 0.0f);
	EXPECT_LE(peak, 1.0f);
}

namespace {

// Two voices that would each saturate a per-voice clip but cancel when summed in
// double: +full*100 and -full*98 leave +2 before the single final clamp to 1.
void AccumulateConstantVoice(std::vector<double>* mix, float sample, float gain)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 48000));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {sample};
	blocks[0].num_samples = 1;
	blocks[0].num_repeats = Ngs2Sampler::kRepeatForever;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetGain(gain);
	playback.SetPlaying(true);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(static_cast<uint32_t>(mix->size() / 2), 48000, mix->data(), &events));
	EXPECT_TRUE(events.empty());
}

} // namespace

TEST(EmulatorAudio, Ngs2SamplerSumsVoicesInDoubleAndCancelsBeforeTheSingleClamp)
{
	// Forward order: +1.0*100 then -1.0*98.
	std::vector<double> forward(8, 0.0);
	AccumulateConstantVoice(&forward, 1.0f, 100.0f);
	AccumulateConstantVoice(&forward, -1.0f, 98.0f);
	// Reverse order: the double sum is order independent.
	std::vector<double> reverse(8, 0.0);
	AccumulateConstantVoice(&reverse, -1.0f, 98.0f);
	AccumulateConstantVoice(&reverse, 1.0f, 100.0f);

	for (size_t i = 0; i < forward.size(); i++)
	{
		// A per-voice +/-64 clip would have made each term cancel to 0; the double
		// sum keeps +2, which the system clamp then limits to +1.
		EXPECT_DOUBLE_EQ(forward[i], 2.0);
		EXPECT_DOUBLE_EQ(reverse[i], 2.0);
		EXPECT_DOUBLE_EQ(std::clamp(forward[i], -1.0, 1.0), 1.0);
	}
}

TEST(EmulatorAudio, Ngs2SamplerShortLoopingBlockStaysBoundedWithoutEvents)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 192000));
	ASSERT_TRUE(playback.SetPitch(16.0f));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {0.25f};
	blocks[0].num_samples = 1;
	blocks[0].num_repeats = Ngs2Sampler::kRepeatForever;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// The worst case for allocation: one-frame forever loop, 192 kHz, pitch 16,
	// a large grain. It advances 64 source frames per output frame but a repeat
	// emits no event, so the event list stays empty and nothing is allocated.
	std::vector<double>             stereo(8192 * 2, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(8192, 48000, stereo.data(), &events));
	EXPECT_TRUE(events.empty());
	EXPECT_EQ(playback.DecodedFrames(), static_cast<uint64_t>(8192) * 64u);
	EXPECT_FALSE(playback.Ended());
	EXPECT_DOUBLE_EQ(stereo[0], 0.25);
}

TEST(EmulatorAudio, Ngs2SamplerFiniteRepeatReportsOneEndWithTheRepeatCount)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 48000));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {0.5f};
	blocks[0].num_samples = 1;
	blocks[0].num_repeats = 5;
	blocks[0].user_data   = 0x77;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// One sample played six times (first pass plus five repeats) ends once.
	std::vector<double>             stereo(16, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(8, 48000, stereo.data(), &events));
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].num_repeated, 5u);
	EXPECT_EQ(events[0].user_data, 0x77u);
	EXPECT_EQ(playback.DecodedFrames(), 6u);
	EXPECT_TRUE(playback.Ended());
}

TEST(EmulatorAudio, Ngs2SamplerPhaseContinuesAcrossRenderCalls)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 48000));
	ASSERT_TRUE(playback.SetPitch(0.5f));
	std::vector<Ngs2Sampler::Block> blocks(1);
	blocks[0].frames      = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
	blocks[0].num_samples = 8;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// A ramp at half speed reads positions 0, 0.5, 1.0, ... The second grain must
	// continue at 2.0, not restart, so the two grains form one ramp.
	std::vector<double>             first(8, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(4, 48000, first.data(), &events));
	std::vector<double> second(8, 0.0);
	ASSERT_TRUE(playback.Render(4, 48000, second.data(), &events));
	const double expected[] = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5};
	for (uint32_t frame = 0; frame < 4; frame++)
	{
		EXPECT_NEAR(first[2 * frame], expected[frame], 1e-9);
		EXPECT_NEAR(second[2 * frame], expected[frame + 4], 1e-9);
	}
}

TEST(EmulatorAudio, Ngs2SamplerInterpolatesAcrossANonRepeatingBlockBoundary)
{
	Ngs2Sampler::Playback playback;
	ASSERT_TRUE(playback.Configure(1, 48000));
	ASSERT_TRUE(playback.SetPitch(0.5f));
	std::vector<Ngs2Sampler::Block> blocks(2);
	blocks[0].frames      = {0.0f, 1.0f};
	blocks[0].num_samples = 2;
	blocks[1].frames      = {3.0f, 3.0f};
	blocks[1].num_samples = 2;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
	playback.SetPlaying(true);

	// At the last sample of block 0 the next sample is the first of block 1, so
	// the seam interpolates 1.0 -> 3.0 rather than clamping to the block edge.
	std::vector<double>             stereo(10, 0.0);
	std::vector<Ngs2Sampler::Event> events;
	ASSERT_TRUE(playback.Render(5, 48000, stereo.data(), &events));
	const double expected[] = {0.0, 0.5, 1.0, 2.0, 3.0};
	for (uint32_t frame = 0; frame < 5; frame++)
	{
		EXPECT_NEAR(stereo[2 * frame], expected[frame], 1e-9);
	}
	ASSERT_EQ(events.size(), 1u);
}

TEST(EmulatorAudio, Ngs2SamplerSystemMixCancelsTwoOppositeVoicesBeforeClamp)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRigVoices(&rig, 2));
	GuestValue<uintptr_t> voice1_storage;
	ASSERT_TRUE(voice1_storage.IsValid());
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rig.rack, 1, voice1_storage.Data()), 0);
	const uintptr_t voice1 = *voice1_storage.Data();

	GuestReadableBlock positive_storage(sizeof(float) * 2);
	GuestReadableBlock negative_storage(sizeof(float) * 2);
	ASSERT_TRUE(positive_storage.IsValid() && negative_storage.IsValid());
	auto* positive = static_cast<float*>(positive_storage.Data());
	auto* negative = static_cast<float*>(negative_storage.Data());
	positive[0] = positive[1] = 1.0f;
	negative[0] = negative[1] = -1.0f;
	SamplerBlock block {};
	block.data_size   = sizeof(float) * 2;
	block.num_samples = 2;

	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmF32, 1, 48000), 0);
	ASSERT_EQ(AddSamplerBlock(rig.voice, positive_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerGain(rig.voice, 100.0f), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);
	ASSERT_EQ(SetSamplerFormat(voice1, kWaveformPcmF32, 1, 48000), 0);
	ASSERT_EQ(AddSamplerBlock(voice1, negative_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerGain(voice1, 98.0f), 0);
	ASSERT_EQ(RunSamplerEvent(voice1, kSamplerPlayEvent), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	const auto* output = static_cast<const float*>(output_storage.Data());
	// +100 and -98 sum to +2 in the double accumulator, clamped once to +1. A
	// per-voice clip would have cancelled to 0.
	EXPECT_FLOAT_EQ(output[0], 1.0f);
	EXPECT_FLOAT_EQ(output[1], 1.0f);
}

TEST(EmulatorAudio, StandardSamplerDefersCallbacksUntilSystemUnlock)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 2);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	pcm[0]    = 1000;
	pcm[1]    = 2000;
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * 2;
	block.num_samples = 2;
	block.user_data   = 0x11;
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000), 0);
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(SetSamplerCallbackHandler(rig.voice, reinterpret_cast<uintptr_t>(&SamplerRecordCallback), kCallbackFlagEnd), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());

	ScopedHostRuntime runtime;
	g_sampler_callback_count = 0;
	// While the system is explicitly locked, the render must not run the callback
	// (it would hold the state lock across a guest call). It runs at unlock.
	ASSERT_EQ(Ngs2::Ngs2SystemLock(rig.system), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_EQ(g_sampler_callback_count, 0u);
	ASSERT_EQ(Ngs2::Ngs2SystemUnlock(rig.system), 0);
	ASSERT_EQ(g_sampler_callback_count, 1u);
	EXPECT_EQ(g_sampler_callbacks[0].user_data, 0x11u);
}

namespace {

using SteadyClock = std::chrono::steady_clock;
constexpr auto kBarrierTimeout = std::chrono::seconds(10);

// Shared with the worker threads. Only globals and values are shared, so a
// worker detached after a timeout never touches a destroyed test frame.
std::atomic<bool>    g_barrier_entered {false};
std::atomic<bool>    g_barrier_release {false};
std::atomic<bool>    g_barrier_render_done {false};
std::atomic<int32_t> g_barrier_render_result {-1};
std::atomic<bool>    g_barrier_destroy_done {false};
std::atomic<int32_t> g_barrier_destroy_result {-1};

// Waits until flag is set or deadline passes, yielding between checks.
bool WaitForFlag(const std::atomic<bool>& flag, SteadyClock::time_point deadline)
{
	while (!flag.load())
	{
		if (SteadyClock::now() >= deadline)
		{
			return false;
		}
		std::this_thread::yield();
	}
	return true;
}

// Holds the dispatching render thread inside the guest callback until the
// controller releases it. The wait is bounded so a failure cannot hang the test.
void KYTY_SYSV_ABI SamplerBarrierCallback(const SamplerCallbackInfo* info)
{
	SamplerRecordCallback(info);
	g_barrier_entered.store(true);
	(void)WaitForFlag(g_barrier_release, SteadyClock::now() + kBarrierTimeout);
}

std::atomic<uint32_t> g_deferred_end_count {0};

void KYTY_SYSV_ABI SamplerCountCallback(const SamplerCallbackInfo* info)
{
	(void)info;
	g_deferred_end_count.fetch_add(1);
}

} // namespace

TEST(EmulatorAudio, StandardSamplerRackDestroyWaitsForACallbackRunningOnAnotherThread)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	auto rig = std::make_unique<SamplerRig>();
	ASSERT_TRUE(CreateSamplerRig(rig.get()));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * 2);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	pcm[0]    = 1000;
	pcm[1]    = 2000;
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * 2;
	block.num_samples = 2;
	ASSERT_EQ(SetSamplerFormat(rig->voice, kWaveformPcmI16, 1, 48000), 0);
	ASSERT_EQ(AddSamplerBlock(rig->voice, pcm_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(SetSamplerCallbackHandler(rig->voice, reinterpret_cast<uintptr_t>(&SamplerBarrierCallback), kCallbackFlagEnd), 0);
	ASSERT_EQ(RunSamplerEvent(rig->voice, kSamplerPlayEvent), 0);

	ScopedHostRuntime runtime;
	g_sampler_callback_count = 0;
	g_barrier_entered.store(false);
	g_barrier_release.store(false);
	g_barrier_render_done.store(false);
	g_barrier_render_result.store(-1);
	g_barrier_destroy_done.store(false);
	g_barrier_destroy_result.store(-1);

	// The render thread enters the end callback and is held there.
	const uintptr_t system = rig->system;
	const uintptr_t rack   = rig->rack;
	std::thread     renderer(
        [system]()
        {
            GuestReadableBlock output(sizeof(float) * kSamplerGrainFrames * 2);
            g_barrier_render_result.store(output.IsValid() ? RenderSamplerGrain(system, output.Data()) : -1);
            g_barrier_render_done.store(true);
        });
	const auto  deadline = SteadyClock::now() + kBarrierTimeout;
	const bool  entered  = WaitForFlag(g_barrier_entered, deadline);
	std::thread destroyer;
	bool        closing_seen            = false;
	bool        destroy_done_while_held = true;
	if (entered)
	{
		// The destroy starts while the callback is still running.
		destroyer = std::thread(
		    [rack]()
		    {
			    g_barrier_destroy_result.store(Ngs2::Ngs2RackDestroy(rack, nullptr));
			    g_barrier_destroy_done.store(true);
		    });
		// The destroy has begun once the guest sees the rack as invalid.
		GuestValue<uintptr_t> voice_storage;
		while (voice_storage.IsValid() && SteadyClock::now() < deadline)
		{
			if (Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice_storage.Data()) == static_cast<int32_t>(0x804a0261u))
			{
				closing_seen = true;
				break;
			}
			std::this_thread::yield();
		}
		// With the callback still held, the destroy must not have completed.
		destroy_done_while_held = g_barrier_destroy_done.load();
	}

	// Release in every path, then wait for both threads within a bound.
	g_barrier_release.store(true);
	const bool render_finished  = WaitForFlag(g_barrier_render_done, SteadyClock::now() + kBarrierTimeout);
	const bool destroy_finished = !destroyer.joinable() || WaitForFlag(g_barrier_destroy_done, SteadyClock::now() + kBarrierTimeout);
	if (render_finished && destroy_finished)
	{
		renderer.join();
		if (destroyer.joinable())
		{
			destroyer.join();
		}
	} else
	{
		// A stuck worker may still use the rig: leak it rather than free it under them.
		renderer.detach();
		if (destroyer.joinable())
		{
			destroyer.detach();
		}
		(void)rig.release();
	}
	ASSERT_TRUE(entered);
	ASSERT_TRUE(render_finished);
	ASSERT_TRUE(destroy_finished);
	EXPECT_TRUE(closing_seen);
	EXPECT_FALSE(destroy_done_while_held);
	EXPECT_EQ(g_barrier_render_result.load(), 0);
	EXPECT_EQ(g_barrier_destroy_result.load(), 0);
	EXPECT_EQ(g_sampler_callback_count, 1u);
	rig->rack = 0;
}

TEST(EmulatorAudio, Ngs2RackDestroyIsRefusedWhileThisThreadHoldsTheSystemLock)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestValue<uintptr_t> voice_storage;
	ASSERT_TRUE(voice_storage.IsValid());

	// Refused at once instead of waiting on pins only the lock holder can free;
	// nothing is marked closing, so the rack stays usable.
	ASSERT_EQ(Ngs2::Ngs2SystemLock(rig.system), 0);
	EXPECT_EQ(Ngs2::Ngs2RackDestroy(rig.rack, nullptr), static_cast<int32_t>(0x804a0261u));
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rig.rack, 0, voice_storage.Data()), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemUnlock(rig.system), 0);

	EXPECT_EQ(Ngs2::Ngs2RackDestroy(rig.rack, nullptr), 0);
	rig.rack = 0;
}

TEST(EmulatorAudio, StandardSamplerRefusesALockedRenderWhenDeferredCallbacksAreFull)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr uint32_t kBlocks   = 64;
	constexpr uint32_t kCapacity = 64u * 256u;
	SamplerRig         rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * kBlocks);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto*        pcm = static_cast<int16_t*>(pcm_storage.Data());
	SamplerBlock blocks[kBlocks] {};
	for (uint32_t i = 0; i < kBlocks; i++)
	{
		pcm[i]                = 1000;
		blocks[i].data_offset = sizeof(int16_t) * i;
		blocks[i].data_size   = sizeof(int16_t);
		blocks[i].num_samples = 1;
		blocks[i].user_data   = i;
	}
	ASSERT_EQ(SetSamplerCallbackHandler(rig.voice, reinterpret_cast<uintptr_t>(&SamplerCountCallback), kCallbackFlagEnd), 0);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	GuestReadableBlock state_storage(48);
	ASSERT_TRUE(output_storage.IsValid() && state_storage.IsValid());

	// Setup returns the voice to Empty, so each queued play starts a new pass.
	auto queue_waveform = [&]()
	{
		return SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000) == 0 &&
		       AddSamplerBlocks(rig.voice, pcm_storage.Data(), kBlocksFlagReset, blocks, kBlocks) == 0 &&
		       RunSamplerEvent(rig.voice, kSamplerPlayEvent) == 0;
	};

	ScopedHostRuntime runtime;
	g_deferred_end_count.store(0);
	ASSERT_EQ(Ngs2::Ngs2SystemLock(rig.system), 0);
	// Each locked render ends 64 blocks and defers their callbacks; 256 renders
	// fill the deferred list exactly.
	for (uint32_t i = 0; i < kCapacity / kBlocks; i++)
	{
		ASSERT_TRUE(queue_waveform());
		ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	}
	EXPECT_EQ(g_deferred_end_count.load(), 0u);

	// The next locked render could end 64 more blocks: it is refused before any
	// voice state changes, and no callback is dropped or delivered.
	ASSERT_TRUE(queue_waveform());
	EXPECT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), kSamplerInvalidControl);
	ASSERT_EQ(Ngs2::Ngs2VoiceGetState(rig.voice, static_cast<Ngs2::Ngs2VoiceState*>(state_storage.Data()), 48), 0);
	uint64_t decoded = 0;
	std::memcpy(&decoded, static_cast<const uint8_t*>(state_storage.Data()) + 0x10, sizeof(decoded));
	EXPECT_EQ(decoded, 0u);
	EXPECT_EQ(g_deferred_end_count.load(), 0u);

	// Unlock delivers every deferred callback.
	ASSERT_EQ(Ngs2::Ngs2SystemUnlock(rig.system), 0);
	EXPECT_EQ(g_deferred_end_count.load(), kCapacity);

	// The refused waveform is intact and plays once the lock is gone.
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_EQ(g_deferred_end_count.load(), kCapacity + kBlocks);
}

namespace {

// Creates one more sampler rack with voice_count voices on an existing system,
// through the same query/create calls the guest makes. workspace keeps the rack
// buffer alive and must outlive the rack.
bool CreateExtraSamplerRack(uintptr_t system, uint32_t voice_count, std::unique_ptr<GuestReadableBlock>* workspace, uintptr_t* rack)
{
	GuestReadableBlock    option_storage(kSamplerOptionSize);
	GuestReadableBlock    rack_info_storage(sizeof(uint64_t) * 8);
	GuestValue<uintptr_t> rack_handle_storage;
	if (!option_storage.IsValid() || !rack_info_storage.IsValid() || !rack_handle_storage.IsValid())
	{
		return false;
	}
	auto* raw_option = static_cast<uint8_t*>(option_storage.Data());
	std::memset(raw_option, 0, kSamplerOptionSize);
	*reinterpret_cast<size_t*>(raw_option)          = kSamplerOptionSize;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = voice_count;
	const auto* option                              = reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option);
	auto*       raw_rack_info                       = static_cast<uint64_t*>(rack_info_storage.Data());
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	if (Ngs2::Ngs2RackQueryBufferSize(kSamplerRackId, option, rack_info) != 0)
	{
		return false;
	}
	*workspace = std::make_unique<GuestReadableBlock>(raw_rack_info[1]);
	if (!(*workspace)->IsValid())
	{
		return false;
	}
	raw_rack_info[0] = reinterpret_cast<uintptr_t>((*workspace)->Data());
	if (Ngs2::Ngs2RackCreate(system, kSamplerRackId, option, rack_info, rack_handle_storage.Data()) != 0)
	{
		return false;
	}
	*rack = *rack_handle_storage.Data();
	return *rack != 0;
}

// Gives a voice a 64-block waveform of one-frame blocks with an end callback, and
// queues play. Every block ends in the first 64 frames of the next render.
bool QueueEndingWaveform(uintptr_t voice, const void* data, const SamplerBlock* blocks, uint32_t count)
{
	return SetSamplerFormat(voice, kWaveformPcmI16, 1, 48000) == 0 &&
	       AddSamplerBlocks(voice, data, kBlocksFlagReset, blocks, count) == 0 &&
	       SetSamplerCallbackHandler(voice, reinterpret_cast<uintptr_t>(&SamplerCountCallback), kCallbackFlagEnd) == 0 &&
	       RunSamplerEvent(voice, kSamplerPlayEvent) == 0;
}

} // namespace

TEST(EmulatorAudio, StandardSamplerRefusesAnUnlockedRenderAboveTheCallbackBatchLimit)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr uint32_t kBlocks     = 64;
	constexpr uint32_t kRackVoices = 256;
	constexpr uint32_t kLimit      = kBlocks * kRackVoices;

	// Declared before the rig so it is freed after the system destroy.
	std::unique_ptr<GuestReadableBlock> extra_workspace;
	SamplerRig                          rig;
	ASSERT_TRUE(CreateSamplerRigVoices(&rig, kRackVoices));
	uintptr_t extra_rack = 0;
	ASSERT_TRUE(CreateExtraSamplerRack(rig.system, 1, &extra_workspace, &extra_rack));

	GuestReadableBlock pcm_storage(sizeof(int16_t) * kBlocks);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto*        pcm = static_cast<int16_t*>(pcm_storage.Data());
	SamplerBlock blocks[kBlocks] {};
	for (uint32_t i = 0; i < kBlocks; i++)
	{
		pcm[i]                = 1000;
		blocks[i].data_offset = sizeof(int16_t) * i;
		blocks[i].data_size   = sizeof(int16_t);
		blocks[i].num_samples = 1;
	}

	// Rack 1: 256 voices x 64 ending blocks, exactly the limit (64 KiB of queued
	// frames, far inside the sample budget). Rack 2 adds one more ending block.
	GuestValue<uintptr_t> voice_storage;
	ASSERT_TRUE(voice_storage.IsValid());
	uintptr_t first_voice = 0;
	for (uint32_t id = 0; id < kRackVoices; id++)
	{
		ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rig.rack, id, voice_storage.Data()), 0);
		if (id == 0)
		{
			first_voice = *voice_storage.Data();
		}
		ASSERT_TRUE(QueueEndingWaveform(*voice_storage.Data(), pcm_storage.Data(), blocks, kBlocks));
	}
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(extra_rack, 0, voice_storage.Data()), 0);
	const uintptr_t extra_voice = *voice_storage.Data();
	ASSERT_TRUE(QueueEndingWaveform(extra_voice, pcm_storage.Data(), blocks, 1));

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	GuestReadableBlock state_storage(48);
	ASSERT_TRUE(output_storage.IsValid() && state_storage.IsValid());
	auto* output = static_cast<float*>(output_storage.Data());
	std::fill_n(output, kSamplerGrainFrames * 2, 0.25f);

	ScopedHostRuntime runtime;
	g_deferred_end_count.store(0);

	// No explicit lock is held, yet one render could end 16385 blocks. It is
	// refused before any voice advances, any callback runs or the output is written.
	EXPECT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), kSamplerInvalidControl);
	EXPECT_EQ(g_deferred_end_count.load(), 0u);
	EXPECT_EQ(output[0], 0.25f);
	EXPECT_EQ(output[kSamplerGrainFrames * 2 - 1], 0.25f);
	for (const uintptr_t voice: {first_voice, extra_voice})
	{
		ASSERT_EQ(Ngs2::Ngs2VoiceGetState(voice, static_cast<Ngs2::Ngs2VoiceState*>(state_storage.Data()), 48), 0);
		uint32_t flags   = 0;
		uint64_t decoded = 0;
		std::memcpy(&flags, state_storage.Data(), sizeof(flags));
		std::memcpy(&decoded, static_cast<const uint8_t*>(state_storage.Data()) + 0x10, sizeof(decoded));
		EXPECT_EQ(decoded, 0u);
		// The queued play was not applied: the voice is still Empty.
		EXPECT_EQ(flags, 0u);
	}

	// A setup on the second rack's voice empties its queue; the batch is then at
	// the limit, the same render succeeds and delivers every end callback.
	ASSERT_EQ(SetSamplerFormat(extra_voice, kWaveformPcmI16, 1, 48000), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_EQ(g_deferred_end_count.load(), kLimit);
	// 256 voices of 1000/32768 sum to about 7.8 and are clamped once to full scale.
	EXPECT_EQ(output[0], 1.0f);

	EXPECT_EQ(Ngs2::Ngs2RackDestroy(extra_rack, nullptr), 0);
}

UT_END();
