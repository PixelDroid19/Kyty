#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Audio.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

UT_BEGIN(EmulatorAudio2Attributes);

using namespace Libs::Audio;

namespace {

class GuestBlock
{
public:
	explicit GuestBlock(size_t size): m_address(Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite)) {}
	~GuestBlock()
	{
		if (m_address != 0)
		{
			(void)Core::VirtualMemory::Free(m_address);
		}
	}

	GuestBlock(const GuestBlock&)            = delete;
	GuestBlock& operator=(const GuestBlock&) = delete;

	[[nodiscard]] bool  IsValid() const { return m_address != 0; }
	[[nodiscard]] void* Data() const { return reinterpret_cast<void*>(m_address); }

private:
	uint64_t m_address = 0;
};

struct AttributeEntry
{
	uint32_t id;
	uint32_t reserved;
	uint64_t value;
	uint64_t value_size;
};
static_assert(sizeof(AttributeEntry) == 0x18);

class AudioOut2Fixture
{
public:
	AudioOut2Fixture(): m_context_param(0x40), m_workspace(0x10000), m_memory_size(sizeof(uint64_t)), m_context_out(sizeof(int32_t)),
	                    m_port_param(0x10), m_port_out(sizeof(int32_t))
	{
	}

	~AudioOut2Fixture()
	{
		if (m_port > 0)
		{
			(void)AudioOut2::AudioOut2PortDestroy(m_port);
		}
		if (m_context > 0)
		{
			(void)AudioOut2::AudioOut2ContextDestroy(m_context);
		}
		if (m_audio_subsystem != nullptr)
		{
			m_audio_subsystem->Destroy(Core::SubsystemsList::Instance());
		}
	}

	AudioOut2Fixture(const AudioOut2Fixture&)            = delete;
	AudioOut2Fixture& operator=(const AudioOut2Fixture&) = delete;

	bool Initialize()
	{
		if (!m_context_param.IsValid() || !m_workspace.IsValid() || !m_memory_size.IsValid() || !m_context_out.IsValid() ||
		    !m_port_param.IsValid() || !m_port_out.IsValid())
		{
			return false;
		}
		if (!Config::IsInitialized())
		{
			Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		if (SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 1) != 0)
		{
			return false;
		}

		m_audio_subsystem = static_cast<AudioSubsystem*>(AudioSubsystem::Instance());
		m_audio_subsystem->Destroy(Core::SubsystemsList::Instance());
		m_audio_subsystem->Init(Core::SubsystemsList::Instance());
		if (AudioOut2::AudioOut2Initialize() != 0)
		{
			return false;
		}

		// This complete captured profile selects the observed 512-frame grain.
		constexpr std::array<uint64_t, 8> profile = {0x0000020000000014ull, 0x0000000100000000ull, 0x0000000100000200ull};
		std::memcpy(m_context_param.Data(), profile.data(), sizeof(profile));
		if (AudioOut2::AudioOut2ContextQueryMemory(m_context_param.Data(), static_cast<uint64_t*>(m_memory_size.Data())) != 0 ||
		    *static_cast<const uint64_t*>(m_memory_size.Data()) != 0x10000u ||
		    AudioOut2::AudioOut2ContextCreate(m_context_param.Data(), m_workspace.Data(), 0x10000,
		                                      static_cast<int32_t*>(m_context_out.Data())) != 0)
		{
			return false;
		}
		m_context = *static_cast<const int32_t*>(m_context_out.Data());

		constexpr std::array<uint32_t, 4> port_param = {0, 0x800u, 48000u, 0}; // MAIN, F32, eight channels.
		std::memcpy(m_port_param.Data(), port_param.data(), sizeof(port_param));
		if (AudioOut2::AudioOut2PortCreate(m_context, m_port_param.Data(), static_cast<int32_t*>(m_port_out.Data())) != 0)
		{
			return false;
		}
		m_port = *static_cast<const int32_t*>(m_port_out.Data());
		return m_context > 0 && m_port > 0;
	}

	[[nodiscard]] int32_t Context() const { return m_context; }
	[[nodiscard]] int32_t Port() const { return m_port; }

private:
	GuestBlock      m_context_param;
	GuestBlock      m_workspace;
	GuestBlock      m_memory_size;
	GuestBlock      m_context_out;
	GuestBlock      m_port_param;
	GuestBlock      m_port_out;
	AudioSubsystem* m_audio_subsystem = nullptr;
	int32_t         m_context         = 0;
	int32_t         m_port             = 0;
};

} // namespace

TEST(EmulatorAudio2Attributes, AcceptsCapturedPcmAndGainPairAndRetainsPendingGrainOnRetry)
{
	AudioOut2Fixture fixture;
	ASSERT_TRUE(fixture.Initialize());

	constexpr size_t kFrames = 512;
	constexpr size_t kChannels = 8;
	GuestBlock pcm(sizeof(float) * kFrames * kChannels);
	GuestBlock pcm_pointer(sizeof(uint64_t));
	GuestBlock gains(sizeof(float) * kChannels);
	GuestBlock invalid_gains(sizeof(float) * kChannels);
	GuestBlock attributes(sizeof(AttributeEntry) * 2);
	GuestBlock used(sizeof(uint32_t));
	GuestBlock available(sizeof(uint32_t));
	ASSERT_TRUE(pcm.IsValid());
	ASSERT_TRUE(pcm_pointer.IsValid());
	ASSERT_TRUE(gains.IsValid());
	ASSERT_TRUE(invalid_gains.IsValid());
	ASSERT_TRUE(attributes.IsValid());
	ASSERT_TRUE(used.IsValid());
	ASSERT_TRUE(available.IsValid());

	std::fill_n(static_cast<float*>(pcm.Data()), kFrames * kChannels, 0.25f);
	*static_cast<uint64_t*>(pcm_pointer.Data()) = reinterpret_cast<uint64_t>(pcm.Data());
	std::array<float, kChannels> all_ones {};
	all_ones.fill(1.0f);
	std::memcpy(gains.Data(), all_ones.data(), sizeof(all_ones));

	const std::array<AttributeEntry, 2> valid_pair = {{
	    {0, 0, reinterpret_cast<uint64_t>(pcm_pointer.Data()), sizeof(uint64_t)},
	    {1, 0, reinterpret_cast<uint64_t>(gains.Data()), sizeof(all_ones)},
	}};
	std::memcpy(attributes.Data(), valid_pair.data(), sizeof(valid_pair));
	// This is the captured main-port call shape; the pre-integration baseline rejects count != 1.
	ASSERT_EQ(AudioOut2::AudioOut2PortSetAttributes(fixture.Port(), attributes.Data(), 2), 0);

	const std::array<AttributeEntry, 2> unknown_second = {{
	    {0, 0, reinterpret_cast<uint64_t>(pcm_pointer.Data()), sizeof(uint64_t)},
	    {7, 0, reinterpret_cast<uint64_t>(gains.Data()), sizeof(all_ones)},
	}};
	std::memcpy(attributes.Data(), unknown_second.data(), sizeof(unknown_second));
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(fixture.Port(), attributes.Data(), 2), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
	std::memcpy(attributes.Data(), valid_pair.data(), sizeof(valid_pair));
	ASSERT_EQ(AudioOut2::AudioOut2PortSetAttributes(fixture.Port(), attributes.Data(), 2), 0);

	std::array<float, kChannels> nan_gains = all_ones;
	nan_gains[3] = std::numeric_limits<float>::quiet_NaN();
	std::memcpy(invalid_gains.Data(), nan_gains.data(), sizeof(nan_gains));
	const std::array<AttributeEntry, 2> non_finite_second = {{
	    {0, 0, reinterpret_cast<uint64_t>(pcm_pointer.Data()), sizeof(uint64_t)},
	    {1, 0, reinterpret_cast<uint64_t>(invalid_gains.Data()), sizeof(nan_gains)},
	}};
	std::memcpy(attributes.Data(), non_finite_second.data(), sizeof(non_finite_second));
	EXPECT_EQ(AudioOut2::AudioOut2PortSetAttributes(fixture.Port(), attributes.Data(), 2), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);

	ASSERT_EQ(AudioOut2::AudioOut2ContextAdvance(fixture.Context()), 0);
	AudioOut2::HostStateTest::FailNextSubmit();
	EXPECT_EQ(AudioOut2::AudioOut2ContextPush(fixture.Context(), 0), AUDIO_OUT_ERROR_INVALID_PORT);
	EXPECT_GT(AudioOut2::HostStateTest::GetContextSinkHandle(fixture.Context()), 0);
	ASSERT_EQ(AudioOut2::AudioOut2ContextPush(fixture.Context(), 0), 0);
	ASSERT_EQ(AudioOut2::AudioOut2ContextGetQueueLevel(fixture.Context(), static_cast<uint32_t*>(used.Data()),
	                                                   static_cast<uint32_t*>(available.Data())),
	          0);
	EXPECT_EQ(*static_cast<const uint32_t*>(used.Data()), 1u);
}

UT_END();
