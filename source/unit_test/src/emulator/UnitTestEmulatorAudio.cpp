#include "Kyty/UnitTest.h"

#include "Emulator/Audio.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/AudioPcm.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include <cstring>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <thread>

UT_BEGIN(EmulatorAudio);

using namespace Libs::Audio;

TEST(EmulatorAudio, AudioOut2UserCreateUsesTwoArgumentPointerSizedHandleAbi)
{
	using ExpectedCreate = int(KYTY_SYSV_ABI*)(uint32_t, uintptr_t*);
	using ExpectedDestroy = int(KYTY_SYSV_ABI*)(uintptr_t);

	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2UserCreate), ExpectedCreate>));
	EXPECT_TRUE((std::is_same_v<decltype(&AudioOut2::AudioOut2UserDestroy), ExpectedDestroy>));
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

	alignas(uint64_t) uint64_t raw_info[8];
	for (auto& value: raw_info)
	{
		value = UINT64_MAX;
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

TEST(EmulatorAudio, CreatesNgs2SystemInProvidedBuffer)
{
	alignas(uint64_t) uint64_t raw_info[8] = {};
	auto*                      info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, info), 0);

	std::unique_ptr<uint8_t[]> storage(new uint8_t[raw_info[1]]);
	raw_info[0]      = reinterpret_cast<uintptr_t>(storage.get());
	uintptr_t handle = 0;

	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, &handle), 0);
	EXPECT_EQ(handle, reinterpret_cast<uintptr_t>(storage.get()));
	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, nullptr, &handle), static_cast<int32_t>(0x804a0206u));
	EXPECT_EQ(Ngs2::Ngs2SystemCreate(nullptr, info, nullptr), static_cast<int32_t>(0x804a0053u));

	storage.release();
}

TEST(EmulatorAudio, RejectsNullNgs2RackHandle)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uintptr_t voice_handle = UINTPTR_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(0, 0, &voice_handle), static_cast<int32_t>(0x804a0261u));
	EXPECT_EQ(voice_handle, 0u);
}

TEST(EmulatorAudio, ReadsGen5CustomRackVoiceCountFromCommonOptionBlock)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	alignas(uint64_t) uint8_t raw_option_one[0x518]     = {};
	alignas(uint64_t) uint8_t raw_option_two[0x518]     = {};
	*reinterpret_cast<size_t*>(raw_option_one)          = sizeof(raw_option_one);
	*reinterpret_cast<size_t*>(raw_option_two)          = sizeof(raw_option_two);
	*reinterpret_cast<uint32_t*>(raw_option_one + 0x50) = 1;
	*reinterpret_cast<uint32_t*>(raw_option_two + 0x50) = 2;

	// This field is part of the custom extension, not the common max-voices field.
	*reinterpret_cast<uint32_t*>(raw_option_one + 0xb8) = 7;
	*reinterpret_cast<uint32_t*>(raw_option_two + 0xb8) = 7;

	alignas(uint64_t) uint64_t raw_info_one[8] = {};
	alignas(uint64_t) uint64_t raw_info_two[8] = {};
	auto*                      info_one        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info_one);
	auto*                      info_two        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info_two);

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

	alignas(uint64_t) uint64_t raw_sys_info[8] = {};
	auto*                      sys_info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	std::unique_ptr<uint8_t[]> sys_storage(new uint8_t[raw_sys_info[1]]);
	raw_sys_info[0]  = reinterpret_cast<uintptr_t>(sys_storage.get());
	uintptr_t system = 0;
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, &system), 0);

	// Classic max_voices at +0x20 left 0; extended field at +0x50 is the real count.
	alignas(uint64_t) uint8_t raw_option[0xb8]      = {};
	*reinterpret_cast<size_t*>(raw_option)          = sizeof(raw_option);
	*reinterpret_cast<uint32_t*>(raw_option + 0x20) = 0;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 16;

	alignas(uint64_t) uint64_t raw_rack_info[8] = {};
	auto*                      rack_info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x2001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	// Must allocate room for 16 voices, not zero.
	EXPECT_GT(raw_rack_info[1], sizeof(void*) * 8u);

	std::unique_ptr<uint8_t[]> rack_storage(new uint8_t[raw_rack_info[1]]);
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.get());
	uintptr_t rack   = 0;
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x2001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info, &rack), 0);

	uintptr_t voice0  = 0;
	uintptr_t voice15 = 0;
	uintptr_t voice16 = UINTPTR_MAX;
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, &voice0), 0);
	EXPECT_NE(voice0, 0u);
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 15, &voice15), 0);
	EXPECT_NE(voice15, 0u);
	EXPECT_NE(voice15, voice0);
	// Out of range is a guest error, not a process exit.
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 16, &voice16), static_cast<int32_t>(0x804a0300u));
	EXPECT_EQ(voice16, 0u);

	sys_storage.release();
	rack_storage.release();
}

TEST(EmulatorAudio, AcceptsCustomSamplerVoiceControlParamClass)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// System buffer.
	alignas(uint64_t) uint64_t raw_sys_info[8] = {};
	auto*                      sys_info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	std::unique_ptr<uint8_t[]> sys_storage(new uint8_t[raw_sys_info[1]]);
	raw_sys_info[0]  = reinterpret_cast<uintptr_t>(sys_storage.get());
	uintptr_t system = 0;
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, &system), 0);

	// Gen5 custom-sampler rack option (0x518) with max_voices at offset 0x50.
	alignas(uint64_t) uint8_t raw_option[0x518]     = {};
	*reinterpret_cast<size_t*>(raw_option)          = sizeof(raw_option);
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;

	alignas(uint64_t) uint64_t raw_rack_info[8] = {};
	auto*                      rack_info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	std::unique_ptr<uint8_t[]> rack_storage(new uint8_t[raw_rack_info[1]]);
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.get());
	uintptr_t rack   = 0;
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info, &rack), 0);

	uintptr_t voice = 0;
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, &voice), 0);
	ASSERT_NE(voice, 0u);

	// Observed frontier param: id 0x40010000 (rack class 0x4001), size 40, next 0.
	// Layout matches Ngs2VoiceParamHeader: size, next, id — then opaque payload.
	alignas(uint64_t) uint8_t param_blob[40]      = {};
	*reinterpret_cast<uint16_t*>(param_blob + 0)  = 40;
	*reinterpret_cast<int16_t*>(param_blob + 2)   = 0;
	*reinterpret_cast<uint32_t*>(param_blob + 4)  = 0x40010000u;
	*reinterpret_cast<uint32_t*>(param_blob + 8)  = 0x12u;
	*reinterpret_cast<uint32_t*>(param_blob + 12) = 2;
	*reinterpret_cast<uint32_t*>(param_blob + 16) = 44100;

	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param_blob)), 0);

	// Common param id 0x0007: VoiceCallback block, size 32 (header + handler +
	// data + flags + reserved). Observed immediately after 0x40010000 at frontier.
	alignas(uint64_t) uint8_t callback_blob[32]     = {};
	*reinterpret_cast<uint16_t*>(callback_blob + 0) = 32;
	*reinterpret_cast<int16_t*>(callback_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(callback_blob + 4) = 0x00000007u;
	// Non-null opaque handler/data addresses; HLE accepts registration without
	// inventing host-side invocation of a guest callback.
	*reinterpret_cast<uintptr_t*>(callback_blob + 8)  = static_cast<uintptr_t>(0x1000);
	*reinterpret_cast<uintptr_t*>(callback_blob + 16) = static_cast<uintptr_t>(0x2000);
	*reinterpret_cast<uint32_t*>(callback_blob + 24)  = 0x3u;

	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(callback_blob)), 0);

	// Observed 0x4000-class module param (id 0x40001300, size 48) on CustomSampler.
	alignas(uint64_t) uint8_t custom_module_blob[48]     = {};
	*reinterpret_cast<uint16_t*>(custom_module_blob + 0) = 48;
	*reinterpret_cast<int16_t*>(custom_module_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(custom_module_blob + 4) = 0x40001300u;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(custom_module_blob)), 0);

	// Additional custom-sampler module controls remain opaque until their
	// guest-visible effect is captured; accepting the class must not abort.
	alignas(uint64_t) uint8_t opaque_sampler_blob[8]      = {};
	*reinterpret_cast<uint16_t*>(opaque_sampler_blob + 0) = sizeof(opaque_sampler_blob);
	*reinterpret_cast<uint32_t*>(opaque_sampler_blob + 4) = 0x40010005u;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(opaque_sampler_blob)), 0);

	// Observed GetState size 48 for CustomSampler (same block as Sampler, not 80).
	alignas(uint64_t) uint8_t state_blob[48] = {};
	for (auto& b: state_blob)
	{
		b = 0xa5;
	}
	EXPECT_EQ(Ngs2::Ngs2VoiceGetState(voice, reinterpret_cast<Ngs2::Ngs2VoiceState*>(state_blob), sizeof(state_blob)), 0);
	// state_flags at offset 0 should be written (Empty → 0).
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(state_blob), 0u);

	uint32_t state_flags = UINT32_MAX;
	EXPECT_EQ(Ngs2::Ngs2VoiceGetStateFlags(voice, &state_flags), 0);
	EXPECT_EQ(state_flags, 0u);

	// Keep buffers alive for the process-global NGS lists used by HLE.
	sys_storage.release();
	rack_storage.release();
}

TEST(EmulatorAudio, RendersCapturedCustomSamplerPcmIntoStereoGrain)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	alignas(uint64_t) uint64_t raw_sys_info[8] = {};
	auto*                      sys_info        = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_sys_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, sys_info), 0);
	std::unique_ptr<uint8_t[]> sys_storage(new uint8_t[raw_sys_info[1]]);
	raw_sys_info[0]  = reinterpret_cast<uintptr_t>(sys_storage.get());
	uintptr_t system = 0;
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, sys_info, &system), 0);

	alignas(uint64_t) uint8_t raw_option[0x518]     = {};
	*reinterpret_cast<size_t*>(raw_option)          = sizeof(raw_option);
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;
	alignas(uint64_t) uint64_t raw_rack_info[8]     = {};
	auto*                      rack_info            = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	std::unique_ptr<uint8_t[]> rack_storage(new uint8_t[raw_rack_info[1]]);
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.get());
	uintptr_t rack   = 0;
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info, &rack), 0);
	uintptr_t voice = 0;
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, &voice), 0);

	alignas(uint64_t) uint8_t format_param[40]      = {};
	*reinterpret_cast<uint16_t*>(format_param + 0)  = sizeof(format_param);
	*reinterpret_cast<uint32_t*>(format_param + 4)  = 0x40010000u;
	*reinterpret_cast<uint32_t*>(format_param + 8)  = 0x12u;
	*reinterpret_cast<uint32_t*>(format_param + 12) = 1;
	*reinterpret_cast<uint32_t*>(format_param + 16) = 44100;
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(format_param)), 0);

	constexpr uint64_t        kSourceFrames = 512;
	alignas(uint64_t) int16_t source[kSourceFrames];
	for (auto& sample: source)
	{
		sample = 16384;
	}
	alignas(uint64_t) uint64_t waveform_context[5]     = {0, sizeof(source), 0, kSourceFrames, 0};
	alignas(uint64_t) uint8_t  waveform_param[32]      = {};
	*reinterpret_cast<uint16_t*>(waveform_param + 0)   = sizeof(waveform_param);
	*reinterpret_cast<uint32_t*>(waveform_param + 4)   = 0x40010001u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 8)  = reinterpret_cast<uintptr_t>(source);
	*reinterpret_cast<uint32_t*>(waveform_param + 16)  = 0x11u;
	*reinterpret_cast<uint32_t*>(waveform_param + 20)  = 1u;
	*reinterpret_cast<uintptr_t*>(waveform_param + 24) = reinterpret_cast<uintptr_t>(waveform_context);
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)), 0);

	const uint32_t play_command[3] = {2, 0x400, 1};
	ASSERT_EQ(Ngs2::Ngs2VoiceRunCommands(voice, play_command, 1), 0);

	alignas(uint64_t) float    output[256 * 2] = {};
	alignas(uint64_t) uint64_t render_info[3]  = {reinterpret_cast<uintptr_t>(output), sizeof(output), (uint64_t {2} << 32u) | 24u};
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_NE(output[0], 0.0f);
	EXPECT_FLOAT_EQ(output[0], output[1]);

	alignas(uint64_t) uint64_t destroyed_rack_info[8] = {};
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(destroyed_rack_info)), 0);
	EXPECT_EQ(destroyed_rack_info[0], rack);
	EXPECT_EQ(destroyed_rack_info[1], raw_rack_info[1]);

	std::fill(std::begin(output), std::end(output), 1.0f);
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_FLOAT_EQ(output[0], 0.0f);
	EXPECT_FLOAT_EQ(output[1], 0.0f);

	sys_storage.release();
	rack_storage.release();
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
