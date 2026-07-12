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
	alignas(uint64_t) uint8_t raw_option[0xb8] = {};
	*reinterpret_cast<size_t*>(raw_option)     = sizeof(raw_option);
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

	uintptr_t voice0 = 0;
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
	alignas(uint64_t) uint8_t param_blob[40] = {};
	*reinterpret_cast<uint16_t*>(param_blob + 0) = 40;
	*reinterpret_cast<int16_t*>(param_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(param_blob + 4) = 0x40010000u;

	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param_blob)), 0);

	// Common param id 0x0007: VoiceCallback block, size 32 (header + handler +
	// data + flags + reserved). Observed immediately after 0x40010000 at frontier.
	alignas(uint64_t) uint8_t callback_blob[32] = {};
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
	alignas(uint64_t) uint8_t custom_module_blob[48] = {};
	*reinterpret_cast<uint16_t*>(custom_module_blob + 0) = 48;
	*reinterpret_cast<int16_t*>(custom_module_blob + 2)  = 0;
	*reinterpret_cast<uint32_t*>(custom_module_blob + 4) = 0x40001300u;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(custom_module_blob)), 0);

	// Observed GetState size 48 for CustomSampler (same block as Sampler, not 80).
	alignas(uint64_t) uint8_t state_blob[48] = {};
	for (auto& b: state_blob)
	{
		b = 0xa5;
	}
	EXPECT_EQ(Ngs2::Ngs2VoiceGetState(voice, reinterpret_cast<Ngs2::Ngs2VoiceState*>(state_blob), sizeof(state_blob)), 0);
	// state_flags at offset 0 should be written (Empty → 0).
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(state_blob), 0u);

	// Keep buffers alive for the process-global NGS lists used by HLE.
	sys_storage.release();
	rack_storage.release();
}

UT_END();
