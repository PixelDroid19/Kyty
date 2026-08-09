#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Audio.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/AudioPcm.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include "SDL.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <thread>

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

} // namespace

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

TEST(EmulatorAudio, AudioOut2ContextOperationsRejectUnmeasuredLayoutsWithoutWrites)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock context_param_storage(64);
	GuestReadableBlock context_workspace(64);
	GuestValue<uint64_t> memory_size_storage;
	GuestValue<int32_t> context_storage;
	ASSERT_TRUE(context_param_storage.IsValid());
	ASSERT_TRUE(context_workspace.IsValid());
	ASSERT_TRUE(memory_size_storage.IsValid());
	ASSERT_TRUE(context_storage.IsValid());
	std::memset(context_param_storage.Data(), 0xa5, 64);
	std::memset(context_workspace.Data(), 0xa5, 64);
	*memory_size_storage.Data() = UINT64_MAX;
	*context_storage.Data()     = INT32_MAX;

	auto* context_param = context_param_storage.Data();
	EXPECT_EQ(AudioOut2::AudioOut2ContextResetParam(nullptr), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextResetParam(context_param), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(nullptr, memory_size_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(context_param, memory_size_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(nullptr, context_workspace.Data(), 64, context_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(context_param, context_workspace.Data(), 64, context_storage.Data()),
	          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*memory_size_storage.Data(), UINT64_MAX);
	EXPECT_EQ(*context_storage.Data(), INT32_MAX);
	for (size_t i = 0; i < 64; ++i)
	{
		EXPECT_EQ(static_cast<uint8_t*>(context_param_storage.Data())[i], 0xa5);
		EXPECT_EQ(static_cast<uint8_t*>(context_workspace.Data())[i], 0xa5);
	}
}

TEST(EmulatorAudio, AudioOut2HostStatePushPreservesPcmQueueAndSinkAcrossFailure)
{
	// Host-state regression only: ContextCreate itself is intentionally not a
	// supported guest contract until its parameter/workspace ABI is evidenced.
	using ExpectedPush = int(KYTY_SYSV_ABI*)(int32_t, uint32_t);
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

	AvPlayer::AvPlayerInitDataEx init {};
	init.this_size = sizeof(init);
	AvPlayer::AvPlayerInternal* handle = nullptr;
	ASSERT_EQ(AvPlayer::AvPlayerInitEx(&init, &handle), 0);
	ASSERT_NE(handle, nullptr);
	ASSERT_EQ(AvPlayer::AvPlayerAddSource(handle, media_path), 0);
	AvPlayer::AvPlayerStreamInfoEx stream {};
	ASSERT_EQ(AvPlayer::AvPlayerGetStreamInfoEx(handle, 0, &stream), 0);
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

static int g_avplayer_test_ready_events = 0;
static int g_avplayer_test_stop_events  = 0;

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

	g_avplayer_test_ready_events = 0;
	g_avplayer_test_stop_events  = 0;

	AvPlayer::AvPlayerInitDataEx init_ex {};
	init_ex.this_size                     = sizeof(init_ex);
	init_ex.event_replacement.event_callback = avplayer_test_event_cb;
	init_ex.num_output_video_framebuffers = 2;

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
	EXPECT_EQ(g_avplayer_test_ready_events, 1);
	EXPECT_EQ(AvPlayer::AvPlayerStreamCount(handle), 2);

	AvPlayer::AvPlayerStreamInfoEx video_stream {};
	ASSERT_EQ(AvPlayer::AvPlayerGetStreamInfoEx(handle, 0, &video_stream), 0);
	EXPECT_EQ(video_stream.type, AvPlayer::AvPlayerStreamVideo);
	EXPECT_EQ(video_stream.details.video.width, 1920u);
	EXPECT_EQ(video_stream.details.video.height, 1080u);

	ASSERT_EQ(AvPlayer::AvPlayerStart(handle), 0);
	EXPECT_EQ(AvPlayer::AvPlayerIsActive(handle), 1);

	AvPlayer::AvPlayerFrameInfoEx frame {};
	uint32_t frame_count = 0;
	while (AvPlayer::AvPlayerGetVideoDataEx(handle, &frame) != 0)
	{
		frame_count++;
	}

	EXPECT_EQ(frame_count, 90u);
	EXPECT_EQ(AvPlayer::AvPlayerIsActive(handle), 0);
	EXPECT_EQ(g_avplayer_test_stop_events, 1);

	EXPECT_EQ(AvPlayer::AvPlayerClose(handle), 0);
}

UT_END();
