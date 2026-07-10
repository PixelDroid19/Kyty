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

UT_END();
