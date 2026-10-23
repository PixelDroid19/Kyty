#include "Kyty/UnitTest.h"

#include "Emulator/AudioVideoBackend.h"

#include "UnitTestEmulatorAudioFixtures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

UT_BEGIN(EmulatorAudioDecoderAdmission);

namespace {

namespace AudioVideoBackend = ::Kyty::Emulator::AudioVideoBackend;

using AudioTestFixtures::kGeneratedOpusSinePacket;
using AudioVideoBackend::AudioCodec;
using AudioVideoBackend::Decoder;
using AudioVideoBackend::ElementaryAudioDecoder;

constexpr uint8_t kStereoAtrac9Config[]        = {0xfe, 0x72, 0x09, 0xf0};
constexpr uint8_t kMonoAtrac9Config[]          = {0xfe, 0x70, 0x1f, 0xf0};
constexpr size_t  kOpusPacketSamplesPerChannel = 960;

std::array<uint8_t, ElementaryAudioDecoder::kAtrac9ExtradataSize> MakeAtrac9Extradata(const uint8_t* config)
{
	std::array<uint8_t, ElementaryAudioDecoder::kAtrac9ExtradataSize> extradata {};
	std::memcpy(extradata.data() + 4, config, 4);
	return extradata;
}

void ExpectAtrac9ConfigRejected(const uint8_t* config, size_t config_size)
{
	std::string error;
	auto        decoder = ElementaryAudioDecoder::OpenAtrac9FromConfig(config, config_size, &error);
	EXPECT_FALSE(decoder);
	EXPECT_FALSE(error.empty());
}

} // namespace

TEST(EmulatorAudioDecoderAdmission, Atrac9ConfigDerivesDecoderFormatAndSuperframeSize)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	std::string error;
	auto        stereo = ElementaryAudioDecoder::OpenAtrac9FromConfig(kStereoAtrac9Config, sizeof(kStereoAtrac9Config), &error);
	ASSERT_TRUE(stereo != nullptr) << error;
	EXPECT_EQ(stereo->Channels(), 2u);
	EXPECT_EQ(stereo->SampleRate(), 48000u);
	EXPECT_EQ(stereo->BlockAlign(), 320u);

	auto mono = ElementaryAudioDecoder::OpenAtrac9FromConfig(kMonoAtrac9Config, sizeof(kMonoAtrac9Config), &error);
	ASSERT_TRUE(mono != nullptr) << error;
	EXPECT_EQ(mono->Channels(), 1u);
	EXPECT_EQ(mono->SampleRate(), 48000u);
	EXPECT_EQ(mono->BlockAlign(), 1024u);
}

TEST(EmulatorAudioDecoderAdmission, Atrac9ConfigRejectsUnsupportedAndOversizedInputs)
{
	ExpectAtrac9ConfigRejected(nullptr, sizeof(kStereoAtrac9Config));
	ExpectAtrac9ConfigRejected(kStereoAtrac9Config, sizeof(kStereoAtrac9Config) - 1);

	const uint8_t oversized_config[] = {0xfe, 0x72, 0x09, 0xf0, 0x00};
	ExpectAtrac9ConfigRejected(oversized_config, sizeof(oversized_config));

	uint8_t invalid_config[sizeof(kStereoAtrac9Config)] = {};
	std::memcpy(invalid_config, kStereoAtrac9Config, sizeof(invalid_config));
	invalid_config[0] = 0xfd;
	ExpectAtrac9ConfigRejected(invalid_config, sizeof(invalid_config));

	std::memcpy(invalid_config, kStereoAtrac9Config, sizeof(invalid_config));
	invalid_config[1] |= 1u;
	ExpectAtrac9ConfigRejected(invalid_config, sizeof(invalid_config));

	std::memcpy(invalid_config, kStereoAtrac9Config, sizeof(invalid_config));
	invalid_config[1] = static_cast<uint8_t>((invalid_config[1] & ~0x0eu) | (6u << 1u));
	ExpectAtrac9ConfigRejected(invalid_config, sizeof(invalid_config));

	std::memcpy(invalid_config, kStereoAtrac9Config, sizeof(invalid_config));
	invalid_config[3] |= 1u;
	ExpectAtrac9ConfigRejected(invalid_config, sizeof(invalid_config));

	std::memcpy(invalid_config, kStereoAtrac9Config, sizeof(invalid_config));
	invalid_config[3] = static_cast<uint8_t>((invalid_config[3] & ~0x18u) | 0x08u);
	ExpectAtrac9ConfigRejected(invalid_config, sizeof(invalid_config));
}

TEST(EmulatorAudioDecoderAdmission, Atrac9ConfigOpensWithoutFfmpeg)
{
	if (Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is available";
	}

	std::string error;
	auto        decoder = ElementaryAudioDecoder::OpenAtrac9FromConfig(kStereoAtrac9Config, sizeof(kStereoAtrac9Config), &error);
	ASSERT_TRUE(decoder != nullptr) << error;
	EXPECT_EQ(decoder->BlockAlign(), 320u);
}

TEST(EmulatorAudioDecoderAdmission, ExplicitAtrac9OpenKeepsExpectedFormatStrict)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	const auto  extradata = MakeAtrac9Extradata(kStereoAtrac9Config);
	std::string error;

	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata.data(), extradata.size(), 320, 1, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata.data(), extradata.size(), 320, 2, 44100, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata.data(), extradata.size(), 320, 0, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, extradata.data(), extradata.size(), 320, 2, 0, &error));
}

TEST(EmulatorAudioDecoderAdmission, ExplicitAtrac9OpenRequiresOneConfiguredSuperframe)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	std::string error;
	const auto  stereo_extradata = MakeAtrac9Extradata(kStereoAtrac9Config);
	auto stereo = ElementaryAudioDecoder::Open(AudioCodec::Atrac9, stereo_extradata.data(), stereo_extradata.size(), 320, 2, 48000, &error);
	ASSERT_TRUE(stereo != nullptr) << error;
	EXPECT_EQ(stereo->BlockAlign(), 320u);
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, stereo_extradata.data(), stereo_extradata.size(), 319, 2, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, stereo_extradata.data(), stereo_extradata.size(), 321, 2, 48000, &error));

	const auto mono_extradata = MakeAtrac9Extradata(kMonoAtrac9Config);
	auto mono = ElementaryAudioDecoder::Open(AudioCodec::Atrac9, mono_extradata.data(), mono_extradata.size(), 1024, 1, 48000, &error);
	ASSERT_TRUE(mono != nullptr) << error;
	EXPECT_EQ(mono->BlockAlign(), 1024u);
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, mono_extradata.data(), mono_extradata.size(), 1023, 1, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::Open(AudioCodec::Atrac9, mono_extradata.data(), mono_extradata.size(), 1025, 1, 48000, &error));
}

TEST(EmulatorAudioDecoderAdmission, OpusOpenAcceptsOnlyMonoOrStereoAt48000Hz)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	std::string error;
	auto        mono = ElementaryAudioDecoder::OpenOpus(1, 48000, &error);
	ASSERT_TRUE(mono != nullptr) << error;
	EXPECT_EQ(mono->Channels(), 1u);
	EXPECT_EQ(mono->SampleRate(), 48000u);
	EXPECT_EQ(mono->BlockAlign(), 0u);

	auto stereo = ElementaryAudioDecoder::OpenOpus(2, 48000, &error);
	ASSERT_TRUE(stereo != nullptr) << error;
	EXPECT_EQ(stereo->Channels(), 2u);
	EXPECT_EQ(stereo->SampleRate(), 48000u);
	EXPECT_EQ(stereo->BlockAlign(), 0u);

	EXPECT_FALSE(ElementaryAudioDecoder::OpenOpus(0, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::OpenOpus(3, 48000, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::OpenOpus(2, 44100, &error));
	EXPECT_FALSE(ElementaryAudioDecoder::OpenOpus(2, 0, &error));
}

TEST(EmulatorAudioDecoderAdmission, OpusDecodesConsecutiveGeneratedPacketsAndResets)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	std::string error;
	auto        decoder = ElementaryAudioDecoder::OpenOpus(2, 48000, &error);
	ASSERT_TRUE(decoder != nullptr) << error;
	EXPECT_EQ(decoder->Channels(), 2u);
	EXPECT_EQ(decoder->SampleRate(), 48000u);

	std::vector<float> pcm;
	for (int packet = 0; packet < 2; packet++)
	{
		ASSERT_TRUE(decoder->Decode(kGeneratedOpusSinePacket.data(), kGeneratedOpusSinePacket.size(), &pcm)) << decoder->LastError();
		ASSERT_EQ(pcm.size(), kOpusPacketSamplesPerChannel * 2);
		EXPECT_TRUE(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return std::isfinite(sample); }));
		EXPECT_TRUE(std::any_of(pcm.begin(), pcm.end(), [](float sample) { return std::abs(sample) > 0.0001f; }));
	}

	decoder->Reset();
	pcm.clear();
	ASSERT_TRUE(decoder->Decode(kGeneratedOpusSinePacket.data(), kGeneratedOpusSinePacket.size(), &pcm)) << decoder->LastError();
	EXPECT_EQ(pcm.size(), kOpusPacketSamplesPerChannel * 2);
}

TEST(EmulatorAudioDecoderAdmission, OpusRejectsNullEmptyMalformedAndOversizedPackets)
{
	if (!Decoder::IsAvailable())
	{
		GTEST_SKIP() << "FFmpeg audio decoder backend is unavailable";
	}

	std::string error;
	auto        decoder = ElementaryAudioDecoder::OpenOpus(2, 48000, &error);
	ASSERT_TRUE(decoder != nullptr) << error;

	std::vector<float> pcm;
	EXPECT_FALSE(decoder->Decode(nullptr, 1, &pcm));
	const uint8_t byte = 0;
	EXPECT_FALSE(decoder->Decode(&byte, 0, &pcm));
	const uint8_t malformed[] = {0xff};
	EXPECT_FALSE(decoder->Decode(malformed, sizeof(malformed), &pcm));
	std::vector<uint8_t> oversized(ElementaryAudioDecoder::kOpusMaxPacketBytes + 1, 0);
	EXPECT_FALSE(decoder->Decode(oversized.data(), oversized.size(), &pcm));
	EXPECT_FALSE(decoder->Decode(kGeneratedOpusSinePacket.data(), kGeneratedOpusSinePacket.size(), nullptr));
}

UT_END();
