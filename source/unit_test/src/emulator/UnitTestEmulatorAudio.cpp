#include "Kyty/UnitTest.h"

#include "Emulator/Audio.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include <memory>

UT_BEGIN(EmulatorAudio);

using namespace Libs::Audio;

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

UT_END();
