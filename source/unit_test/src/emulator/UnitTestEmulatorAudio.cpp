#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Audio.h"
#include "Emulator/AudioNgs2Sampler.h"
#include "Emulator/AudioPcm.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Config.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include "SDL.h"
#include "UnitTestEmulatorAudioFixtures.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Kyty::Libs::LibC {
void* KYTY_SYSV_ABI c_malloc(size_t size);
void KYTY_SYSV_ABI  c_free(void* ptr);
} // namespace Kyty::Libs::LibC

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

#if defined(__linux__)
class GuestSharedBlock
{
public:
	explicit GuestSharedBlock(size_t size)
	{
		const uint64_t page_size = Core::VirtualMemory::GetPageSize();
		if (page_size == 0 || size == 0)
		{
			return;
		}
		m_size    = ((size + page_size - 1u) / page_size) * page_size;
		m_backing = Core::VirtualMemory::CreateSharedBacking(m_size);
		if (m_backing != nullptr)
		{
			m_address = Core::VirtualMemory::MapSharedAligned(m_backing, 0, 0, m_size, Core::VirtualMemory::Mode::ReadWrite, page_size);
		}
	}
	~GuestSharedBlock()
	{
		if (m_address != 0)
		{
			(void)Core::VirtualMemory::Free(m_address);
		}
		if (m_backing != nullptr)
		{
			Core::VirtualMemory::DestroySharedBacking(m_backing);
		}
	}
	GuestSharedBlock(const GuestSharedBlock&)            = delete;
	GuestSharedBlock& operator=(const GuestSharedBlock&) = delete;

	[[nodiscard]] bool  IsValid() const { return m_address != 0; }
	[[nodiscard]] void* Data() const { return reinterpret_cast<void*>(m_address); }

private:
	uint64_t                            m_size    = 0;
	uint64_t                            m_address = 0;
	Core::VirtualMemory::SharedBacking* m_backing = nullptr;
};
#endif

bool IsAjmBytePattern(const void* memory, size_t size, uint8_t expected)
{
	if (size == 0)
	{
		return true;
	}
	const auto* bytes = static_cast<const uint8_t*>(memory);
	return std::all_of(bytes, bytes + size, [expected](uint8_t value) { return value == expected; });
}

#if defined(__linux__)
constexpr int AJM_STRICT_EXIT_STATUS = 65; // dbg_exit(321) is truncated by POSIX process status.

struct AjmBatchSharedResults
{
	alignas(uint64_t) uint8_t data_output[32];
	alignas(uint64_t) uint8_t init_result[32];
	alignas(uint64_t) uint8_t decode_result[32];
	alignas(uint64_t) uint8_t sideband_output[56];
	Ajm::AjmBatchError error;
	uint32_t           batch_id;
};

class SharedAjmBatchResults
{
public:
	SharedAjmBatchResults()
	{
		void* const storage = ::mmap(nullptr, sizeof(AjmBatchSharedResults), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (storage != MAP_FAILED)
		{
			m_results = ::new (storage) AjmBatchSharedResults {};
			Reset();
		}
	}
	~SharedAjmBatchResults()
	{
		if (m_results != nullptr)
		{
			m_results->~AjmBatchSharedResults();
			(void)::munmap(m_results, sizeof(*m_results));
		}
	}
	SharedAjmBatchResults(const SharedAjmBatchResults&)            = delete;
	SharedAjmBatchResults& operator=(const SharedAjmBatchResults&) = delete;

	[[nodiscard]] bool                   IsValid() const { return m_results != nullptr; }
	[[nodiscard]] AjmBatchSharedResults* Data() const { return m_results; }
	void Reset() { std::memset(m_results, 0xa5, sizeof(*m_results)); }

private:
	AjmBatchSharedResults* m_results = nullptr;
};

constexpr size_t AJM_OPUS_TEST_OUTPUT_CAPACITY = 12288;
constexpr size_t AJM_OPUS_TEST_BELOW_PACKET_CAPACITY = 512 * 2 * sizeof(int16_t);
bool WaitForAjmChild(pid_t child, int* status)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	for (;;)
	{
		const pid_t result = ::waitpid(child, status, WNOHANG);
		if (result == child)
		{
			return true;
		}
		if (result < 0 && errno != EINTR)
		{
			(void)::kill(child, SIGKILL);
			(void)::waitpid(child, status, 0);
			return false;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			(void)::kill(child, SIGKILL);
			while (::waitpid(child, status, 0) < 0 && errno == EINTR)
			{
			}
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

template <typename Callback>
bool RunAjmStrictChild(Callback callback, int* status)
{
	const pid_t child = ::fork();
	if (child < 0)
	{
		return false;
	}
	if (child == 0)
	{
		callback();
		std::_Exit(0);
	}
	return WaitForAjmChild(child, status);
}

enum class AjmDataTestOperation : uint8_t
{
	Decode,
	DecodeSingle,
	DecodeSplit,
	Encode,
	Run,
	RunSplit,
};

int QueueAjmDataTestOperation(AjmDataTestOperation operation, void* batch, uint32_t instance, const uint8_t* input, uint8_t* data_output,
                              uint8_t* decode_result, uint8_t* sideband_output)
{
	constexpr size_t     input_size  = 4;
	constexpr size_t     output_size = 32;
	constexpr uint64_t   run_flags   = (1ull << 47u) | 1u;
	const Ajm::AjmBuffer input_buffer {const_cast<uint8_t*>(input), input_size};
	const Ajm::AjmBuffer output_buffer {data_output, output_size};
	const Ajm::AjmBuffer input_buffers[] {input_buffer};
	const Ajm::AjmBuffer output_buffers[] {output_buffer};
	switch (operation)
	{
		case AjmDataTestOperation::Decode:
			return Ajm::AjmBatchJobDecode(batch, instance, input, input_size, data_output, output_size, decode_result);
		case AjmDataTestOperation::DecodeSingle:
			return Ajm::AjmBatchJobDecodeSingle(batch, instance, input, input_size, data_output, output_size, decode_result);
		case AjmDataTestOperation::DecodeSplit:
		{
			GuestReadableBlock input_descriptor_storage(sizeof(input_buffer));
			GuestReadableBlock output_descriptor_storage(sizeof(output_buffer));
			if (!input_descriptor_storage.IsValid() || !output_descriptor_storage.IsValid())
			{
				return static_cast<int32_t>(0x80930005u);
			}
			auto* const guest_input_buffer  = static_cast<Ajm::AjmBuffer*>(input_descriptor_storage.Data());
			auto* const guest_output_buffer = static_cast<Ajm::AjmBuffer*>(output_descriptor_storage.Data());
			*guest_input_buffer             = input_buffer;
			*guest_output_buffer            = output_buffer;
			// DecodeSplit snapshots descriptor metadata at enqueue, so these mappings may be released on return.
			return Ajm::AjmBatchJobDecodeSplit(batch, instance, guest_input_buffer, 1, guest_output_buffer, 1, decode_result);
		}
		case AjmDataTestOperation::Encode:
			return Ajm::AjmBatchJobEncode(batch, instance, input, input_size, data_output, output_size, decode_result);
		case AjmDataTestOperation::Run:
			return Ajm::AjmBatchJobRun(batch, instance, run_flags, input, input_size, data_output, output_size, sideband_output, 56);
		case AjmDataTestOperation::RunSplit:
			return Ajm::AjmBatchJobRunSplit(batch, instance, run_flags, input_buffers, 1, output_buffers, 1, sideband_output, 56);
	}
	std::abort();
}

int RunAjmOpusSplitScenario(const std::array<uint8_t, 648>& framed_input, const std::array<std::vector<uint8_t>, 4>& expected_packet_pcm)
{
	constexpr size_t framed_packet_bytes = 162;
	constexpr size_t packet_pcm_bytes = 960 * 2 * sizeof(int16_t);
	constexpr size_t first_output_bytes = 3 * packet_pcm_bytes;
	constexpr size_t tail_output_bytes = packet_pcm_bytes - AJM_OPUS_TEST_BELOW_PACKET_CAPACITY;
	constexpr size_t pcm_frame_bytes = 2 * sizeof(int16_t);
	constexpr size_t pending_first_bytes = 371 * pcm_frame_bytes;
	constexpr size_t pending_carry_bytes = 512 * pcm_frame_bytes;
	constexpr size_t pending_final_bytes = 77 * pcm_frame_bytes;
	constexpr size_t pending_case_bytes = 2 * packet_pcm_bytes;
	std::vector<uint8_t> expected_pcm;
	for (const auto& packet: expected_packet_pcm)
	{
		expected_pcm.insert(expected_pcm.end(), packet.begin(), packet.end());
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock             config_storage(12);
	GuestReadableBlock             input_storage(framed_input.size());
	GuestReadableBlock             input_descriptors_storage(17 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_descriptor_storage(17 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             below_packet_output_storage(AJM_OPUS_TEST_BELOW_PACKET_CAPACITY + sizeof(int32_t));
	GuestReadableBlock             output_storage(AJM_OPUS_TEST_OUTPUT_CAPACITY);
	GuestReadableBlock             init_result_storage(32);
	GuestReadableBlock             below_packet_result_storage(32);
	GuestReadableBlock             first_result_storage(32);
	GuestReadableBlock             second_result_storage(32);
	GuestReadableBlock             clear_result_storage(32);
	GuestReadableBlock             reset_result_storage(32);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	if (!config_storage.IsValid() || !input_storage.IsValid() || !input_descriptors_storage.IsValid() ||
	    !output_descriptor_storage.IsValid() || !below_packet_output_storage.IsValid() || !output_storage.IsValid() ||
	    !init_result_storage.IsValid() || !below_packet_result_storage.IsValid() || !first_result_storage.IsValid() ||
	    !second_result_storage.IsValid() || !clear_result_storage.IsValid() || !reset_result_storage.IsValid() ||
	    !error_storage.IsValid() || !batch_id_storage.IsValid())
	{
		return 2;
	}

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(input_storage.Data(), framed_input.data(), framed_input.size());
	std::memset(init_result_storage.Data(), 0xa5, 32);
	std::memset(below_packet_result_storage.Data(), 0xa5, 32);
	std::memset(first_result_storage.Data(), 0xa5, 32);
	std::memset(second_result_storage.Data(), 0xa5, 32);
	std::memset(clear_result_storage.Data(), 0xa5, 32);
	std::memset(reset_result_storage.Data(), 0xa5, 32);

	uint32_t context = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0)
	{
		return 3;
	}
	uint32_t instance = 0;
	if (Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 || instance != ((24u << 14u) | 1u))
	{
		return 4;
	}

	auto* const               input_buffers  = static_cast<Ajm::AjmBuffer*>(input_descriptors_storage.Data());
	auto* const               output_buffers = static_cast<Ajm::AjmBuffer*>(output_descriptor_storage.Data());
	auto* const               input_bytes    = static_cast<uint8_t*>(input_storage.Data());
	alignas(uint64_t) uint8_t batch[256] {};
	for (size_t index = 0; index < 17; ++index)
	{
		input_buffers[index]  = {input_bytes, 1};
		output_buffers[index] = {output_storage.Data(), 1};
	}
	constexpr int32_t invalid_parameter = static_cast<int32_t>(0x80930005u);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 17, output_buffers, 1, nullptr) != invalid_parameter ||
	    Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 17, nullptr) != invalid_parameter)
	{
		return 9;
	}
	input_buffers[0]  = {input_bytes, 1024u * 1024u + 1u};
	output_buffers[0] = {output_storage.Data(), AJM_OPUS_TEST_OUTPUT_CAPACITY};
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, nullptr) != invalid_parameter)
	{
		return 8;
	}

	if (Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), 12, init_result_storage.Data()) != 0)
	{
		return 10;
	}
	input_buffers[0]  = {input_bytes, 487};
	input_buffers[1]  = {input_bytes + 487, 161};
	output_buffers[0] = {below_packet_output_storage.Data(), AJM_OPUS_TEST_BELOW_PACKET_CAPACITY};
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 2, output_buffers, 1, below_packet_result_storage.Data()) != 0)
	{
		return 11;
	}
	// Descriptor metadata is snapshotted at enqueue; the pointed-to input bytes are copied at Start.
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	input_buffers[1]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};

	auto start_and_wait = [&]()
	{
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		if (Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 || *batch_id_storage.Data() == 0 ||
		    error_storage.Data()->error_code != 0)
		{
			return false;
		}
		return Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) == 0;
	};
	std::memset(below_packet_output_storage.Data(), 0xa5, AJM_OPUS_TEST_BELOW_PACKET_CAPACITY + sizeof(int32_t));
	if (!start_and_wait())
	{
		return 12;
	}
	auto read_i32 = [](const uint8_t* result, size_t offset)
	{
		int32_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	auto read_u64 = [](const uint8_t* result, size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	auto opus_decode_result_matches = [&](const void* result, size_t consumed, size_t produced, uint64_t delivered)
	{
		const auto* bytes = static_cast<const uint8_t*>(result);
		return IsAjmBytePattern(bytes, 8, 0) && IsAjmBytePattern(bytes + 24, 8, 0xa5) &&
		       read_i32(bytes, 8) == static_cast<int32_t>(consumed) && read_i32(bytes, 12) == static_cast<int32_t>(produced) &&
		       read_u64(bytes, 16) == delivered;
	};
	if (!IsAjmBytePattern(init_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(init_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(below_packet_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(below_packet_result_storage.Data()) + 24, 8, 0xa5) ||
	    std::memcmp(below_packet_output_storage.Data(), expected_pcm.data(), AJM_OPUS_TEST_BELOW_PACKET_CAPACITY) != 0)
	{
		return 16;
	}
	if (read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 4) != 0 ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 8) != static_cast<int32_t>(framed_packet_bytes) ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 12) !=
	        static_cast<int32_t>(AJM_OPUS_TEST_BELOW_PACKET_CAPACITY) ||
	    read_u64(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 16) != AJM_OPUS_TEST_BELOW_PACKET_CAPACITY / 4)
	{
		return 17;
	}

	input_buffers[0]  = {input_bytes + framed_packet_bytes, 325};
	input_buffers[1]  = {input_bytes + 487, 161};
	output_buffers[0] = {output_storage.Data(), 5001};
	output_buffers[1] = {static_cast<uint8_t*>(output_storage.Data()) + 5001, first_output_bytes - 5001};
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 2, output_buffers, 2, first_result_storage.Data()) != 0)
	{
		return 18;
	}
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	input_buffers[1]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[1] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	std::memset(output_storage.Data(), 0xa5, AJM_OPUS_TEST_OUTPUT_CAPACITY);
	if (!start_and_wait())
	{
		return 19;
	}
	if (!IsAjmBytePattern(static_cast<const uint8_t*>(first_result_storage.Data()) + 24, 8, 0xa5))
	{
		return 20;
	}
	// Interpret the cumulative counter as per-channel frames for this oracle; its AJM unit remains inferred.
	if (read_i32(static_cast<const uint8_t*>(first_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(first_result_storage.Data()), 4) != 0 ||
	    read_i32(static_cast<const uint8_t*>(first_result_storage.Data()), 8) != 3 * static_cast<int32_t>(framed_packet_bytes) ||
	    read_i32(static_cast<const uint8_t*>(first_result_storage.Data()), 12) != 3 * static_cast<int32_t>(packet_pcm_bytes) ||
	    read_u64(static_cast<const uint8_t*>(first_result_storage.Data()), 16) !=
	        (AJM_OPUS_TEST_BELOW_PACKET_CAPACITY + first_output_bytes) / 4 ||
	    std::memcmp(output_storage.Data(), expected_pcm.data() + AJM_OPUS_TEST_BELOW_PACKET_CAPACITY, first_output_bytes) != 0 ||
	    !IsAjmBytePattern(static_cast<uint8_t*>(output_storage.Data()) + 3 * packet_pcm_bytes,
	                      AJM_OPUS_TEST_OUTPUT_CAPACITY - 3 * packet_pcm_bytes, 0xa5))
	{
		return 17;
	}

	std::memset(output_storage.Data(), 0xa5, AJM_OPUS_TEST_OUTPUT_CAPACITY);
	std::memset(second_result_storage.Data(), 0xa5, 32);
	output_buffers[0] = {output_storage.Data(), AJM_OPUS_TEST_OUTPUT_CAPACITY};
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, nullptr, 0, output_buffers, 1, second_result_storage.Data()) != 0 ||
	    !start_and_wait())
	{
		return 13;
	}
	if (!IsAjmBytePattern(static_cast<const uint8_t*>(second_result_storage.Data()) + 24, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(second_result_storage.Data()), 8) != 0 ||
	    read_i32(static_cast<const uint8_t*>(second_result_storage.Data()), 12) != static_cast<int32_t>(tail_output_bytes) ||
	    read_u64(static_cast<const uint8_t*>(second_result_storage.Data()), 16) != 4u * 960u ||
	    std::memcmp(output_storage.Data(), expected_pcm.data() + expected_pcm.size() - tail_output_bytes, tail_output_bytes) != 0 ||
	    !IsAjmBytePattern(static_cast<uint8_t*>(output_storage.Data()) + tail_output_bytes, AJM_OPUS_TEST_OUTPUT_CAPACITY - tail_output_bytes,
	                      0xa5))
	{
		return 18;
	}

	input_buffers[0] = {input_bytes, framed_packet_bytes};
	output_buffers[0] = {below_packet_output_storage.Data(), AJM_OPUS_TEST_BELOW_PACKET_CAPACITY};
	std::memset(below_packet_output_storage.Data(), 0xa5, AJM_OPUS_TEST_BELOW_PACKET_CAPACITY + sizeof(int32_t));
	std::memset(below_packet_result_storage.Data(), 0xa5, 32);
	std::memset(clear_result_storage.Data(), 0xa5, 32);
	std::memset(reset_result_storage.Data(), 0xa5, 32);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, below_packet_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobClearContext(batch, instance, clear_result_storage.Data()) != 0)
	{
		return 14;
	}
	input_buffers[0]  = {input_bytes, framed_packet_bytes};
	output_buffers[0] = {output_storage.Data(), AJM_OPUS_TEST_BELOW_PACKET_CAPACITY};
	std::memset(output_storage.Data(), 0xa5, AJM_OPUS_TEST_OUTPUT_CAPACITY);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, reset_result_storage.Data()) != 0)
	{
		return 14;
	}
	input_buffers[0] = {input_bytes + framed_packet_bytes, framed_packet_bytes};
	output_buffers[0] = {static_cast<uint8_t*>(output_storage.Data()) + AJM_OPUS_TEST_BELOW_PACKET_CAPACITY,
	                     2 * packet_pcm_bytes - AJM_OPUS_TEST_BELOW_PACKET_CAPACITY};
	std::memset(second_result_storage.Data(), 0xa5, 32);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, second_result_storage.Data()) != 0)
	{
		return 14;
	}
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	if (!start_and_wait())
	{
		return 14;
	}
	if (read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 4) != 0 ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 8) != static_cast<int32_t>(framed_packet_bytes) ||
	    read_i32(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 12) != static_cast<int32_t>(AJM_OPUS_TEST_BELOW_PACKET_CAPACITY) ||
	    read_u64(static_cast<const uint8_t*>(below_packet_result_storage.Data()), 16) !=
	        3840u + AJM_OPUS_TEST_BELOW_PACKET_CAPACITY / 4 ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(below_packet_result_storage.Data()) + 24, 8, 0xa5) ||
	    !IsAjmBytePattern(static_cast<uint8_t*>(below_packet_output_storage.Data()) + AJM_OPUS_TEST_BELOW_PACKET_CAPACITY,
	                      sizeof(int32_t), 0xa5) ||
	    !IsAjmBytePattern(clear_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(clear_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(reset_result_storage.Data()) + 24, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(reset_result_storage.Data()), 8) != static_cast<int32_t>(framed_packet_bytes) ||
	    read_i32(static_cast<const uint8_t*>(reset_result_storage.Data()), 12) != static_cast<int32_t>(AJM_OPUS_TEST_BELOW_PACKET_CAPACITY) ||
	    read_u64(static_cast<const uint8_t*>(reset_result_storage.Data()), 16) != AJM_OPUS_TEST_BELOW_PACKET_CAPACITY / 4 ||
	    !IsAjmBytePattern(second_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(second_result_storage.Data()) + 24, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(second_result_storage.Data()), 8) != static_cast<int32_t>(framed_packet_bytes) ||
	    read_i32(static_cast<const uint8_t*>(second_result_storage.Data()), 12) !=
	        static_cast<int32_t>(2 * packet_pcm_bytes - AJM_OPUS_TEST_BELOW_PACKET_CAPACITY) ||
	    read_u64(static_cast<const uint8_t*>(second_result_storage.Data()), 16) != 1920u ||
	    std::memcmp(output_storage.Data(), expected_pcm.data(), 2 * packet_pcm_bytes) != 0 ||
	    !IsAjmBytePattern(static_cast<uint8_t*>(output_storage.Data()) + 2 * packet_pcm_bytes, AJM_OPUS_TEST_OUTPUT_CAPACITY - 2 * packet_pcm_bytes,
	                      0xa5))
	{
		return 19;
	}

	// Keep 589 frames pending, drain them across two bounded outputs, then retry the offered packet.
	std::memset(output_storage.Data(), 0xa5, AJM_OPUS_TEST_OUTPUT_CAPACITY);
	std::memset(clear_result_storage.Data(), 0xa5, 32);
	std::memset(reset_result_storage.Data(), 0xa5, 32);
	input_buffers[0]  = {input_bytes, framed_packet_bytes};
	output_buffers[0] = {output_storage.Data(), pending_first_bytes};
	if (Ajm::AjmBatchJobClearContext(batch, instance, clear_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, reset_result_storage.Data()) != 0)
	{
		return 21;
	}
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	if (!start_and_wait() || !IsAjmBytePattern(clear_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(clear_result_storage.Data()) + 8, 24, 0xa5) ||
	    !opus_decode_result_matches(reset_result_storage.Data(), framed_packet_bytes, pending_first_bytes,
	                                pending_first_bytes / pcm_frame_bytes))
	{
		return 22;
	}

	input_buffers[0]  = {input_bytes + framed_packet_bytes, framed_packet_bytes};
	output_buffers[0] = {static_cast<uint8_t*>(output_storage.Data()) + pending_first_bytes, pending_carry_bytes};
	std::memset(first_result_storage.Data(), 0xa5, 32);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, first_result_storage.Data()) != 0)
	{
		return 23;
	}
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	if (!start_and_wait() || !opus_decode_result_matches(first_result_storage.Data(), 0, pending_carry_bytes,
	                                                     (pending_first_bytes + pending_carry_bytes) / pcm_frame_bytes))
	{
		return 24;
	}

	output_buffers[0] = {static_cast<uint8_t*>(output_storage.Data()) + pending_first_bytes + pending_carry_bytes,
	                     pending_final_bytes};
	std::memset(second_result_storage.Data(), 0xa5, 32);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, nullptr, 0, output_buffers, 1, second_result_storage.Data()) != 0)
	{
		return 25;
	}
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	if (!start_and_wait() || !opus_decode_result_matches(second_result_storage.Data(), 0, pending_final_bytes,
	                                                     packet_pcm_bytes / pcm_frame_bytes))
	{
		return 26;
	}

	input_buffers[0]  = {input_bytes + framed_packet_bytes, framed_packet_bytes};
	output_buffers[0] = {static_cast<uint8_t*>(output_storage.Data()) + packet_pcm_bytes, packet_pcm_bytes};
	std::memset(below_packet_result_storage.Data(), 0xa5, 32);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, below_packet_result_storage.Data()) != 0)
	{
		return 27;
	}
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	if (!start_and_wait() || !opus_decode_result_matches(below_packet_result_storage.Data(), framed_packet_bytes, packet_pcm_bytes,
	                                                     2 * packet_pcm_bytes / pcm_frame_bytes) ||
	    std::memcmp(output_storage.Data(), expected_pcm.data(), pending_case_bytes) != 0 ||
	    !IsAjmBytePattern(static_cast<uint8_t*>(output_storage.Data()) + pending_case_bytes,
	                      AJM_OPUS_TEST_OUTPUT_CAPACITY - pending_case_bytes, 0xa5) ||
	    std::memcmp(input_storage.Data(), framed_input.data(), framed_input.size()) != 0)
	{
		return 28;
	}

	constexpr size_t batch_decoder_count = 15;
	std::array<uint32_t, batch_decoder_count>                       batch_instances {};
	std::array<std::unique_ptr<GuestReadableBlock>, batch_decoder_count> batch_output_storage;
	std::array<std::unique_ptr<GuestReadableBlock>, batch_decoder_count> batch_result_storage;
	for (size_t index = 0; index < batch_decoder_count; ++index)
	{
		if (Ajm::AjmInstanceCreate(context, 24, 0x1002, &batch_instances[index]) != 0)
		{
			return 29;
		}
		batch_output_storage[index] = std::make_unique<GuestReadableBlock>(packet_pcm_bytes + sizeof(uint32_t));
		batch_result_storage[index] = std::make_unique<GuestReadableBlock>(32);
		if (!batch_output_storage[index]->IsValid() || !batch_result_storage[index]->IsValid())
		{
			return 30;
		}
		std::memset(batch_output_storage[index]->Data(), 0xa5, packet_pcm_bytes + sizeof(uint32_t));
		std::memset(batch_result_storage[index]->Data(), 0xa5, 32);
		std::memset(init_result_storage.Data(), 0xa5, 32);
		if (Ajm::AjmBatchJobInitialize(batch, batch_instances[index], config_storage.Data(), 12, init_result_storage.Data()) != 0 ||
		    !start_and_wait() || !IsAjmBytePattern(init_result_storage.Data(), 8, 0) ||
		    !IsAjmBytePattern(static_cast<const uint8_t*>(init_result_storage.Data()) + 8, 24, 0xa5))
		{
			return 31;
		}
	}

	auto queue_batch_decode = [&](void* queued_batch, size_t packet_index)
	{
		uint8_t* const packet = input_bytes + packet_index * framed_packet_bytes;
		for (size_t index = 0; index < batch_decoder_count; ++index)
		{
			input_buffers[0]  = {packet, framed_packet_bytes};
			output_buffers[0] = {batch_output_storage[index]->Data(), packet_pcm_bytes};
			if (Ajm::AjmBatchJobDecodeSplit(queued_batch, batch_instances[index], input_buffers, 1, output_buffers, 1,
			                                batch_result_storage[index]->Data()) != 0)
			{
				return false;
			}
		}
		input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
		output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
		return true;
	};
	auto start_and_wait_queued_batch = [&](void* queued_batch)
	{
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		if (Ajm::AjmBatchStart(context, queued_batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 ||
		    *batch_id_storage.Data() == 0 || error_storage.Data()->error_code != 0)
		{
			return false;
		}
		return Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) == 0 &&
		       error_storage.Data()->error_code == 0;
	};

	// Disjoint PCM/result pairs for every decoder make the final output preflight fail late.
	alignas(uint64_t) uint8_t invalid_output_batch[512] {};
	std::memset(error_storage.Data(), 0xa5, sizeof(Ajm::AjmBatchError));
	*batch_id_storage.Data() = 0xa5a5a5a5u;
	if (!queue_batch_decode(invalid_output_batch, 0) || !batch_output_storage.back()->Protect(Core::VirtualMemory::Mode::Read))
	{
		return 32;
	}
	const int32_t invalid_output_status =
	    Ajm::AjmBatchStart(context, invalid_output_batch, 0, error_storage.Data(), batch_id_storage.Data());
	const bool output_mapping_restored = batch_output_storage.back()->Protect(Core::VirtualMemory::Mode::ReadWrite);
	if (!output_mapping_restored)
	{
		return 33;
	}
	if (invalid_output_status != invalid_parameter || !IsAjmBytePattern(error_storage.Data(), sizeof(Ajm::AjmBatchError), 0xa5) ||
	    *batch_id_storage.Data() != 0xa5a5a5a5u)
	{
		return 34;
	}
	for (size_t index = 0; index < batch_decoder_count; ++index)
	{
		if (!IsAjmBytePattern(batch_output_storage[index]->Data(), packet_pcm_bytes + sizeof(uint32_t), 0xa5) ||
		    !IsAjmBytePattern(batch_result_storage[index]->Data(), 32, 0xa5))
		{
			return 35;
		}
	}

	// Retry the first packet on every instance; exact oracle PCM proves the refused batch did not advance them.
	alignas(uint64_t) uint8_t valid_output_batch[512] {};
	for (size_t index = 0; index < batch_decoder_count; ++index)
	{
		std::memset(batch_output_storage[index]->Data(), 0xa5, packet_pcm_bytes + sizeof(uint32_t));
		std::memset(batch_result_storage[index]->Data(), 0xa5, 32);
	}
	if (!queue_batch_decode(valid_output_batch, 0) || !start_and_wait_queued_batch(valid_output_batch) ||
	    *batch_id_storage.Data() == 0 || error_storage.Data()->error_code != 0 ||
	    std::memcmp(input_storage.Data(), framed_input.data(), framed_input.size()) != 0)
	{
		return 36;
	}
	for (size_t index = 0; index < batch_decoder_count; ++index)
	{
		if (!opus_decode_result_matches(batch_result_storage[index]->Data(), framed_packet_bytes, packet_pcm_bytes,
		                                packet_pcm_bytes / pcm_frame_bytes) ||
		    std::memcmp(batch_output_storage[index]->Data(), expected_packet_pcm[0].data(), packet_pcm_bytes) != 0 ||
		    !IsAjmBytePattern(static_cast<uint8_t*>(batch_output_storage[index]->Data()) + packet_pcm_bytes, sizeof(uint32_t), 0xa5))
		{
			return 37;
		}
	}

	// Reset each decoder independently, then fill a batch with the most scattered outputs a job can publish.
	constexpr size_t max_batch_packets_per_decoder = 4;
	constexpr size_t max_batch_job_count          = batch_decoder_count * max_batch_packets_per_decoder;
	constexpr size_t max_scatter_count            = 16;
	constexpr size_t max_scatter_bytes            = packet_pcm_bytes / max_scatter_count;
	constexpr size_t max_scatter_stride           = max_scatter_bytes + sizeof(uint32_t);
	constexpr size_t max_output_storage_bytes     = max_scatter_count * max_scatter_stride;
	static_assert(packet_pcm_bytes % max_scatter_count == 0);
	static_assert(sizeof(Ajm::AjmBatchError) == 32);
	static_assert(max_batch_job_count * (max_scatter_count + 1) + 2 == 1022);

	alignas(uint64_t) uint8_t reset_batches[batch_decoder_count][512] {};
	for (size_t index = 0; index < batch_decoder_count; ++index)
	{
		std::memset(clear_result_storage.Data(), 0xa5, 32);
		if (Ajm::AjmBatchJobClearContext(reset_batches[index], batch_instances[index], clear_result_storage.Data()) != 0 ||
		    !start_and_wait_queued_batch(reset_batches[index]) || !IsAjmBytePattern(clear_result_storage.Data(), 8, 0) ||
		    !IsAjmBytePattern(static_cast<const uint8_t*>(clear_result_storage.Data()) + 8, 24, 0xa5))
		{
			return 39;
		}
	}

	struct MaxCapacityJobStorage
	{
		std::unique_ptr<GuestReadableBlock> output;
		std::unique_ptr<GuestReadableBlock> result;
	};
	std::array<MaxCapacityJobStorage, max_batch_job_count> max_batch_storage;
	for (size_t job_index = 0; job_index < max_batch_job_count; ++job_index)
	{
		max_batch_storage[job_index].output = std::make_unique<GuestReadableBlock>(max_output_storage_bytes);
		max_batch_storage[job_index].result = std::make_unique<GuestReadableBlock>(32);
		if (!max_batch_storage[job_index].output->IsValid() || !max_batch_storage[job_index].result->IsValid())
		{
			return 40;
		}
		std::memset(max_batch_storage[job_index].output->Data(), 0xa5, max_output_storage_bytes);
		std::memset(max_batch_storage[job_index].result->Data(), 0xa5, 32);
	}
	GuestReadableBlock max_batch_error_storage(sizeof(Ajm::AjmBatchError) + sizeof(uint32_t));
	GuestReadableBlock max_batch_id_storage(2 * sizeof(uint32_t));
	if (!max_batch_error_storage.IsValid() || !max_batch_id_storage.IsValid())
	{
		return 41;
	}
	auto* const max_batch_error = static_cast<Ajm::AjmBatchError*>(max_batch_error_storage.Data());
	auto* const max_batch_id    = static_cast<uint32_t*>(max_batch_id_storage.Data());

	alignas(uint64_t) uint8_t max_capacity_batch[512] {};
	for (size_t packet_index = 0; packet_index < max_batch_packets_per_decoder; ++packet_index)
	{
		for (size_t decoder_index = 0; decoder_index < batch_decoder_count; ++decoder_index)
		{
			const size_t job_index = packet_index * batch_decoder_count + decoder_index;
			input_buffers[0]        = {input_bytes + packet_index * framed_packet_bytes, framed_packet_bytes};
			for (size_t scatter_index = 0; scatter_index < max_scatter_count; ++scatter_index)
			{
				auto* const output = static_cast<uint8_t*>(max_batch_storage[job_index].output->Data()) +
				                     scatter_index * max_scatter_stride;
				output_buffers[scatter_index] = {output, max_scatter_bytes};
			}
			if (Ajm::AjmBatchJobDecodeSplit(max_capacity_batch, batch_instances[decoder_index], input_buffers, 1, output_buffers,
			                                max_scatter_count, max_batch_storage[job_index].result->Data()) != 0)
			{
				return 42;
			}
		}
	}
	input_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	for (size_t scatter_index = 0; scatter_index < max_scatter_count; ++scatter_index)
	{
		output_buffers[scatter_index] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	}

	std::memset(max_batch_error_storage.Data(), 0xa5, sizeof(Ajm::AjmBatchError) + sizeof(uint32_t));
	std::memset(max_batch_id_storage.Data(), 0xa5, 2 * sizeof(uint32_t));
	if (!max_batch_id_storage.Protect(Core::VirtualMemory::Mode::Read))
	{
		return 43;
	}
	const int32_t max_batch_id_status =
	    Ajm::AjmBatchStart(context, max_capacity_batch, 0, max_batch_error, max_batch_id);
	const bool max_batch_id_mapping_restored = max_batch_id_storage.Protect(Core::VirtualMemory::Mode::ReadWrite);
	if (!max_batch_id_mapping_restored)
	{
		return 44;
	}
	if (max_batch_id_status != invalid_parameter ||
	    !IsAjmBytePattern(max_batch_error_storage.Data(), sizeof(Ajm::AjmBatchError) + sizeof(uint32_t), 0xa5) ||
	    !IsAjmBytePattern(max_batch_id_storage.Data(), 2 * sizeof(uint32_t), 0xa5))
	{
		return 45;
	}
	for (size_t job_index = 0; job_index < max_batch_job_count; ++job_index)
	{
		if (!IsAjmBytePattern(max_batch_storage[job_index].output->Data(), max_output_storage_bytes, 0xa5) ||
		    !IsAjmBytePattern(max_batch_storage[job_index].result->Data(), 32, 0xa5))
		{
			return 46;
		}
	}
	if (std::memcmp(input_storage.Data(), framed_input.data(), framed_input.size()) != 0)
	{
		return 47;
	}

	// Retry the same queued batch after the late preflight failure; no jobs are enqueued again.
	auto start_and_wait_max_batch = [&]()
	{
		*max_batch_id       = 0;
		*max_batch_error    = {};
		if (Ajm::AjmBatchStart(context, max_capacity_batch, 0, max_batch_error, max_batch_id) != 0 || *max_batch_id == 0 ||
		    max_batch_error->error_code != 0)
		{
			return false;
		}
		return Ajm::AjmBatchWait(context, *max_batch_id, UINT32_MAX, max_batch_error) == 0 &&
		       max_batch_error->error_code == 0;
	};
	if (!start_and_wait_max_batch() ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(max_batch_error_storage.Data()) + sizeof(Ajm::AjmBatchError),
	                      sizeof(uint32_t), 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(max_batch_id_storage.Data()) + sizeof(uint32_t), sizeof(uint32_t), 0xa5) ||
	    std::memcmp(input_storage.Data(), framed_input.data(), framed_input.size()) != 0)
	{
		return 48;
	}
	for (size_t packet_index = 0; packet_index < max_batch_packets_per_decoder; ++packet_index)
	{
		for (size_t decoder_index = 0; decoder_index < batch_decoder_count; ++decoder_index)
		{
			const size_t job_index = packet_index * batch_decoder_count + decoder_index;
			if (!opus_decode_result_matches(max_batch_storage[job_index].result->Data(), framed_packet_bytes, packet_pcm_bytes,
			                                (packet_index + 1) * packet_pcm_bytes / pcm_frame_bytes))
			{
				return 49;
			}
			for (size_t scatter_index = 0; scatter_index < max_scatter_count; ++scatter_index)
			{
				const auto* const output = static_cast<const uint8_t*>(max_batch_storage[job_index].output->Data()) +
				                           scatter_index * max_scatter_stride;
				if (std::memcmp(output, expected_packet_pcm[packet_index].data() + scatter_index * max_scatter_bytes,
				                max_scatter_bytes) != 0 ||
				    !IsAjmBytePattern(output + max_scatter_bytes, sizeof(uint32_t), 0xa5))
				{
					return 50;
				}
			}
		}
	}

	// The guest owns the batch storage, so a batch of many small jobs is bounded only by the ranges it publishes.
	constexpr size_t small_job_count       = 5 * batch_decoder_count;
	constexpr size_t small_job_result_size = 32;
	GuestReadableBlock small_job_results(small_job_count * small_job_result_size);
	if (!small_job_results.IsValid())
	{
		return 51;
	}
	std::memset(small_job_results.Data(), 0xa5, small_job_count * small_job_result_size);
	alignas(uint64_t) uint8_t small_job_batch[512] {};
	for (size_t job_index = 0; job_index < small_job_count; ++job_index)
	{
		auto* const result = static_cast<uint8_t*>(small_job_results.Data()) + job_index * small_job_result_size;
		if (Ajm::AjmBatchJobClearContext(small_job_batch, batch_instances[job_index % batch_decoder_count], result) != 0)
		{
			return 52;
		}
	}
	if (!start_and_wait_queued_batch(small_job_batch))
	{
		return 53;
	}
	for (size_t job_index = 0; job_index < small_job_count; ++job_index)
	{
		const auto* const result = static_cast<const uint8_t*>(small_job_results.Data()) + job_index * small_job_result_size;
		if (!IsAjmBytePattern(result, 8, 0) || !IsAjmBytePattern(result + 8, small_job_result_size - 8, 0xa5))
		{
			return 54;
		}
	}

	// Initializing the control abandons the jobs queued on it; only jobs queued afterwards run.
	auto* const abandoned_result = static_cast<uint8_t*>(small_job_results.Data());
	auto* const started_result   = abandoned_result + small_job_result_size;
	std::memset(small_job_results.Data(), 0xa5, 2 * small_job_result_size);
	alignas(uint64_t) uint8_t reinitialized_storage[64] {};
	if (Ajm::AjmBatchJobClearContext(small_job_batch, batch_instances[0], abandoned_result) != 0 ||
	    Ajm::AjmBatchInitializeBuffer(reinitialized_storage, sizeof(reinitialized_storage), small_job_batch) != 0 ||
	    Ajm::AjmBatchJobClearContext(small_job_batch, batch_instances[0], started_result) != 0 ||
	    !start_and_wait_queued_batch(small_job_batch))
	{
		return 55;
	}
	if (!IsAjmBytePattern(abandoned_result, small_job_result_size, 0xa5) || !IsAjmBytePattern(started_result, 8, 0) ||
	    !IsAjmBytePattern(started_result + 8, small_job_result_size - 8, 0xa5))
	{
		return 56;
	}

	for (const uint32_t batch_instance: batch_instances)
	{
		if (Ajm::AjmInstanceDestroy(context, batch_instance) != 0)
		{
			return 38;
		}
	}
	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 15;
	}
	return 0;
}

int RunAjmOpusGaplessScenario(const std::array<uint8_t, 648>& framed_input, const std::vector<uint8_t>& expected_output,
                              uint32_t resample_flags, uint64_t gapless_reset)
{
	constexpr size_t output_capacity   = 9000;
	constexpr size_t output_split      = 4000;
	constexpr size_t output_bytes      = 2000 * 2 * sizeof(int16_t);
	constexpr size_t first_call_frames = 2 * 960 - 312 - 8;
	constexpr size_t first_call_bytes  = first_call_frames * 2 * sizeof(int16_t);
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock             config_storage(12);
	GuestReadableBlock             gapless_storage(8);
	GuestReadableBlock             input_storage(framed_input.size());
	GuestReadableBlock             input_descriptors_storage(2 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_descriptors_storage(2 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_storage(output_capacity);
	GuestReadableBlock             init_result_storage(32);
	GuestReadableBlock             clear_result_storage(32);
	GuestReadableBlock             gapless_result_storage(32);
	GuestReadableBlock             resample_result_storage(32);
	GuestReadableBlock             resample_info_storage(24);
	GuestReadableBlock             decode_result_storage(32);
	GuestReadableBlock             second_decode_result_storage(32);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	if (!config_storage.IsValid() || !gapless_storage.IsValid() || !input_storage.IsValid() || !input_descriptors_storage.IsValid() ||
	    !output_descriptors_storage.IsValid() || !output_storage.IsValid() || !init_result_storage.IsValid() ||
	    !clear_result_storage.IsValid() || !gapless_result_storage.IsValid() || !resample_result_storage.IsValid() ||
	    !resample_info_storage.IsValid() || !decode_result_storage.IsValid() || !second_decode_result_storage.IsValid() ||
	    !error_storage.IsValid() || !batch_id_storage.IsValid() || expected_output.size() != output_bytes)
	{
		return 2;
	}

	constexpr uint8_t opus_config[12]   = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	constexpr uint8_t gapless_config[8] = {0xd0, 0x07, 0, 0,
	                                       0x38, 0x01, 0, 0}; // 2000 output frames, 312 priming frames, 0 already skipped.
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	std::memcpy(input_storage.Data(), framed_input.data(), framed_input.size());
	std::memset(init_result_storage.Data(), 0xa5, 32);
	std::memset(clear_result_storage.Data(), 0xa5, 32);
	std::memset(gapless_result_storage.Data(), 0xa5, 32);
	std::memset(resample_result_storage.Data(), 0xa5, 32);
	std::memset(resample_info_storage.Data(), 0xa5, 24);
	std::memset(decode_result_storage.Data(), 0xa5, 32);
	std::memset(second_decode_result_storage.Data(), 0xa5, 32);
	std::memset(output_storage.Data(), 0xa5, output_capacity);

	uint32_t context = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0)
	{
		return 3;
	}
	uint32_t instance = 0;
	if (Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 || instance != ((24u << 14u) | 1u))
	{
		return 4;
	}

	auto* const input_buffers    = static_cast<Ajm::AjmBuffer*>(input_descriptors_storage.Data());
	auto* const output_buffers   = static_cast<Ajm::AjmBuffer*>(output_descriptors_storage.Data());
	auto* const input_bytes      = static_cast<uint8_t*>(input_storage.Data());
	auto* const output_bytes_ptr = static_cast<uint8_t*>(output_storage.Data());
	input_buffers[0]             = {input_bytes, 161};
	input_buffers[1]             = {input_bytes + 161, 163};
	output_buffers[0]            = {output_bytes_ptr, output_split};
	output_buffers[1]            = {output_bytes_ptr + output_split, first_call_bytes - output_split};
	alignas(uint64_t) uint8_t batch[256] {};
	if (Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), 12, init_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobClearContext(batch, instance, clear_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_storage.Data(), gapless_reset, gapless_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, resample_flags, resample_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobGetResampleInfo(batch, instance, resample_info_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 2, output_buffers, 2, decode_result_storage.Data()) != 0)
	{
		return 5;
	}

	// Config and descriptor metadata are captured by the enqueue calls.
	std::memset(config_storage.Data(), 0xa5, 12);
	std::memset(gapless_storage.Data(), 0xa5, 8);
	input_buffers[0]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	input_buffers[1]  = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[1] = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};

	*batch_id_storage.Data() = 0;
	*error_storage.Data()    = {};
	if (Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 || *batch_id_storage.Data() == 0 ||
	    error_storage.Data()->error_code != 0 ||
	    Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) != 0)
	{
		return 6;
	}
	auto read_i32 = [](const uint8_t* result, size_t offset)
	{
		int32_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	uint64_t samples = 0;
	std::memcpy(&samples, static_cast<const uint8_t*>(decode_result_storage.Data()) + 16, sizeof(samples));
	float resample_ratio = 0.0f;
	std::memcpy(&resample_ratio, static_cast<const uint8_t*>(resample_info_storage.Data()) + 8, sizeof(resample_ratio));
	if (!IsAjmBytePattern(init_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(init_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(clear_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(clear_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(gapless_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(gapless_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(resample_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(resample_result_storage.Data()) + 8, 24, 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(resample_info_storage.Data()) + 16, 8, 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(decode_result_storage.Data()) + 24, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(resample_info_storage.Data()), 0) != 0 || resample_ratio != 1.0f ||
	    read_i32(static_cast<const uint8_t*>(resample_info_storage.Data()), 12) != 0 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 4) != 0 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 8) != 324 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 12) != static_cast<int32_t>(first_call_bytes) ||
	    samples != first_call_frames || std::memcmp(output_storage.Data(), expected_output.data(), first_call_bytes) != 0 ||
	    !IsAjmBytePattern(output_bytes_ptr + first_call_bytes, output_capacity - first_call_bytes, 0xa5))
	{
		return 7;
	}

	input_buffers[0]  = {input_bytes + 324, 161};
	input_buffers[1]  = {input_bytes + 485, 163};
	output_buffers[0] = {output_bytes_ptr, output_split};
	output_buffers[1] = {output_bytes_ptr + output_split, output_capacity - output_split};
	std::memset(output_storage.Data(), 0xa5, output_capacity);
	if (Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 2, output_buffers, 2, second_decode_result_storage.Data()) != 0)
	{
		return 8;
	}
	input_buffers[0]         = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	input_buffers[1]         = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[0]        = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	output_buffers[1]        = {reinterpret_cast<void*>(static_cast<uintptr_t>(1)), SIZE_MAX};
	*batch_id_storage.Data() = 0;
	*error_storage.Data()    = {};
	if (Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 || *batch_id_storage.Data() == 0 ||
	    error_storage.Data()->error_code != 0 ||
	    Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) != 0)
	{
		return 9;
	}
	uint64_t second_samples = 0;
	std::memcpy(&second_samples, static_cast<const uint8_t*>(second_decode_result_storage.Data()) + 16, sizeof(second_samples));
	const size_t  second_call_bytes = output_bytes - first_call_bytes;
	const int32_t second_consumed   = read_i32(static_cast<const uint8_t*>(second_decode_result_storage.Data()), 8);
	if (!IsAjmBytePattern(static_cast<const uint8_t*>(second_decode_result_storage.Data()) + 24, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(second_decode_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(second_decode_result_storage.Data()), 4) != 0 || second_consumed != 162 ||
	    read_i32(static_cast<const uint8_t*>(second_decode_result_storage.Data()), 12) != static_cast<int32_t>(second_call_bytes) ||
	    second_samples != 2000 || std::memcmp(output_storage.Data(), expected_output.data() + first_call_bytes, second_call_bytes) != 0 ||
	    !IsAjmBytePattern(output_bytes_ptr + second_call_bytes, output_capacity - second_call_bytes, 0xa5))
	{
		return 10;
	}

	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 11;
	}
	return 0;
}

// Splits one decoded packet across two batches: the second batch repeats the neutral profile with flags 2 before draining
// the pending PCM, as observed at the end of finite gapless streams.
int RunAjmOpusNeutralFlag2Scenario(const std::array<uint8_t, 162>& framed_input, const std::vector<uint8_t>& expected_output)
{
	constexpr size_t frame_bytes        = 2 * sizeof(int16_t);
	constexpr size_t first_output_bytes = 240 * frame_bytes;
	constexpr size_t drain_output_bytes = 720 * frame_bytes;
	if (expected_output.size() != first_output_bytes + drain_output_bytes)
	{
		return 2;
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock             config_storage(12);
	GuestReadableBlock             input_storage(framed_input.size());
	GuestReadableBlock             input_descriptors_storage(sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_descriptors_storage(2 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_storage(expected_output.size() + 64);
	GuestReadableBlock             init_result_storage(16);
	GuestReadableBlock             resample_result_storage(32);
	GuestReadableBlock             decode_result_storage(64);
	GuestReadableBlock             info_result_storage(24);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	if (!config_storage.IsValid() || !input_storage.IsValid() || !input_descriptors_storage.IsValid() ||
	    !output_descriptors_storage.IsValid() || !output_storage.IsValid() || !init_result_storage.IsValid() ||
	    !resample_result_storage.IsValid() || !decode_result_storage.IsValid() || !info_result_storage.IsValid() ||
	    !error_storage.IsValid() || !batch_id_storage.IsValid())
	{
		return 3;
	}

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(input_storage.Data(), framed_input.data(), framed_input.size());
	std::memset(output_storage.Data(), 0xa5, expected_output.size() + 64);
	std::memset(resample_result_storage.Data(), 0xa5, 32);
	std::memset(decode_result_storage.Data(), 0xa5, 64);
	std::memset(info_result_storage.Data(), 0xa5, 24);

	uint32_t context  = 0;
	uint32_t instance = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
	    Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0)
	{
		return 4;
	}

	auto* const output         = static_cast<uint8_t*>(output_storage.Data());
	auto* const decode_results = static_cast<uint8_t*>(decode_result_storage.Data());
	auto* const input_buffers  = static_cast<Ajm::AjmBuffer*>(input_descriptors_storage.Data());
	auto* const output_buffers = static_cast<Ajm::AjmBuffer*>(output_descriptors_storage.Data());
	input_buffers[0]           = {input_storage.Data(), framed_input.size()};
	output_buffers[0]          = {output, first_output_bytes};
	output_buffers[1]          = {output + first_output_bytes, drain_output_bytes};
	const auto run_batch       = [&](const auto& queue_jobs)
	{
		alignas(uint64_t) uint8_t batch[512] {};
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		return queue_jobs(batch) && Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) == 0 &&
		       *batch_id_storage.Data() != 0 && error_storage.Data()->error_code == 0 &&
		       Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) == 0;
	};
	if (!run_batch(
	        [&](uint8_t* batch)
	        {
		        return Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), sizeof(opus_config), init_result_storage.Data()) ==
		                   0 &&
		               Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 0, resample_result_storage.Data()) == 0 &&
		               Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, decode_results) == 0;
	        }))
	{
		return 5;
	}
	if (!run_batch(
	        [&](uint8_t* batch)
	        {
		        auto* const marker_results = static_cast<uint8_t*>(resample_result_storage.Data()) + 16;
		        return Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 2, marker_results) == 0 &&
		               Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers + 1, 1, decode_results + 32) == 0 &&
		               Ajm::AjmBatchJobGetResampleInfo(batch, instance, info_result_storage.Data()) == 0;
	        }))
	{
		return 6;
	}

	auto read_i32 = [](const uint8_t* result, size_t offset)
	{
		int32_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	auto read_u64 = [](const uint8_t* result, size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	const auto* const resample_results = static_cast<const uint8_t*>(resample_result_storage.Data());
	const auto* const info             = static_cast<const uint8_t*>(info_result_storage.Data());
	float             info_ratio       = 0.0f;
	std::memcpy(&info_ratio, info + 8, sizeof(info_ratio));
	if (!IsAjmBytePattern(resample_results, 8, 0) || !IsAjmBytePattern(resample_results + 16, 8, 0) ||
	    !IsAjmBytePattern(resample_results + 24, 8, 0xa5) || read_i32(decode_results, 0) != 0 || read_i32(decode_results, 4) != 0 ||
	    read_i32(decode_results, 8) != static_cast<int32_t>(framed_input.size()) ||
	    read_i32(decode_results, 12) != static_cast<int32_t>(first_output_bytes) || read_u64(decode_results, 16) != 240 ||
	    read_i32(decode_results, 32) != 0 || read_i32(decode_results, 36) != 0 || read_i32(decode_results, 40) != 0 ||
	    read_i32(decode_results, 44) != static_cast<int32_t>(drain_output_bytes) || read_u64(decode_results, 48) != 960 ||
	    !IsAjmBytePattern(decode_results + 56, 8, 0xa5) || std::memcmp(output, expected_output.data(), expected_output.size()) != 0 ||
	    !IsAjmBytePattern(output + expected_output.size(), 64, 0xa5) || read_i32(info, 0) != 0 || read_i32(info, 4) != 0 ||
	    info_ratio != 1.0f || read_i32(info, 12) != 0 || !IsAjmBytePattern(info + 16, 8, 0xa5))
	{
		return 7;
	}

	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 8;
	}
	return 0;
}

struct AjmResampleCall
{
	float    ratio         = 1.0f;
	uint32_t output_frames = 0;
	uint32_t flags         = 0;
	// Restarts the stream like a guest reusing its instance: clear and gapless again, then decode with no new setter.
	bool  restart = false;
	float change  = 0.0f;
	// The guest skips the setter while its ratio and change per sample are unchanged; the running profile carries on.
	bool send_setter = true;
};

struct AjmResampleCallResult
{
	int32_t  input_consumed  = 0;
	int32_t  output_bytes    = 0;
	uint64_t admitted_frames = 0;
	int32_t  held_frames     = 0;
	float    info_ratio      = 0.0f;
};

constexpr size_t AJM_OPUS_FIXTURE_RECORD_SIZE = sizeof(uint16_t) + AudioTestFixtures::kGeneratedOpusSinePacket.size();

// Drives one finite stereo Opus stream through resampled split decodes, one batch per call. Like the guest, each batch
// repeats the resample setter and resubmits the input from the first unconsumed byte; once everything is consumed, the
// whole input is offered again.
int RunAjmOpusResampleStream(size_t record_count, uint32_t total_frames, uint16_t priming_frames, const std::vector<AjmResampleCall>& calls,
                             std::vector<uint8_t>* pcm, std::vector<AjmResampleCallResult>* results)
{
	constexpr size_t frame_bytes = 2 * sizeof(int16_t);
	uint32_t         max_frames  = 0;
	for (const auto& call: calls)
	{
		max_frames = std::max(max_frames, call.output_frames);
	}
	if (pcm == nullptr || results == nullptr || record_count == 0 || calls.empty() || max_frames == 0)
	{
		return 2;
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const size_t                   input_size = record_count * AJM_OPUS_FIXTURE_RECORD_SIZE;
	GuestReadableBlock             config_storage(12);
	GuestReadableBlock             gapless_storage(8);
	GuestReadableBlock             input_storage(input_size);
	GuestReadableBlock             descriptors_storage(2 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_storage(static_cast<size_t>(max_frames) * frame_bytes + 64);
	GuestReadableBlock             control_results_storage(64);
	GuestReadableBlock             decode_result_storage(32);
	GuestReadableBlock             info_result_storage(24);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	if (!config_storage.IsValid() || !gapless_storage.IsValid() || !input_storage.IsValid() || !descriptors_storage.IsValid() ||
	    !output_storage.IsValid() || !control_results_storage.IsValid() || !decode_result_storage.IsValid() ||
	    !info_result_storage.IsValid() || !error_storage.IsValid() || !batch_id_storage.IsValid())
	{
		return 3;
	}

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	const uint8_t     gapless_config[8] = {static_cast<uint8_t>(total_frames),        static_cast<uint8_t>(total_frames >> 8u),
	                                       static_cast<uint8_t>(total_frames >> 16u), static_cast<uint8_t>(total_frames >> 24u),
	                                       static_cast<uint8_t>(priming_frames),      static_cast<uint8_t>(priming_frames >> 8u),
	                                       0,
	                                       0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	auto* const input = static_cast<uint8_t*>(input_storage.Data());
	for (size_t record = 0; record < record_count; ++record)
	{
		uint8_t* const framed = input + record * AJM_OPUS_FIXTURE_RECORD_SIZE;
		framed[0]             = static_cast<uint8_t>(AudioTestFixtures::kGeneratedOpusSinePacket.size());
		framed[1]             = static_cast<uint8_t>(AudioTestFixtures::kGeneratedOpusSinePacket.size() >> 8u);
		std::memcpy(framed + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(),
		            AudioTestFixtures::kGeneratedOpusSinePacket.size());
	}

	uint32_t context  = 0;
	uint32_t instance = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
	    Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0)
	{
		return 4;
	}

	auto* const output          = static_cast<uint8_t*>(output_storage.Data());
	auto* const control_results = static_cast<uint8_t*>(control_results_storage.Data());
	auto* const descriptors     = static_cast<Ajm::AjmBuffer*>(descriptors_storage.Data());
	const auto* decode_result   = static_cast<const uint8_t*>(decode_result_storage.Data());
	const auto* info            = static_cast<const uint8_t*>(info_result_storage.Data());
	size_t      consumed        = 0;
	pcm->clear();
	results->clear();
	for (size_t call_index = 0; call_index < calls.size(); ++call_index)
	{
		const AjmResampleCall& call         = calls[call_index];
		consumed                            = call.restart ? 0 : consumed;
		const size_t           input_offset = consumed == input_size ? 0 : consumed;
		const size_t           output_bytes = static_cast<size_t>(call.output_frames) * frame_bytes;
		descriptors[0]                      = {input + input_offset, input_size - input_offset};
		descriptors[1]                      = {output, output_bytes};
		std::memset(output, 0xa5, output_bytes + 64);
		std::memset(decode_result_storage.Data(), 0xa5, 32);
		std::memset(info_result_storage.Data(), 0xa5, 24);
		alignas(uint64_t) uint8_t batch[512] {};
		if (call_index == 0 &&
		    (Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), sizeof(opus_config), control_results) != 0 ||
		     Ajm::AjmBatchJobClearContext(batch, instance, control_results + 16) != 0 ||
		     Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_storage.Data(), 0, control_results + 32) != 0))
		{
			return 5;
		}
		if (call.restart && (Ajm::AjmBatchJobClearContext(batch, instance, control_results + 16) != 0 ||
		                     Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_storage.Data(), 0, control_results + 32) != 0))
		{
			return 5;
		}
		if ((!call.restart && call.send_setter &&
		     Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, call.ratio, call.change, call.flags, control_results + 48) != 0) ||
		    Ajm::AjmBatchJobDecodeSplit(batch, instance, descriptors, 1, descriptors + 1, 1, decode_result_storage.Data()) != 0 ||
		    Ajm::AjmBatchJobGetResampleInfo(batch, instance, info_result_storage.Data()) != 0)
		{
			return 6;
		}
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		if (Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 || *batch_id_storage.Data() == 0 ||
		    error_storage.Data()->error_code != 0 ||
		    Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) != 0)
		{
			return 7;
		}

		AjmResampleCallResult result {};
		int32_t               status = -1;
		std::memcpy(&status, decode_result, sizeof(status));
		std::memcpy(&result.input_consumed, decode_result + 8, sizeof(result.input_consumed));
		std::memcpy(&result.output_bytes, decode_result + 12, sizeof(result.output_bytes));
		std::memcpy(&result.admitted_frames, decode_result + 16, sizeof(result.admitted_frames));
		std::memcpy(&result.info_ratio, info + 8, sizeof(result.info_ratio));
		std::memcpy(&result.held_frames, info + 12, sizeof(result.held_frames));
		if (status != 0 || result.input_consumed < 0 || static_cast<size_t>(result.input_consumed) > input_size - input_offset ||
		    result.output_bytes < 0 || static_cast<size_t>(result.output_bytes) > output_bytes ||
		    !IsAjmBytePattern(output + result.output_bytes, output_bytes + 64 - static_cast<size_t>(result.output_bytes), 0xa5) ||
		    !IsAjmBytePattern(decode_result + 24, 8, 0xa5) || !IsAjmBytePattern(info, 8, 0) || !IsAjmBytePattern(info + 16, 8, 0xa5))
		{
			return 8;
		}
		consumed = input_offset + static_cast<size_t>(result.input_consumed);
		pcm->insert(pcm->end(), output, output + result.output_bytes);
		results->push_back(result);
	}

	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 9;
	}
	return 0;
}

// Checks the guest-visible source accounting of a finished finite stream: the admitted counter A never exceeds the
// source total, at most two interpolation frames are held, and A minus held lands on the total exactly.
bool AjmResampleStreamEndsOnSourceTotal(const std::vector<AjmResampleCallResult>& results, uint32_t total_frames, float final_ratio)
{
	if (results.empty())
	{
		return false;
	}
	for (const auto& result: results)
	{
		if (result.admitted_frames > total_frames || result.held_frames < 0 || result.held_frames > 2 ||
		    static_cast<uint64_t>(result.held_frames) > result.admitted_frames)
		{
			return false;
		}
	}
	const auto& last = results.back();
	return last.admitted_frames == total_frames && last.held_frames == 0 && last.info_ratio == final_ratio;
}

// A cleared ratio-1 instance restarts its stream with no new setter: the output and counters match a fresh stream and
// GetResampleInfo still reports the ratio-1 profile.
int RunAjmOpusClearedNeutralRestartScenario()
{
	constexpr uint32_t total_frames = 1500;
	constexpr uint16_t priming      = 312;

	std::vector<uint8_t>               fresh_pcm;
	std::vector<AjmResampleCallResult> fresh_results;
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, {{1.0f, 4096}, {1.0f, 512}}, &fresh_pcm, &fresh_results);
	    status != 0)
	{
		return status;
	}
	std::vector<uint8_t>               restarted_pcm;
	std::vector<AjmResampleCallResult> restarted_results;
	const std::vector<AjmResampleCall> calls = {{1.0f, 600}, {1.0f, 4096, 0, true}, {1.0f, 512}};
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, calls, &restarted_pcm, &restarted_results); status != 0)
	{
		return 20 + status;
	}
	constexpr size_t first_bytes = 600 * 2 * sizeof(int16_t);
	if (restarted_pcm.size() != first_bytes + fresh_pcm.size() ||
	    !std::equal(fresh_pcm.begin(), fresh_pcm.end(), restarted_pcm.begin() + first_bytes) ||
	    restarted_results[1].admitted_frames != total_frames || restarted_results[1].held_frames != 0 ||
	    restarted_results[1].info_ratio != 1.0f || restarted_results[1].input_consumed != fresh_results[0].input_consumed ||
	    !AjmResampleStreamEndsOnSourceTotal(restarted_results, total_frames, 1.0f))
	{
		return 40;
	}
	return 0;
}

// A stream started with the reset marker ends like any other: the flags 2 marker on its last calls keeps its state.
int RunAjmOpusResetMarkerStreamEndScenario()
{
	constexpr uint32_t total_frames = 1500;
	constexpr uint16_t priming      = 312;

	std::vector<uint8_t>               fresh_pcm;
	std::vector<AjmResampleCallResult> fresh_results;
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, {{1.0f, 4096}, {1.0f, 512}}, &fresh_pcm, &fresh_results);
	    status != 0)
	{
		return status;
	}
	std::vector<uint8_t>               marked_pcm;
	std::vector<AjmResampleCallResult> marked_results;
	const std::vector<AjmResampleCall> calls = {{1.0f, 600, 1}, {1.0f, 4096, 2}, {1.0f, 512, 2}};
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, calls, &marked_pcm, &marked_results); status != 0)
	{
		return 20 + status;
	}
	if (marked_pcm != fresh_pcm || !AjmResampleStreamEndsOnSourceTotal(marked_results, total_frames, 1.0f))
	{
		return 40;
	}
	return 0;
}

int RunAjmOpusResampleSplitInvarianceScenario()
{
	constexpr float    ratio        = 0x1.df108cp-1f;
	constexpr uint32_t total_frames = 1500;
	constexpr uint16_t priming      = 312;

	std::vector<uint8_t>               whole_pcm;
	std::vector<AjmResampleCallResult> whole_results;
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, {{ratio, 4096}, {ratio, 512}}, &whole_pcm, &whole_results);
	    status != 0)
	{
		return status;
	}
	// Near the end of a finite stream the guest repeats the running ratio with flags 2; the stream state carries over.
	std::vector<AjmResampleCall> split_calls;
	for (const uint32_t frames: {97u, 1u, 250u, 513u, 2u, 333u, 640u, 64u})
	{
		split_calls.push_back({ratio, frames, split_calls.size() < 5 ? 0u : 2u});
	}
	std::vector<uint8_t>               split_pcm;
	std::vector<AjmResampleCallResult> split_results;
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, split_calls, &split_pcm, &split_results); status != 0)
	{
		return 20 + status;
	}

	// The source total maps to ceil(total / ratio) output frames; the last ones interpolate past the final source frame.
	const auto expected_frames = static_cast<size_t>(std::ceil(static_cast<double>(total_frames) / static_cast<double>(ratio)));
	int64_t    split_consumed  = 0;
	for (const auto& result: split_results)
	{
		split_consumed += result.input_consumed;
	}
	const auto& whole_end = whole_results.back();
	const auto& split_end = split_results.back();
	if (whole_pcm.size() != expected_frames * 4 || split_pcm != whole_pcm ||
	    !AjmResampleStreamEndsOnSourceTotal(whole_results, total_frames, ratio) ||
	    !AjmResampleStreamEndsOnSourceTotal(split_results, total_frames, ratio) ||
	    whole_results[0].input_consumed != static_cast<int32_t>(2 * AJM_OPUS_FIXTURE_RECORD_SIZE) ||
	    split_consumed != static_cast<int64_t>(2 * AJM_OPUS_FIXTURE_RECORD_SIZE) || whole_end.input_consumed != 0 ||
	    whole_end.output_bytes != 0 || split_end.input_consumed != 0 || split_end.output_bytes != 0)
	{
		return 40;
	}
	return 0;
}

int RunAjmOpusResampleValueScenario(const std::vector<AjmResampleCall>& calls, uint32_t total_frames, uint16_t priming,
                                    const std::vector<uint8_t>& expected_pcm, const std::vector<AjmResampleCallResult>& expected_results)
{
	std::vector<uint8_t>               pcm;
	std::vector<AjmResampleCallResult> results;
	if (const int status = RunAjmOpusResampleStream(2, total_frames, priming, calls, &pcm, &results); status != 0)
	{
		return status;
	}
	if (pcm != expected_pcm || results.size() != expected_results.size())
	{
		return 40;
	}
	for (size_t index = 0; index < results.size(); ++index)
	{
		const auto& result   = results[index];
		const auto& expected = expected_results[index];
		if (result.input_consumed != expected.input_consumed || result.output_bytes != expected.output_bytes ||
		    result.admitted_frames != expected.admitted_frames || result.held_frames != expected.held_frames ||
		    result.info_ratio != expected.info_ratio)
		{
			return 41 + static_cast<int>(index);
		}
	}
	return 0;
}

int RunAjmOpusResampleInfoScenario(const std::array<uint8_t, 162>& framed_input, const std::vector<uint8_t>& expected_output)
{
	constexpr size_t output_bytes = 960 * 2 * sizeof(int16_t);
	if (expected_output.size() != output_bytes)
	{
		return 2;
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock             config_storage(12);
	GuestReadableBlock             input_storage(framed_input.size());
	GuestReadableBlock             input_descriptors_storage(sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_descriptors_storage(sizeof(Ajm::AjmBuffer));
	GuestReadableBlock             output_storage(output_bytes + 64);
	GuestReadableBlock             init_result_storage(16);
	GuestReadableBlock             resample_result_storage(16);
	GuestReadableBlock             decode_result_storage(32);
	GuestReadableBlock             info_result_storage(24);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	if (!config_storage.IsValid() || !input_storage.IsValid() || !input_descriptors_storage.IsValid() ||
	    !output_descriptors_storage.IsValid() || !output_storage.IsValid() || !init_result_storage.IsValid() ||
	    !resample_result_storage.IsValid() || !decode_result_storage.IsValid() || !info_result_storage.IsValid() ||
	    !error_storage.IsValid() || !batch_id_storage.IsValid())
	{
		return 3;
	}

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(input_storage.Data(), framed_input.data(), framed_input.size());
	std::memset(output_storage.Data(), 0xa5, output_bytes + 64);
	std::memset(init_result_storage.Data(), 0xa5, 16);
	std::memset(resample_result_storage.Data(), 0xa5, 16);
	std::memset(decode_result_storage.Data(), 0xa5, 32);
	std::memset(info_result_storage.Data(), 0xa5, 24);

	uint32_t context  = 0;
	uint32_t instance = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
	    Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0)
	{
		return 4;
	}

	auto* const input_buffers  = static_cast<Ajm::AjmBuffer*>(input_descriptors_storage.Data());
	auto* const output_buffers = static_cast<Ajm::AjmBuffer*>(output_descriptors_storage.Data());
	input_buffers[0]           = {input_storage.Data(), framed_input.size()};
	output_buffers[0]          = {output_storage.Data(), output_bytes};
	alignas(uint64_t) uint8_t batch[256] {};
	if (Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), sizeof(opus_config), init_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 0, resample_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, decode_result_storage.Data()) != 0 ||
	    Ajm::AjmBatchJobGetResampleInfo(batch, instance, info_result_storage.Data()) != 0)
	{
		return 5;
	}

	*batch_id_storage.Data() = 0;
	*error_storage.Data()    = {};
	if (Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) != 0 || *batch_id_storage.Data() == 0 ||
	    error_storage.Data()->error_code != 0 ||
	    Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) != 0)
	{
		return 6;
	}

	auto read_i32 = [](const uint8_t* result, size_t offset)
	{
		int32_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	auto read_u64 = [](const uint8_t* result, size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, result + offset, sizeof(value));
		return value;
	};
	float info_ratio = 0.0f;
	std::memcpy(&info_ratio, static_cast<const uint8_t*>(info_result_storage.Data()) + 8, sizeof(info_ratio));
	if (!IsAjmBytePattern(init_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(init_result_storage.Data()) + 8, 8, 0xa5) ||
	    !IsAjmBytePattern(resample_result_storage.Data(), 8, 0) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(resample_result_storage.Data()) + 8, 8, 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(decode_result_storage.Data()) + 24, 8, 0xa5) ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(info_result_storage.Data()) + 16, 8, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 4) != 0 ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 8) != static_cast<int32_t>(framed_input.size()) ||
	    read_i32(static_cast<const uint8_t*>(decode_result_storage.Data()), 12) != static_cast<int32_t>(output_bytes) ||
	    read_u64(static_cast<const uint8_t*>(decode_result_storage.Data()), 16) != 960 ||
	    std::memcmp(output_storage.Data(), expected_output.data(), output_bytes) != 0 ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(output_storage.Data()) + output_bytes, 64, 0xa5) ||
	    read_i32(static_cast<const uint8_t*>(info_result_storage.Data()), 0) != 0 ||
	    read_i32(static_cast<const uint8_t*>(info_result_storage.Data()), 4) != 0 || info_ratio != 1.0f ||
	    read_i32(static_cast<const uint8_t*>(info_result_storage.Data()), 12) != 0)
	{
		return 7;
	}

	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 8;
	}
	return 0;
}
#endif

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

	// The grain at word 4 is the only field the context consumes; the host mixer handles 64..4096 frames.
	for (const uint32_t grain: {0u, 63u, 4097u})
	{
		auto*          words = static_cast<uint32_t*>(parameter_storage.Data());
		const uint32_t saved = words[4];
		words[4]             = grain;
		*memory_size_storage.Data() = UINT64_MAX;
		*context_storage.Data()     = INT32_MAX;
		EXPECT_EQ(AudioOut2::AudioOut2ContextQueryMemory(words, memory_size_storage.Data()), Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
		EXPECT_EQ(*memory_size_storage.Data(), UINT64_MAX);
		EXPECT_EQ(AudioOut2::AudioOut2ContextCreate(words, workspace.Data(), 0x10000, context_storage.Data()),
		          Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL);
		EXPECT_EQ(*context_storage.Data(), INT32_MAX);
		words[4] = saved;
	}
}

TEST(EmulatorAudio, AudioOut2ContextLifecycleAcceptsADeeperQueueProfile)
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
	// The leading fields are captured from a guest: 16 ports, 128 object ports, a queue depth of 4 and a 512-frame grain.
	// The reserved tail is nonzero because titles leave it uninitialized after ResetParam.
	const uint64_t observed_configuration[8] = {0x0000008000000010ull, 0x0000000400000000ull, 0x0000000100000200ull, 0xa5a5a5a5a5a5a5a5ull};
	std::memcpy(parameter_storage.Data(), observed_configuration, sizeof(observed_configuration));
	ASSERT_EQ(AudioOut2::AudioOut2ContextQueryMemory(parameter_storage.Data(), memory_size_storage.Data()), 0);
	GuestReadableBlock workspace(*memory_size_storage.Data());
	ASSERT_TRUE(workspace.IsValid());
	ASSERT_EQ(
	    AudioOut2::AudioOut2ContextCreate(parameter_storage.Data(), workspace.Data(), *memory_size_storage.Data(), context_storage.Data()),
	    0);
	EXPECT_GT(*context_storage.Data(), 0);
	EXPECT_EQ(AudioOut2::AudioOut2ContextDestroy(*context_storage.Data()), 0);
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

TEST(EmulatorAudio, ElementaryVideoDecoderEnforcesConfiguredBoundsForCroppedAvcAndHevc)
{
	if (!::Kyty::Emulator::AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP();
	}

	// Generated constant-color Annex B fixtures. Both streams include coded-picture padding cropped by the decoder.
	static constexpr uint8_t avc_fixture[] = {
	    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdc, 0x97, 0xe2, 0x30, 0x11, 0x00, 0x00, 0x03,
	    0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x02, 0x8f, 0x12, 0x27, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68,
	    0xce, 0x0f, 0xc8, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x11, 0x8a, 0x00, 0x02, 0x19,
	    0x71, 0xc0, 0x00, 0x40, 0x32, 0x38, 0x00, 0x08, 0x03, 0xc9, 0xd7, 0x5e,
	};
	static constexpr uint8_t hevc_fixture[] = {
	    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00,
	    0x9f, 0xa8, 0x00, 0x00, 0x03, 0x00, 0x00, 0x1e, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42,
	    0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9f, 0xa8, 0x00, 0x00, 0x03, 0x00, 0x00, 0x1e,
	    0xa0, 0x20, 0x83, 0x1d, 0x56, 0x5b, 0xab, 0x93, 0x2b, 0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00,
	    0x02, 0x00, 0x00, 0x03, 0x00, 0x02, 0x10, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc0,
	    0x89, 0x00, 0x00, 0x00, 0x01, 0x28, 0x01, 0xaf, 0x78, 0xf7, 0x04, 0x03, 0xff, 0xdc, 0xd4, 0xd0,
	    0x58, 0xfe, 0x7e, 0xdd, 0xc4, 0x40, 0x83, 0xc0, 0x00, 0x78,
	};
	struct Fixture
	{
		::Kyty::Emulator::AudioVideoBackend::VideoCodec codec;
		const uint8_t*                                  data;
		size_t                                          size;
		int32_t                                         width;
		int32_t                                         height;
	};
	const Fixture fixtures[] = {
	    {::Kyty::Emulator::AudioVideoBackend::VideoCodec::Avc, avc_fixture, sizeof(avc_fixture), 32, 18},
	    {::Kyty::Emulator::AudioVideoBackend::VideoCodec::Hevc, hevc_fixture, sizeof(hevc_fixture), 62, 46},
	};

	for (const auto& fixture: fixtures)
	{
		std::string error;
		auto decoder = ::Kyty::Emulator::AudioVideoBackend::ElementaryVideoDecoder::Open(
		    fixture.codec, fixture.width, fixture.height, &error);
		ASSERT_NE(decoder, nullptr) << error;
		ASSERT_TRUE(decoder->Send(fixture.data, fixture.size, 23)) << decoder->LastError();
		ASSERT_TRUE(decoder->Drain()) << decoder->LastError();

		::Kyty::Emulator::AudioVideoBackend::VideoFrame frame;
		int64_t tag = 0;
		ASSERT_TRUE(decoder->Receive(&frame, &tag));
		EXPECT_EQ(tag, 23);
		EXPECT_EQ(frame.width, static_cast<uint32_t>(fixture.width));
		EXPECT_EQ(frame.height, static_cast<uint32_t>(fixture.height));
		EXPECT_EQ(frame.data.size(), static_cast<size_t>(fixture.width) * fixture.height * 3u / 2u);
		EXPECT_FALSE(decoder->Receive(&frame, &tag));
	}

	for (const auto& fixture: fixtures)
	{
		std::string error;
		auto decoder = ::Kyty::Emulator::AudioVideoBackend::ElementaryVideoDecoder::Open(
		    fixture.codec, fixture.width - 1, fixture.height - 1, &error);
		ASSERT_NE(decoder, nullptr) << error;
		const bool sent   = decoder->Send(fixture.data, fixture.size, 29);
		const bool drained = decoder->Drain();
		EXPECT_FALSE(sent && drained);

		::Kyty::Emulator::AudioVideoBackend::VideoFrame frame;
		int64_t tag = 0;
		EXPECT_FALSE(decoder->Receive(&frame, &tag));
		EXPECT_TRUE(frame.data.empty());
	}
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

TEST(EmulatorAudio, RejectsQueuedAjmDataOperationsBeforeCommittingResults)
{
#if !defined(__linux__)
	GTEST_SKIP() << "bounded shared-memory refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t context = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);

	alignas(uint64_t) uint8_t batch[64] {};
	void* const               batch_address = batch;
	const uint8_t             input[]       = {0x41, 0x4a, 0x4d, 0x01};
	GuestReadableBlock        config_storage(8);
	ASSERT_TRUE(config_storage.IsValid());
	auto* const   config          = static_cast<uint8_t*>(config_storage.Data());
	const uint8_t config_bytes[8] = {0xfe, 0x70, 0x1f, 0xf0, 0, 0, 0, 0};
	std::memcpy(config, config_bytes, sizeof(config_bytes));
	const AjmDataTestOperation operations[] = {AjmDataTestOperation::Decode,      AjmDataTestOperation::DecodeSingle,
	                                           AjmDataTestOperation::DecodeSplit, AjmDataTestOperation::Encode,
	                                           AjmDataTestOperation::Run,         AjmDataTestOperation::RunSplit};
	for (const auto operation: operations)
	{
		uint32_t instance = 0;
		ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);
		SharedAjmBatchResults shared_results;
		ASSERT_TRUE(shared_results.IsValid());
		auto* const shared = shared_results.Data();
		if (operation == AjmDataTestOperation::Run || operation == AjmDataTestOperation::RunSplit)
		{
			ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch_address, instance, config, 8, shared->decode_result), 0);
			ASSERT_EQ(Ajm::AjmBatchJobDecode(batch_address, instance, input, sizeof(input), shared->data_output,
			                                 sizeof(shared->data_output), shared->decode_result),
			          0);
		}
		ASSERT_EQ(QueueAjmDataTestOperation(operation, batch_address, instance, input, shared->data_output, shared->decode_result,
		                                    shared->sideband_output),
		          0);

		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild([context, batch_address, shared]
		                              { (void)Ajm::AjmBatchStart(context, batch_address, 0, &shared->error, &shared->batch_id); }, &status))
		    << "strict AJM refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5));
		EXPECT_TRUE(IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5));
		EXPECT_TRUE(IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5));
		EXPECT_TRUE(IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5));
		EXPECT_TRUE(IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5));
		ASSERT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	}

	GuestValue<Ajm::AjmBatchError> cleanup_error;
	GuestValue<uint32_t>           cleanup_batch_id;
	ASSERT_TRUE(cleanup_error.IsValid());
	ASSERT_TRUE(cleanup_batch_id.IsValid());
	ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, cleanup_error.Data(), cleanup_batch_id.Data()), 0);
	ASSERT_EQ(Ajm::AjmBatchWait(context, *cleanup_batch_id.Data(), UINT32_MAX, cleanup_error.Data()), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
#endif
}

TEST(EmulatorAudio, RejectsMixedAjmControlAndRunBeforeWritingAnyJobOutput)
{
#if !defined(__linux__)
	GTEST_SKIP() << "bounded shared-memory refusal check requires Linux";
#else
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

	alignas(uint64_t) uint8_t batch[160] {};
	void* const               batch_address = batch;
	const uint8_t            input[] = {0x41, 0x4a, 0x4d, 0x02};
	SharedAjmBatchResults    shared_results;
	ASSERT_TRUE(shared_results.IsValid());
	auto* const shared = shared_results.Data();
	auto* const control_end = static_cast<uint8_t*>(Ajm::AjmBatchJobControlBufferRa(
	    batch, instance, 0, nullptr, 0, shared->sideband_output, 8, nullptr));
	ASSERT_NE(control_end, nullptr);
	auto* const end = static_cast<uint8_t*>(Ajm::AjmBatchJobRunBufferRa(
	    control_end, instance, (1ull << 47u) | 1u, const_cast<uint8_t*>(input), sizeof(input), shared->data_output,
	    sizeof(shared->data_output), shared->sideband_output + 8, sizeof(shared->sideband_output) - 8, nullptr));
	ASSERT_NE(end, nullptr);

	int status = 0;
	ASSERT_TRUE(RunAjmStrictChild([context, batch_address, end, shared] {
				  (void)Ajm::AjmBatchStartBuffer(context, static_cast<uint8_t*>(batch_address),
				                                static_cast<uint32_t>(end - static_cast<uint8_t*>(batch_address)), 0, &shared->error,
				                                &shared->batch_id);
			  }, &status))
	    << "strict AJM refusal timed out";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
	EXPECT_TRUE(IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5));
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
#endif
}

TEST(EmulatorAudio, CompletesControlOnlyAjmBufferBatch)
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
	alignas(uint64_t) uint8_t sideband[8];
	std::memset(sideband, 0xa5, sizeof(sideband));
	auto* const end = static_cast<uint8_t*>(Ajm::AjmBatchJobControlBufferRa(batch, instance, 0, nullptr, 0, sideband,
	                                                                       sizeof(sideband), nullptr));
	ASSERT_NE(end, nullptr);

	Ajm::AjmBatchError error {};
	uint32_t           batch_id = 0;
	ASSERT_EQ(Ajm::AjmBatchStartBuffer(context, batch, static_cast<uint32_t>(end - batch), 0, &error, &batch_id), 0);
	EXPECT_NE(batch_id, 0u);
	EXPECT_EQ(error.error_code, 0);
	EXPECT_TRUE(IsAjmBytePattern(sideband, sizeof(sideband), 0));
	EXPECT_EQ(Ajm::AjmBatchWait(context, batch_id, UINT32_MAX, &error), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, ClearsErrorWhenAjmBufferBatchQueueIsFull)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	struct AjmContextCleanup
	{
		~AjmContextCleanup()
		{
			if (context != 0)
			{
				(void)Ajm::AjmFinalize(context);
			}
		}

		uint32_t context = 0;
	} cleanup;
	auto&              context               = cleanup.context;
	constexpr uint32_t completed_batch_limit = 256;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);

	uint8_t               empty_batch = 0;
	Ajm::AjmBatchError    error {};
	uint32_t              batch_id = 0;
	std::vector<uint32_t> batch_ids;
	batch_ids.reserve(completed_batch_limit);
	for (uint32_t index = 0; index < completed_batch_limit; ++index)
	{
		std::memset(&error, 0xa5, sizeof(error));
		batch_id = 0xa5a5a5a5u;
		ASSERT_EQ(Ajm::AjmBatchStartBuffer(context, &empty_batch, 0, 0, &error, &batch_id), 0);
		EXPECT_TRUE(IsAjmBytePattern(&error, sizeof(error), 0));
		EXPECT_NE(batch_id, 0u);
		batch_ids.push_back(batch_id);
	}

	std::memset(&error, 0xa5, sizeof(error));
	batch_id = 0xa5a5a5a5u;
	EXPECT_EQ(Ajm::AjmBatchStartBuffer(context, &empty_batch, 0, 0, &error, &batch_id), static_cast<int32_t>(0x80930007u));
	EXPECT_TRUE(IsAjmBytePattern(&error, sizeof(error), 0));
	EXPECT_EQ(batch_id, 0xa5a5a5a5u);

	for (const uint32_t completed_batch_id: batch_ids)
	{
		ASSERT_EQ(Ajm::AjmBatchWait(context, completed_batch_id, UINT32_MAX, &error), 0);
	}
	ASSERT_EQ(Ajm::AjmFinalize(context), 0);
	cleanup.context = 0;
}

TEST(EmulatorAudio, AcceptsExtendedAudio3dOpenParameters)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	GuestReadableBlock guest_storage(0x80);
	ASSERT_TRUE(guest_storage.IsValid());
	auto* guest_bytes = static_cast<uint8_t*>(guest_storage.Data());
	std::memset(guest_bytes, 0, 0x80);
	*reinterpret_cast<uint64_t*>(guest_bytes + 0x00) = 0x28;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x08) = 0x200;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x0c) = 0;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x10) = 128;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x14) = 4;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x18) = 2;
	*reinterpret_cast<uint32_t*>(guest_bytes + 0x20) = 2;
	auto* port = reinterpret_cast<uint32_t*>(guest_bytes + 0x40);
	auto* queue_level = reinterpret_cast<uint32_t*>(guest_bytes + 0x44);
	auto* queue_available = reinterpret_cast<uint32_t*>(guest_bytes + 0x48);
	auto* capability_count = reinterpret_cast<uint32_t*>(guest_bytes + 0x4c);
	auto* capabilities = reinterpret_cast<uint32_t*>(guest_bytes + 0x50);
	*port = UINT32_MAX;
	ASSERT_EQ(Audio3d::Audio3dInitialize(0), 0);
	ASSERT_EQ(Audio3d::Audio3dPortOpen(255, reinterpret_cast<const Audio3d::Audio3dOpenParameters*>(guest_bytes), port), 0);
	EXPECT_LT(*port, 4u);

	*queue_level = UINT32_MAX;
	*queue_available = 0;
	EXPECT_EQ(Audio3d::Audio3dPortGetQueueLevel(*port, queue_level, queue_available), 0);
	EXPECT_EQ(*queue_level, 0u);
	EXPECT_EQ(*queue_available, 4u);

	*capability_count = 0;
	ASSERT_EQ(Audio3d::Audio3dPortGetAttributesSupported(*port, nullptr, capability_count), 0);
	ASSERT_EQ(*capability_count, 3u);
	*capability_count = 3;
	ASSERT_EQ(Audio3d::Audio3dPortGetAttributesSupported(*port, capabilities, capability_count), 0);
	EXPECT_EQ(*capability_count, 3u);
	EXPECT_EQ(capabilities[0], 1u);
	EXPECT_EQ(capabilities[1], 3u);
	EXPECT_EQ(capabilities[2], 9u);

	EXPECT_EQ(Audio3d::Audio3dAudioOutOpen(*port, 255, 126, 0, 256, 48000, 1), static_cast<int32_t>(0x80ea0004u));
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

TEST(EmulatorAudio, RejectsNgs2SamplerRackQueryWithAnUnreadableOption)
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
	EXPECT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x1000u, reinterpret_cast<const Ngs2::Ngs2RackOption*>(uintptr_t {1}), info),
	          static_cast<int32_t>(0x804a0081u));
	EXPECT_EQ(raw_info[0], UINT64_MAX);
	EXPECT_EQ(raw_info[1], UINT64_MAX);
	EXPECT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001u, nullptr, info), static_cast<int32_t>(0x804a0081u));
	EXPECT_EQ(raw_info[1], UINT64_MAX);
}

// A title creates its sampler and reverb racks without an option and its submixer and mastering racks with Gen5
// expanded options (0xc8 and 0xb8 bytes) that set only the voice count at +0x50.
TEST(EmulatorAudio, Ngs2CreatesRacksFromDefaultAndExpandedOptions)
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

	GuestReadableBlock submixer_option(0xc8);
	GuestReadableBlock mastering_option(0xb8);
	ASSERT_TRUE(submixer_option.IsValid());
	ASSERT_TRUE(mastering_option.IsValid());
	std::memset(submixer_option.Data(), 0, 0xc8);
	std::memset(mastering_option.Data(), 0, 0xb8);
	auto* submixer_bytes = static_cast<uint8_t*>(submixer_option.Data());
	auto* mastering_bytes = static_cast<uint8_t*>(mastering_option.Data());
	*reinterpret_cast<size_t*>(submixer_bytes)          = 0xc8;
	*reinterpret_cast<uint32_t*>(submixer_bytes + 0x50) = 5;
	*reinterpret_cast<size_t*>(mastering_bytes)          = 0xb8;
	*reinterpret_cast<uint32_t*>(mastering_bytes + 0x50) = 3;

	struct RackCase
	{
		uint32_t    rack_id;
		const void* option;
		uint32_t    voices;
	};
	const RackCase cases[] = {{0x1000, nullptr, 256}, {0x2001, nullptr, 1}, {0x2000, submixer_bytes, 5}, {0x3000, mastering_bytes, 3}};
	GuestReadableBlock rack_info_storage(sizeof(uint64_t) * 8);
	GuestValue<uintptr_t> rack_handle_storage;
	GuestValue<uintptr_t> voice_storage;
	ASSERT_TRUE(rack_info_storage.IsValid());
	ASSERT_TRUE(rack_handle_storage.IsValid());
	ASSERT_TRUE(voice_storage.IsValid());
	auto* raw_rack_info = static_cast<uint64_t*>(rack_info_storage.Data());
	auto* rack_info     = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	for (const RackCase& rack_case: cases)
	{
		const auto* option = static_cast<const Ngs2::Ngs2RackOption*>(rack_case.option);
		std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
		ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(rack_case.rack_id, option, rack_info), 0);
		GuestReadableBlock rack_storage(raw_rack_info[1]);
		ASSERT_TRUE(rack_storage.IsValid());
		raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage.Data());
		ASSERT_EQ(Ngs2::Ngs2RackCreate(system, rack_case.rack_id, option, rack_info, rack_handle_storage.Data()), 0);
		const uintptr_t rack = *rack_handle_storage.Data();
		EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, rack_case.voices - 1, voice_storage.Data()), 0);
		EXPECT_NE(*voice_storage.Data(), 0u);
		EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, rack_case.voices, voice_storage.Data()), static_cast<int32_t>(0x804a0300u));
		ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	}
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
}


namespace {

// Stands in for a guest allocator: each request gets a guest-readable block, and each call is recorded.
struct Ngs2AllocatorLog
{
	std::vector<std::unique_ptr<GuestReadableBlock>> blocks;
	std::vector<uint64_t>                            requested;
	std::vector<uint64_t>                            freed;
	std::vector<uint64_t>                            user_data;
};
Ngs2AllocatorLog g_ngs2_allocator_log;

// The handlers take the 64-byte buffer info: host_buffer at +0, host_buffer_size at +8, user_data at +56.
uint64_t KYTY_SYSV_ABI Ngs2TestAlloc(uint64_t info_address, uint64_t /*unused1*/, uint64_t /*unused2*/)
{
	auto* info = reinterpret_cast<uint64_t*>(info_address);
	auto  block = std::make_unique<GuestReadableBlock>(info[1]);
	if (!block->IsValid())
	{
		return 1;
	}
	info[0] = reinterpret_cast<uintptr_t>(block->Data());
	g_ngs2_allocator_log.requested.push_back(info[1]);
	g_ngs2_allocator_log.user_data.push_back(info[7]);
	g_ngs2_allocator_log.blocks.push_back(std::move(block));
	return 0;
}

uint64_t KYTY_SYSV_ABI Ngs2TestFree(uint64_t info_address, uint64_t /*unused1*/, uint64_t /*unused2*/)
{
	const auto* info = reinterpret_cast<const uint64_t*>(info_address);
	g_ngs2_allocator_log.freed.push_back(info[0]);
	g_ngs2_allocator_log.user_data.push_back(info[7]);
	return 0;
}

} // namespace

TEST(EmulatorAudio, Ngs2AllocatorCreatesTakeAndReturnGuestWorkspaces)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ScopedHostRuntime runtime;
	g_ngs2_allocator_log = {};

	// {alloc_handler, free_handler, user_data}
	GuestReadableBlock allocator_storage(sizeof(uint64_t) * 3);
	GuestReadableBlock info_storage(sizeof(uint64_t) * 8);
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(allocator_storage.IsValid());
	ASSERT_TRUE(info_storage.IsValid());
	ASSERT_TRUE(handle_storage.IsValid());
	auto* allocator = static_cast<uint64_t*>(allocator_storage.Data());
	allocator[0]    = 0;
	allocator[1]    = reinterpret_cast<uintptr_t>(&Ngs2TestFree);
	allocator[2]    = 0x1234;
	const auto* guest_allocator = reinterpret_cast<const Ngs2::Ngs2BufferAllocator*>(allocator);
	auto*       info            = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(info_storage.Data());
	auto*       raw_info        = static_cast<uint64_t*>(info_storage.Data());

	// Both handlers are required; nothing is allocated otherwise.
	EXPECT_EQ(Ngs2::Ngs2SystemCreateWithAllocator(nullptr, guest_allocator, handle_storage.Data()), static_cast<int32_t>(0x804a0206u));
	EXPECT_TRUE(g_ngs2_allocator_log.requested.empty());
	allocator[0] = reinterpret_cast<uintptr_t>(&Ngs2TestAlloc);

	// The handler is asked for the size the buffer-info variants report, with the allocator's user data.
	std::memset(raw_info, 0, sizeof(uint64_t) * 8);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, info), 0);
	const uint64_t system_size = raw_info[1];
	ASSERT_EQ(Ngs2::Ngs2SystemCreateWithAllocator(nullptr, guest_allocator, handle_storage.Data()), 0);
	const uintptr_t system = *handle_storage.Data();
	ASSERT_EQ(g_ngs2_allocator_log.requested.size(), 1u);
	EXPECT_EQ(g_ngs2_allocator_log.requested[0], system_size);
	EXPECT_EQ(system, reinterpret_cast<uintptr_t>(g_ngs2_allocator_log.blocks[0]->Data()));

	std::memset(raw_info, 0, sizeof(uint64_t) * 8);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x1000u, nullptr, info), 0);
	const uint64_t rack_size = raw_info[1];
	ASSERT_EQ(Ngs2::Ngs2RackCreateWithAllocator(system, 0x1000u, nullptr, guest_allocator, handle_storage.Data()), 0);
	const uintptr_t rack = *handle_storage.Data();
	ASSERT_EQ(g_ngs2_allocator_log.requested.size(), 2u);
	EXPECT_EQ(g_ngs2_allocator_log.requested[1], rack_size);
	GuestValue<uintptr_t> voice_storage;
	ASSERT_TRUE(voice_storage.IsValid());
	EXPECT_EQ(Ngs2::Ngs2RackGetVoiceHandle(rack, 0, voice_storage.Data()), 0);

	// A create that fails after the allocation hands the buffer straight back.
	EXPECT_EQ(Ngs2::Ngs2RackCreateWithAllocator(0x10, 0x1000u, nullptr, guest_allocator, handle_storage.Data()),
	          static_cast<int32_t>(0x804a0201u));
	ASSERT_EQ(g_ngs2_allocator_log.freed.size(), 1u);
	EXPECT_EQ(g_ngs2_allocator_log.freed[0], reinterpret_cast<uintptr_t>(g_ngs2_allocator_log.blocks[2]->Data()));

	// Destroys return each workspace to the allocator.
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rack, nullptr), 0);
	ASSERT_EQ(g_ngs2_allocator_log.freed.size(), 2u);
	EXPECT_EQ(g_ngs2_allocator_log.freed[1], rack);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
	ASSERT_EQ(g_ngs2_allocator_log.freed.size(), 3u);
	EXPECT_EQ(g_ngs2_allocator_log.freed[2], system);
	for (const uint64_t user_data: g_ngs2_allocator_log.user_data)
	{
		EXPECT_EQ(user_data, 0x1234u);
	}
	g_ngs2_allocator_log = {};
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

TEST(EmulatorAudio, Ngs2RejectsUnreadableAllocatorOptionsAndUnmeasuredGeometryWithoutWrites)
{
	constexpr auto kUnsupported   = static_cast<int32_t>(0x804a0309u);
	constexpr auto kInvalidOption = static_cast<int32_t>(0x804a0081u);
	GuestValue<uintptr_t> handle_storage;
	ASSERT_TRUE(handle_storage.IsValid());
	*handle_storage.Data() = UINTPTR_MAX;
	uint8_t        output[256];
	std::memset(output, 0xa5, sizeof(output));

	// An unreadable option is refused before the allocator is called.
	EXPECT_EQ(Ngs2::Ngs2SystemCreateWithAllocator(reinterpret_cast<const Ngs2::Ngs2SystemOption*>(uintptr_t {1}),
	                                               reinterpret_cast<const Ngs2::Ngs2BufferAllocator*>(uintptr_t {1}), handle_storage.Data()),
	          kInvalidOption);
	EXPECT_EQ(*handle_storage.Data(), UINTPTR_MAX);
	EXPECT_EQ(Ngs2::Ngs2RackCreateWithAllocator(1, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(uintptr_t {1}),
	                                             reinterpret_cast<const Ngs2::Ngs2BufferAllocator*>(uintptr_t {1}), handle_storage.Data()),
	          kInvalidOption);
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

TEST(EmulatorAudio, Ngs2StartsLibcBackedMasteringVoiceBeforeTheApplicationHeap)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// A title without a malloc replacement builds its NGS2 objects in libc memory, which is host-backed until the
	// application heap exists. It is guest memory by contract even though it is not a guest mapping.
	auto* raw_system_info = static_cast<uint64_t*>(Libs::LibC::c_malloc(sizeof(uint64_t) * 8));
	ASSERT_NE(raw_system_info, nullptr);
	auto* system_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_system_info);
	ASSERT_EQ(Ngs2::Ngs2SystemQueryBufferSize(nullptr, system_info), 0);
	void* system_storage = Libs::LibC::c_malloc(raw_system_info[1]);
	ASSERT_NE(system_storage, nullptr);
	EXPECT_FALSE(Core::VirtualMemory::IsRangeGuestOwned(reinterpret_cast<uint64_t>(system_storage), raw_system_info[1]));
	raw_system_info[0]    = reinterpret_cast<uintptr_t>(system_storage);
	auto* handle_storage  = static_cast<uintptr_t*>(Libs::LibC::c_malloc(sizeof(uintptr_t) * 2));
	ASSERT_NE(handle_storage, nullptr);
	ASSERT_EQ(Ngs2::Ngs2SystemCreate(nullptr, system_info, &handle_storage[0]), 0);
	const uintptr_t system = handle_storage[0];

	auto* raw_option = static_cast<uint8_t*>(Libs::LibC::c_malloc(0x518));
	ASSERT_NE(raw_option, nullptr);
	std::memset(raw_option, 0, 0x518);
	*reinterpret_cast<size_t*>(raw_option)          = 0x518;
	*reinterpret_cast<uint32_t*>(raw_option + 0x50) = 1;
	auto* raw_rack_info = static_cast<uint64_t*>(Libs::LibC::c_malloc(sizeof(uint64_t) * 8));
	ASSERT_NE(raw_rack_info, nullptr);
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	auto* rack_info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_rack_info);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info), 0);
	void* rack_storage = Libs::LibC::c_malloc(raw_rack_info[1]);
	ASSERT_NE(rack_storage, nullptr);
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(rack_storage);
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x4001, reinterpret_cast<const Ngs2::Ngs2RackOption*>(raw_option), rack_info,
	                               &handle_storage[1]),
	          0);
	const uintptr_t sampler_rack = handle_storage[1];

	// The mastering voice the title routes everything to is started with a play event before its output thread runs.
	std::memset(raw_rack_info, 0, sizeof(uint64_t) * 8);
	ASSERT_EQ(Ngs2::Ngs2RackQueryBufferSize(0x3000, nullptr, rack_info), 0);
	void* mastering_storage = Libs::LibC::c_malloc(raw_rack_info[1]);
	ASSERT_NE(mastering_storage, nullptr);
	raw_rack_info[0] = reinterpret_cast<uintptr_t>(mastering_storage);
	ASSERT_EQ(Ngs2::Ngs2RackCreate(system, 0x3000, nullptr, rack_info, &handle_storage[1]), 0);
	auto* mastering_voice = static_cast<uintptr_t*>(Libs::LibC::c_malloc(sizeof(uintptr_t)));
	ASSERT_NE(mastering_voice, nullptr);
	ASSERT_EQ(Ngs2::Ngs2RackGetVoiceHandle(handle_storage[1], 0, mastering_voice), 0);
	auto* play_command = static_cast<uint32_t*>(Libs::LibC::c_malloc(sizeof(uint32_t) * 3));
	ASSERT_NE(play_command, nullptr);
	play_command[0] = 2;
	play_command[1] = 0x400;
	play_command[2] = 1;
	EXPECT_EQ(Ngs2::Ngs2VoiceRunCommands(*mastering_voice, play_command, 1), 0);
	play_command[2] = 3;
	EXPECT_EQ(Ngs2::Ngs2VoiceRunCommands(*mastering_voice, play_command, 1), static_cast<int32_t>(0x804a0309u));

	ASSERT_EQ(Ngs2::Ngs2RackDestroy(handle_storage[1], nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(sampler_rack, nullptr), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemDestroy(system), 0);
	Libs::LibC::c_free(play_command);
	Libs::LibC::c_free(mastering_voice);
	Libs::LibC::c_free(mastering_storage);

	// Freed libc memory is no longer accepted.
	const auto freed_info = reinterpret_cast<uint64_t>(raw_system_info);
	Libs::LibC::c_free(raw_system_info);
	EXPECT_FALSE(Core::VirtualMemory::IsRangeWritableOrLent(freed_info, sizeof(uint64_t)));
	EXPECT_TRUE(Core::VirtualMemory::IsRangeWritableOrLent(reinterpret_cast<uint64_t>(raw_rack_info) + 8, 8));
	EXPECT_FALSE(Core::VirtualMemory::IsRangeWritableOrLent(reinterpret_cast<uint64_t>(raw_rack_info) + 8, sizeof(uint64_t) * 8));
	Libs::LibC::c_free(rack_storage);
	Libs::LibC::c_free(raw_rack_info);
	Libs::LibC::c_free(raw_option);
	Libs::LibC::c_free(handle_storage);
	Libs::LibC::c_free(system_storage);
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

TEST(EmulatorAudio, Ngs2SystemRenderClearsEveryBufferWithoutARoutedVoice)
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

	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_infos), 2), 0);
	EXPECT_FLOAT_EQ(output0[0], 0.0f);
	EXPECT_FLOAT_EQ(output0[256 * 2 - 1], 0.0f);
	EXPECT_FLOAT_EQ(output1[0], 0.0f);
	EXPECT_FLOAT_EQ(output1[256 * 2 - 1], 0.0f);

	// A buffer below one grain is refused before any buffer is written.
	std::fill_n(output0, 256 * 2, 0.25f);
	std::fill_n(output1, 256 * 2, 0.5f);
	render_infos[4] = kGrainBytes - 1;
	EXPECT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_infos), 2),
	          static_cast<int32_t>(0x804a0209u));
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

namespace {

constexpr uint32_t kSubmixerRackId  = 0x2000;
constexpr uint32_t kMasteringRackId = 0x3000;

// A rack created from the default option, with its single voice started by a play event.
struct PlayingRack
{
	std::unique_ptr<GuestReadableBlock> workspace;
	uintptr_t                           rack  = 0;
	uintptr_t                           voice = 0;
};

bool CreatePlayingRack(uintptr_t system, uint32_t rack_id, PlayingRack* out)
{
	GuestReadableBlock    info_storage(sizeof(uint64_t) * 8);
	GuestReadableBlock    command_storage(sizeof(uint32_t) * 3);
	GuestValue<uintptr_t> rack_storage;
	GuestValue<uintptr_t> voice_storage;
	if (!info_storage.IsValid() || !command_storage.IsValid() || !rack_storage.IsValid() || !voice_storage.IsValid())
	{
		return false;
	}
	auto* raw_info = static_cast<uint64_t*>(info_storage.Data());
	std::memset(raw_info, 0, sizeof(uint64_t) * 8);
	auto* info = reinterpret_cast<Ngs2::Ngs2ContextBufferInfo*>(raw_info);
	if (Ngs2::Ngs2RackQueryBufferSize(rack_id, nullptr, info) != 0)
	{
		return false;
	}
	out->workspace = std::make_unique<GuestReadableBlock>(raw_info[1]);
	if (!out->workspace->IsValid())
	{
		return false;
	}
	raw_info[0] = reinterpret_cast<uintptr_t>(out->workspace->Data());
	if (Ngs2::Ngs2RackCreate(system, rack_id, nullptr, info, rack_storage.Data()) != 0)
	{
		return false;
	}
	out->rack = *rack_storage.Data();
	if (Ngs2::Ngs2RackGetVoiceHandle(out->rack, 0, voice_storage.Data()) != 0)
	{
		return false;
	}
	out->voice  = *voice_storage.Data();
	auto* play  = static_cast<uint32_t*>(command_storage.Data());
	play[0]     = 2;
	play[1]     = 0x400;
	play[2]     = 1;
	return Ngs2::Ngs2VoiceRunCommands(out->voice, play, 1) == 0;
}

// The patch param: port 0 of voice to input 0 of destination, a voice or a single-voice rack.
int32_t PatchVoice(uintptr_t voice, uintptr_t destination)
{
	GuestReadableBlock storage(24);
	if (!storage.IsValid())
	{
		return static_cast<int32_t>(0x804a0309u);
	}
	auto* param = static_cast<uint8_t*>(storage.Data());
	std::memset(param, 0, 24);
	*reinterpret_cast<uint16_t*>(param + 0)   = 24;
	*reinterpret_cast<uint32_t*>(param + 4)   = 0x5u;
	*reinterpret_cast<uintptr_t*>(param + 16) = destination;
	return Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param));
}

} // namespace

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
	PlayingRack     mastering;
	ASSERT_TRUE(CreatePlayingRack(system, kMasteringRackId, &mastering));
	ASSERT_EQ(PatchVoice(voice, mastering.rack), 0);

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

	// A block submitted while one plays queues behind it: the first keeps playing and the second follows without a gap.
	GuestReadableBlock next_storage(sizeof(int16_t) * kSourceFrames + sizeof(uint64_t) * 5);
	ASSERT_TRUE(next_storage.IsValid());
	auto* next_source = static_cast<int16_t*>(next_storage.Data());
	std::fill_n(next_source, kSourceFrames, int16_t {-16384});
	auto* next_context = reinterpret_cast<uint64_t*>(next_source + kSourceFrames);
	next_context[0]    = 0;
	next_context[1]    = sizeof(int16_t) * kSourceFrames;
	next_context[2]    = 0;
	next_context[3]    = kSourceFrames;
	next_context[4]    = 0;
	*reinterpret_cast<uintptr_t*>(waveform_param + 8)  = reinterpret_cast<uintptr_t>(next_source);
	*reinterpret_cast<uintptr_t*>(waveform_param + 24) = reinterpret_cast<uintptr_t>(next_context);
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_GT(output[0], 0.0f);
	// 48 kHz output frames 512..767 read 44.1 kHz source frames 470..704: the first block ends at 512.
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_GT(output[0], 0.0f);
	EXPECT_LT(output[2 * 255], 0.0f);
	for (int grain = 0; grain < 3; grain++)
	{
		ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	}
	EXPECT_FLOAT_EQ(output[0], 0.0f);

	// A block started part-way: the byte offset skips the silent first half, and size and frames count the rest.
	std::fill_n(next_source, kSourceFrames / 2, int16_t {0});
	next_context[0] = sizeof(int16_t) * kSourceFrames / 2;
	next_context[1] = sizeof(int16_t) * kSourceFrames / 2;
	next_context[3] = kSourceFrames / 2;
	ASSERT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)), 0);
	ASSERT_EQ(Ngs2::Ngs2VoiceRunCommands(voice, play_command, 1), 0);
	ASSERT_EQ(Ngs2::Ngs2SystemRender(system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(render_info), 1), 0);
	EXPECT_LT(output[0], 0.0f);
	next_context[0] = 1;
	EXPECT_EQ(Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(waveform_param)),
	          static_cast<int32_t>(0x804a0309u));

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

// Owns a sampler system, its rack, the first voice and a playing mastering voice every sampler voice is patched to.
// The racks and system are destroyed before their guest workspaces are released.
struct SamplerRig
{
	std::unique_ptr<GuestReadableBlock> system_workspace;
	std::unique_ptr<GuestReadableBlock> rack_workspace;
	PlayingRack                         mastering;
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
		if (mastering.rack != 0)
		{
			(void)Ngs2::Ngs2RackDestroy(mastering.rack, nullptr);
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
	if (!CreatePlayingRack(rig->system, kMasteringRackId, &rig->mastering))
	{
		return false;
	}
	for (uint32_t i = voice_count; i > 0; i--)
	{
		if (Ngs2::Ngs2RackGetVoiceHandle(rig->rack, i - 1, voice_handle_storage.Data()) != 0 ||
		    PatchVoice(*voice_handle_storage.Data(), rig->mastering.rack) != 0)
		{
			return false;
		}
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

namespace {

int32_t RunSamplerGain(uintptr_t voice, float gain)
{
	GuestReadableBlock command_storage(sizeof(uint32_t) * 3);
	if (!command_storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* command = static_cast<uint32_t*>(command_storage.Data());
	command[0]    = 6;     // port volume command
	command[1]    = 0x100; // float value type
	std::memcpy(&command[2], &gain, sizeof(gain));
	return Ngs2::Ngs2VoiceRunCommands(voice, command, 1);
}

int32_t SetRoutingValue(uintptr_t voice, uint32_t id, uint32_t value)
{
	GuestReadableBlock storage(16);
	if (!storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* param = static_cast<uint8_t*>(storage.Data());
	std::memset(param, 0, 16);
	*reinterpret_cast<uint16_t*>(param + 0) = 16;
	*reinterpret_cast<uint32_t*>(param + 4) = id;
	*reinterpret_cast<uint32_t*>(param + 8) = value;
	return Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param));
}

// Queues one 256-frame mono PCM16 block of a constant sample and plays it.
bool PlayConstantBlock(uintptr_t voice, GuestReadableBlock* pcm_storage, int16_t sample)
{
	if (!pcm_storage->IsValid())
	{
		return false;
	}
	std::fill_n(static_cast<int16_t*>(pcm_storage->Data()), kSamplerGrainFrames, sample);
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * kSamplerGrainFrames;
	block.num_samples = kSamplerGrainFrames;
	return SetSamplerFormat(voice, kWaveformPcmI16, 1, 48000) == 0 &&
	       AddSamplerBlock(voice, pcm_storage->Data(), kBlocksFlagReset, block) == 0 && RunSamplerEvent(voice, kSamplerPlayEvent) == 0;
}

} // namespace

TEST(EmulatorAudio, Ngs2RoutesASamplerThroughASubmixerToItsMasteringBuffer)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	GuestReadableBlock pcm_storage(sizeof(int16_t) * kSamplerGrainFrames);
	ASSERT_TRUE(PlayConstantBlock(rig.voice, &pcm_storage, 16384));

	// sampler -> submixer (port volume 0.5) -> second mastering voice, which writes mono render buffer 1.
	PlayingRack submixer;
	PlayingRack mono_mastering;
	ASSERT_TRUE(CreatePlayingRack(rig.system, kSubmixerRackId, &submixer));
	ASSERT_TRUE(CreatePlayingRack(rig.system, kMasteringRackId, &mono_mastering));
	ASSERT_EQ(SetRoutingValue(submixer.voice, 0x20000000u, 2), 0);
	EXPECT_EQ(SetRoutingValue(submixer.voice, 0x20000000u, 1), kSamplerInvalidControl);
	ASSERT_EQ(SetRoutingValue(mono_mastering.voice, 0x30000000u, 1), 0);
	ASSERT_EQ(SetRoutingValue(mono_mastering.voice, 0x30000005u, 1), 0);
	ASSERT_EQ(PatchVoice(rig.voice, submixer.voice), 0);
	ASSERT_EQ(PatchVoice(submixer.voice, mono_mastering.rack), 0);
	ASSERT_EQ(RunSamplerGain(submixer.voice, 0.5f), 0);

	// A patch that closes a cycle, starts at a mastering voice or ends at a source voice is refused.
	EXPECT_EQ(PatchVoice(submixer.voice, submixer.voice), kSamplerInvalidControl);
	EXPECT_EQ(PatchVoice(mono_mastering.voice, submixer.voice), kSamplerInvalidControl);
	EXPECT_EQ(PatchVoice(submixer.voice, rig.voice), kSamplerInvalidControl);

	constexpr size_t   kStereoBytes = sizeof(float) * kSamplerGrainFrames * 2;
	constexpr size_t   kMonoBytes   = sizeof(int16_t) * kSamplerGrainFrames;
	GuestReadableBlock storage(kStereoBytes + kMonoBytes + sizeof(uint64_t) * 6);
	ASSERT_TRUE(storage.IsValid());
	auto* bytes  = static_cast<uint8_t*>(storage.Data());
	auto* stereo = reinterpret_cast<float*>(bytes);
	auto* mono   = reinterpret_cast<int16_t*>(bytes + kStereoBytes);
	auto* infos  = reinterpret_cast<uint64_t*>(bytes + kStereoBytes + kMonoBytes);
	std::fill_n(stereo, kSamplerGrainFrames * 2, 1.0f);
	infos[0] = reinterpret_cast<uintptr_t>(stereo);
	infos[1] = kStereoBytes;
	infos[2] = (uint64_t {2} << 32u) | kWaveformPcmF32;
	infos[3] = reinterpret_cast<uintptr_t>(mono);
	infos[4] = kMonoBytes;
	infos[5] = (uint64_t {1} << 32u) | kWaveformPcmI16;
	ASSERT_EQ(Ngs2::Ngs2SystemRender(rig.system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(infos), 2), 0);
	// 0.5 * 0.5 on both bus channels; the mono buffer takes their mean.
	EXPECT_EQ(mono[0], 8192);
	EXPECT_EQ(mono[kSamplerGrainFrames - 1], 8192);
	EXPECT_FLOAT_EQ(stereo[0], 0.0f);

	// A mastering voice whose setup names another channel count than its buffer refuses the render.
	ASSERT_EQ(SetRoutingValue(mono_mastering.voice, 0x30000000u, 2), 0);
	EXPECT_EQ(Ngs2::Ngs2SystemRender(rig.system, reinterpret_cast<const Ngs2::Ngs2RenderBufferInfo*>(infos), 2),
	          static_cast<int32_t>(0x804a0206u));

	(void)Ngs2::Ngs2RackDestroy(mono_mastering.rack, nullptr);
	(void)Ngs2::Ngs2RackDestroy(submixer.rack, nullptr);
}

namespace {

constexpr uint32_t kReverbRackId = 0x2001;

int32_t SetReverbSetup(uintptr_t voice, uint32_t channels, uint32_t flags)
{
	GuestReadableBlock storage(24);
	if (!storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* param = static_cast<uint8_t*>(storage.Data());
	std::memset(param, 0, 24);
	*reinterpret_cast<uint16_t*>(param + 0)  = 24;
	*reinterpret_cast<uint32_t*>(param + 4)  = 0x20010000u;
	*reinterpret_cast<uint32_t*>(param + 8)  = channels; // input channels
	*reinterpret_cast<uint32_t*>(param + 12) = channels; // output channels
	*reinterpret_cast<uint32_t*>(param + 16) = flags;
	return Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param));
}

// The 96-byte I3DL2 block of a late-reverb-only room, with the first reserved word set as the guest library sets it.
int32_t SetReverbI3dl2(uintptr_t voice, float wet, float dry)
{
	GuestReadableBlock storage(96);
	if (!storage.IsValid())
	{
		return kSamplerInvalidControl;
	}
	auto* param = static_cast<uint8_t*>(storage.Data());
	std::memset(param, 0, 96);
	*reinterpret_cast<uint16_t*>(param + 0) = 96;
	*reinterpret_cast<uint32_t*>(param + 4) = 0x20010001u;
	const float floats[]                    = {wet, dry};
	std::memcpy(param + 8, floats, sizeof(floats));
	const float   decay[]       = {2.0f, 1.0f};             // decay_time, decay_hf_ratio
	const int32_t reflections   = -10000;                   // no early reflections
	const float   diffusion[]   = {100.0f, 100.0f, 5000.0f}; // diffusion, density, hf_reference
	std::memcpy(param + 28, decay, sizeof(decay));
	std::memcpy(param + 36, &reflections, sizeof(reflections));
	std::memcpy(param + 52, diffusion, sizeof(diffusion));
	*reinterpret_cast<uint32_t*>(param + 64) = 8;
	return Ngs2::Ngs2VoiceControl(voice, reinterpret_cast<const Ngs2::Ngs2VoiceParamHeader*>(param));
}

float RenderPeak(uintptr_t system, GuestReadableBlock* output_storage)
{
	if (RenderSamplerGrain(system, output_storage->Data()) != 0)
	{
		return -1.0f;
	}
	const auto* output = static_cast<const float*>(output_storage->Data());
	float       peak   = 0.0f;
	for (uint32_t i = 0; i < kSamplerGrainFrames * 2; i++)
	{
		peak = std::max(peak, std::abs(output[i]));
	}
	return peak;
}

} // namespace

TEST(EmulatorAudio, Ngs2ReverbVoicePassesItsInputThroughTheReverb)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	PlayingRack reverb;
	ASSERT_TRUE(CreatePlayingRack(rig.system, kReverbRackId, &reverb));
	// The bus is stereo: other channel counts and flags are refused.
	EXPECT_EQ(SetReverbSetup(reverb.voice, 8, 0), kSamplerInvalidControl);
	EXPECT_EQ(SetReverbSetup(reverb.voice, 2, 1), kSamplerInvalidControl);
	ASSERT_EQ(SetReverbSetup(reverb.voice, 2, 0), 0);
	ASSERT_EQ(PatchVoice(rig.voice, reverb.voice), 0);
	ASSERT_EQ(PatchVoice(reverb.voice, rig.mastering.rack), 0);

	GuestReadableBlock pcm_storage(sizeof(int16_t) * kSamplerGrainFrames);
	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	const auto* output = static_cast<const float*>(output_storage.Data());

	// A voice that was only set up passes silence.
	ASSERT_TRUE(PlayConstantBlock(rig.voice, &pcm_storage, 16384));
	EXPECT_EQ(RenderPeak(rig.system, &output_storage), 0.0f);

	// No wet gain: the dry path alone.
	ASSERT_EQ(SetReverbI3dl2(reverb.voice, 0.0f, 0.5f), 0);
	EXPECT_EQ(SetReverbI3dl2(reverb.voice, std::numeric_limits<float>::quiet_NaN(), 0.5f), kSamplerInvalidControl);
	ASSERT_TRUE(PlayConstantBlock(rig.voice, &pcm_storage, 16384));
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_FLOAT_EQ(output[0], 0.25f);
	EXPECT_FLOAT_EQ(output[kSamplerGrainFrames * 2 - 1], 0.25f);

	// Wet only: the tail goes on after the sampler ends, until an immediate stop clears it.
	ASSERT_EQ(SetReverbI3dl2(reverb.voice, 1.0f, 0.0f), 0);
	ASSERT_TRUE(PlayConstantBlock(rig.voice, &pcm_storage, 16384));
	float tail = 0.0f;
	for (uint32_t grain = 0; grain < 24; grain++)
	{
		const float peak = RenderPeak(rig.system, &output_storage);
		ASSERT_GE(peak, 0.0f);
		tail = grain >= 12 ? std::max(tail, peak) : tail;
	}
	EXPECT_GT(tail, 0.0f);
	ASSERT_EQ(RunSamplerEvent(reverb.voice, 0x4), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	ASSERT_EQ(RunSamplerEvent(reverb.voice, kSamplerPlayEvent), 0);
	EXPECT_EQ(RenderPeak(rig.system, &output_storage), 0.0f);

	(void)Ngs2::Ngs2RackDestroy(reverb.rack, nullptr);
}

TEST(EmulatorAudio, Ngs2PortInfoReportsThePortVolumeAndPatchDestination)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	ASSERT_EQ(RunSamplerGain(rig.voice, 0.25f), 0);

	// {matrix_id, volume, num_delay_samples, destination_input, destination}
	GuestReadableBlock info_storage(24);
	ASSERT_TRUE(info_storage.IsValid());
	auto* info = static_cast<uint8_t*>(info_storage.Data());
	std::memset(info, 0xa5, 24);
	ASSERT_EQ(Ngs2::Ngs2VoiceGetPortInfo(rig.voice, 0, info, 24), 0);
	EXPECT_EQ(*reinterpret_cast<const int32_t*>(info + 0), -1);
	EXPECT_FLOAT_EQ(*reinterpret_cast<const float*>(info + 4), 0.25f);
	EXPECT_EQ(*reinterpret_cast<const uint32_t*>(info + 8), 0u);
	EXPECT_EQ(*reinterpret_cast<const uint32_t*>(info + 12), 0u);
	EXPECT_EQ(*reinterpret_cast<const uintptr_t*>(info + 16), rig.mastering.rack);

	// Only port 0 exists, and the size must be the layout's.
	std::memset(info, 0xa5, 24);
	EXPECT_EQ(Ngs2::Ngs2VoiceGetPortInfo(rig.voice, 1, info, 24), kSamplerInvalidControl);
	EXPECT_EQ(Ngs2::Ngs2VoiceGetPortInfo(rig.voice, 0, info, 23), kSamplerInvalidControl);
	EXPECT_EQ(info[0], 0xa5);

	// A setup restores unit volume; a voice whose destination is gone reports no destination.
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000), 0);
	ASSERT_EQ(Ngs2::Ngs2RackDestroy(rig.mastering.rack, nullptr), 0);
	rig.mastering.rack = 0;
	ASSERT_EQ(Ngs2::Ngs2VoiceGetPortInfo(rig.voice, 0, info, 24), 0);
	EXPECT_FLOAT_EQ(*reinterpret_cast<const float*>(info + 4), 1.0f);
	EXPECT_EQ(*reinterpret_cast<const uintptr_t*>(info + 16), 0u);
}

TEST(EmulatorAudio, Ngs2SamplerEventsStopPauseAndResumePlayback)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	SamplerRig rig;
	ASSERT_TRUE(CreateSamplerRig(&rig));
	// A ramp three grains long: frame k is k * 32 / 32768 of full scale.
	constexpr uint32_t kFrames = kSamplerGrainFrames * 3;
	GuestReadableBlock pcm_storage(sizeof(int16_t) * kFrames);
	ASSERT_TRUE(pcm_storage.IsValid());
	auto* pcm = static_cast<int16_t*>(pcm_storage.Data());
	for (uint32_t k = 0; k < kFrames; k++)
	{
		pcm[k] = static_cast<int16_t>(k * 32);
	}
	SamplerBlock block {};
	block.data_size   = sizeof(int16_t) * kFrames;
	block.num_samples = kFrames;
	ASSERT_EQ(SetSamplerFormat(rig.voice, kWaveformPcmI16, 1, 48000), 0);
	ASSERT_EQ(AddSamplerBlock(rig.voice, pcm_storage.Data(), kBlocksFlagReset, block), 0);
	ASSERT_EQ(RunSamplerEvent(rig.voice, kSamplerPlayEvent), 0);

	GuestReadableBlock output_storage(sizeof(float) * kSamplerGrainFrames * 2);
	ASSERT_TRUE(output_storage.IsValid());
	const auto* output = static_cast<const float*>(output_storage.Data());
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_NEAR(output[2 * 1], 32.0f / 32768.0f, 1e-6f);

	// Pause (0x10) renders silence and keeps the position; resume (0x20) continues from it.
	ASSERT_EQ(RunSamplerEvent(rig.voice, 0x10), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_EQ(output[2 * 1], 0.0f);
	ASSERT_EQ(RunSamplerEvent(rig.voice, 0x20), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_NEAR(output[0], static_cast<float>(kSamplerGrainFrames * 32) / 32768.0f, 1e-6f);

	// Stop (0x2) ends playback with a grain still queued: the voice reports stopped, then empty.
	ASSERT_EQ(RunSamplerEvent(rig.voice, 0x2), 0);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	EXPECT_EQ(output[0], 0.0f);
	GuestValue<uint32_t> flags;
	ASSERT_TRUE(flags.IsValid());
	ASSERT_EQ(Ngs2::Ngs2VoiceGetStateFlags(rig.voice, flags.Data()), 0);
	EXPECT_EQ(*flags.Data(), 0xbu);
	ASSERT_EQ(RenderSamplerGrain(rig.system, output_storage.Data()), 0);
	ASSERT_EQ(Ngs2::Ngs2VoiceGetStateFlags(rig.voice, flags.Data()), 0);
	EXPECT_EQ(*flags.Data(), 0u);

	// Values that are not one event bit are refused.
	EXPECT_EQ(RunSamplerEvent(rig.voice, 0x3), kSamplerInvalidControl);
	EXPECT_EQ(RunSamplerEvent(rig.voice, 0x40), kSamplerInvalidControl);
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

// One mono frame with three bands, no band extension, a gradient whose curve
// starts past the ten coded units, ten fixed-length scalefactors of 24, and 24
// fixed-length coarse and fine coefficient fields. A zero coefficient value
// produces silence; a nonzero value produces a measurable PCM16 signal.
void PutAtrac9Frame(BitWriter* writer, uint16_t coefficient_value, bool first_in_superframe)
{
	writer->Put(first_in_superframe ? 0 : 1, 1); // 0 only in the superframe's first frame
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
	writer->Put(3, 2);  // length 5
	for (int unit = 0; unit < 10; unit++)
	{
		writer->Put(24, 5);
	}
	for (int coefficient = 0; coefficient < 24; coefficient++)
	{
		writer->Put(coefficient_value, 16);
	}
	for (int coefficient = 0; coefficient < 24; coefficient++)
	{
		writer->Put(coefficient_value, 10);
	}
	writer->Align();
}

std::vector<uint8_t> BuildAtrac9Superframe(uint16_t coefficient_value)
{
	BitWriter writer;
	for (int frame = 0; frame < 4; frame++)
	{
		PutAtrac9Frame(&writer, coefficient_value, frame == 0);
	}
	auto bytes = writer.Bytes();
	bytes.resize(kSyntheticAtrac9Superframe, 0);
	return bytes;
}

std::vector<uint8_t> BuildSilentAtrac9Superframe()
{
	return BuildAtrac9Superframe(0);
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

constexpr uint32_t kAtrac9GaplessPrimingFrames = 256;
constexpr uint32_t kAtrac9GaplessTotalFrames   = 1500; // Ends inside the second superframe.

struct AjmDecodeResultFields
{
	int32_t  status          = -1;
	int32_t  codec_result    = -1;
	int32_t  input_consumed  = -1;
	int32_t  output_produced = -1;
	uint64_t delivered       = UINT64_MAX;
};

AjmDecodeResultFields ReadAjmDecodeResult(const void* storage)
{
	const auto*           bytes = static_cast<const uint8_t*>(storage);
	AjmDecodeResultFields fields {};
	std::memcpy(&fields.status, bytes, sizeof(fields.status));
	std::memcpy(&fields.codec_result, bytes + 4, sizeof(fields.codec_result));
	std::memcpy(&fields.input_consumed, bytes + 8, sizeof(fields.input_consumed));
	std::memcpy(&fields.output_produced, bytes + 12, sizeof(fields.output_produced));
	std::memcpy(&fields.delivered, bytes + 16, sizeof(fields.delivered));
	return fields;
}

bool IsAjmDecodeResult(const void* storage, int32_t input_consumed, int32_t output_produced, uint64_t delivered)
{
	const auto fields = ReadAjmDecodeResult(storage);
	return fields.status == 0 && fields.codec_result == 0 && fields.input_consumed == input_consumed &&
	       fields.output_produced == output_produced && fields.delivered == delivered &&
	       IsAjmBytePattern(static_cast<const uint8_t*>(storage) + 24, 8, 0xa5);
}

bool IsAjmControlResult(const void* storage)
{
	return IsAjmBytePattern(storage, 8, 0) && IsAjmBytePattern(static_cast<const uint8_t*>(storage) + 8, 24, 0xa5);
}

// Runs one finite gapless ATRAC9 stream through three batches: a first call that trims the priming frames and retains the
// rest of the superframe, a two-job batch that drains pending PCM, decodes the next superframe up to the output limit and
// drains the remainder without new input, a call after the limit, and a reset that restarts the stream's accounting.
// `expected` is the oracle PCM after trimming.
int RunAjmAtrac9GaplessScenario(const std::vector<uint8_t>& first_superframe, const std::vector<uint8_t>& second_superframe,
                                const std::vector<int16_t>& expected, uint64_t reset)
{
	constexpr size_t first_frames  = 600;
	constexpr size_t second_frames = 500;
	constexpr size_t drain_frames  = 512;
	constexpr size_t drained       = kAtrac9GaplessTotalFrames - first_frames - second_frames;
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (expected.size() != kAtrac9GaplessTotalFrames || first_superframe.size() != kSyntheticAtrac9Superframe ||
	    second_superframe.size() != kSyntheticAtrac9Superframe)
	{
		return 2;
	}

	GuestReadableBlock             config_storage(8);
	GuestReadableBlock             gapless_storage(8);
	GuestReadableBlock             input_storage(2 * kSyntheticAtrac9Superframe);
	GuestReadableBlock             first_output(first_frames * sizeof(int16_t));
	GuestReadableBlock             second_output(second_frames * sizeof(int16_t));
	GuestReadableBlock             drain_output(drain_frames * sizeof(int16_t));
	GuestReadableBlock             late_output(drain_frames * sizeof(int16_t));
	GuestReadableBlock             init_result(32);
	GuestReadableBlock             gapless_result(32);
	GuestReadableBlock             first_result(32);
	GuestReadableBlock             second_result(32);
	GuestReadableBlock             drain_result(32);
	GuestReadableBlock             late_result(32);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	for (const GuestReadableBlock* block: {&config_storage, &gapless_storage, &input_storage, &first_output, &second_output, &drain_output,
	                                       &late_output, &init_result, &gapless_result, &first_result, &second_result, &drain_result,
	                                       &late_result})
	{
		if (!block->IsValid())
		{
			return 3;
		}
	}
	if (!error_storage.IsValid() || !batch_id_storage.IsValid())
	{
		return 3;
	}
	auto* const input = static_cast<uint8_t*>(input_storage.Data());
	std::memcpy(input, first_superframe.data(), kSyntheticAtrac9Superframe);
	std::memcpy(input + kSyntheticAtrac9Superframe, second_superframe.data(), kSyntheticAtrac9Superframe);
	std::memcpy(config_storage.Data(), kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(static_cast<uint8_t*>(config_storage.Data()) + sizeof(kSyntheticAtrac9Config), 0, 4);
	// Total output frames (LE32), priming frames (LE16), already skipped frames (LE16).
	const uint8_t gapless_config[8] = {static_cast<uint8_t>(kAtrac9GaplessTotalFrames), static_cast<uint8_t>(kAtrac9GaplessTotalFrames >> 8u),
	                                   0,
	                                   0,
	                                   static_cast<uint8_t>(kAtrac9GaplessPrimingFrames),
	                                   static_cast<uint8_t>(kAtrac9GaplessPrimingFrames >> 8u),
	                                   0,
	                                   0};
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	for (const GuestReadableBlock* block: {&init_result, &gapless_result, &first_result, &second_result, &drain_result, &late_result})
	{
		std::memset(block->Data(), 0xa5, 32);
	}
	std::memset(first_output.Data(), 0xa5, first_frames * sizeof(int16_t));
	std::memset(second_output.Data(), 0xa5, second_frames * sizeof(int16_t));
	std::memset(drain_output.Data(), 0xa5, drain_frames * sizeof(int16_t));
	std::memset(late_output.Data(), 0xa5, drain_frames * sizeof(int16_t));

	uint32_t context  = 0;
	uint32_t instance = 0;
	if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 1, 0) != 0 ||
	    Ajm::AjmInstanceCreate(context, 1, 0x401, &instance) != 0)
	{
		return 4;
	}
	const auto run_batch = [&](uint8_t* batch)
	{
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		return Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()) == 0 && *batch_id_storage.Data() != 0 &&
		       error_storage.Data()->error_code == 0 &&
		       Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()) == 0;
	};

	alignas(uint64_t) uint8_t first_batch[256] {};
	if (Ajm::AjmBatchJobInitialize(first_batch, instance, config_storage.Data(), 8, init_result.Data()) != 0 ||
	    Ajm::AjmBatchJobSetGaplessDecode(first_batch, instance, gapless_storage.Data(), reset, gapless_result.Data()) != 0 ||
	    Ajm::AjmBatchJobDecode(first_batch, instance, input, 2 * kSyntheticAtrac9Superframe, first_output.Data(),
	                           first_frames * sizeof(int16_t), first_result.Data()) != 0)
	{
		return 5;
	}
	std::memset(gapless_storage.Data(), 0xa5, 8); // The setter snapshots its configuration when queued.
	if (!run_batch(first_batch))
	{
		return 6;
	}
	// Only the first superframe is consumed: its 768 frames after priming exceed the 600-frame output.
	if (!IsAjmControlResult(init_result.Data()) || !IsAjmControlResult(gapless_result.Data()) ||
	    !IsAjmDecodeResult(first_result.Data(), static_cast<int32_t>(kSyntheticAtrac9Superframe),
	                       static_cast<int32_t>(first_frames * sizeof(int16_t)), first_frames) ||
	    std::memcmp(first_output.Data(), expected.data(), first_frames * sizeof(int16_t)) != 0)
	{
		return 7;
	}

	alignas(uint64_t) uint8_t second_batch[256] {};
	if (Ajm::AjmBatchJobDecode(second_batch, instance, input + kSyntheticAtrac9Superframe, kSyntheticAtrac9Superframe,
	                           second_output.Data(), second_frames * sizeof(int16_t), second_result.Data()) != 0 ||
	    Ajm::AjmBatchJobDecode(second_batch, instance, nullptr, 0, drain_output.Data(), drain_frames * sizeof(int16_t),
	                           drain_result.Data()) != 0 ||
	    !run_batch(second_batch))
	{
		return 8;
	}
	if (!IsAjmDecodeResult(second_result.Data(), static_cast<int32_t>(kSyntheticAtrac9Superframe),
	                       static_cast<int32_t>(second_frames * sizeof(int16_t)), first_frames + second_frames) ||
	    std::memcmp(second_output.Data(), expected.data() + first_frames, second_frames * sizeof(int16_t)) != 0)
	{
		return 9;
	}
	// The input-free call drains the retained frames up to the output limit and leaves the rest of its buffer untouched.
	if (!IsAjmDecodeResult(drain_result.Data(), 0, static_cast<int32_t>(drained * sizeof(int16_t)), kAtrac9GaplessTotalFrames) ||
	    std::memcmp(drain_output.Data(), expected.data() + first_frames + second_frames, drained * sizeof(int16_t)) != 0 ||
	    !IsAjmBytePattern(static_cast<const uint8_t*>(drain_output.Data()) + drained * sizeof(int16_t),
	                      (drain_frames - drained) * sizeof(int16_t), 0xa5))
	{
		return 10;
	}

	// After the limit no frame is produced and no superframe is consumed (host qualification, not a captured result).
	alignas(uint64_t) uint8_t late_batch[64] {};
	if (Ajm::AjmBatchJobDecode(late_batch, instance, input, kSyntheticAtrac9Superframe, late_output.Data(), drain_frames * sizeof(int16_t),
	                           late_result.Data()) != 0 ||
	    !run_batch(late_batch))
	{
		return 11;
	}
	if (!IsAjmDecodeResult(late_result.Data(), 0, 0, kAtrac9GaplessTotalFrames) ||
	    !IsAjmBytePattern(late_output.Data(), drain_frames * sizeof(int16_t), 0xa5))
	{
		return 12;
	}

	// A reset reconfigures the decoded instance: priming is trimmed again and output resumes up to the new limit.
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	std::memset(gapless_result.Data(), 0xa5, 32);
	std::memset(late_result.Data(), 0xa5, 32);
	alignas(uint64_t) uint8_t reset_batch[128] {};
	if (Ajm::AjmBatchJobSetGaplessDecode(reset_batch, instance, gapless_storage.Data(), 1, gapless_result.Data()) != 0 ||
	    Ajm::AjmBatchJobDecode(reset_batch, instance, input, kSyntheticAtrac9Superframe, late_output.Data(), drain_frames * sizeof(int16_t),
	                           late_result.Data()) != 0 ||
	    !run_batch(reset_batch))
	{
		return 14;
	}
	if (!IsAjmControlResult(gapless_result.Data()) ||
	    !IsAjmDecodeResult(late_result.Data(), static_cast<int32_t>(kSyntheticAtrac9Superframe),
	                       static_cast<int32_t>(drain_frames * sizeof(int16_t)), kAtrac9GaplessTotalFrames + drain_frames))
	{
		return 15;
	}
	if (Ajm::AjmInstanceDestroy(context, instance) != 0 || Ajm::AjmFinalize(context) != 0)
	{
		return 13;
	}
	return 0;
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

TEST(EmulatorAudio, AjmAtrac9DecodeUsesPendingPcmAndPublishesOnlyObservedResultPrefix)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);

	GuestReadableBlock             config_storage(8);
	GuestReadableBlock             input_storage(kSyntheticAtrac9Superframe);
	GuestReadableBlock             first_output_storage(512 * sizeof(int16_t));
	GuestReadableBlock             second_output_storage(512 * sizeof(int16_t));
	GuestReadableBlock             init_result_storage(32);
	GuestReadableBlock             first_result_storage(32);
	GuestReadableBlock             second_result_storage(32);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(first_output_storage.IsValid());
	ASSERT_TRUE(second_output_storage.IsValid());
	ASSERT_TRUE(init_result_storage.IsValid());
	ASSERT_TRUE(first_result_storage.IsValid());
	ASSERT_TRUE(second_result_storage.IsValid());
	ASSERT_TRUE(error_storage.IsValid());
	ASSERT_TRUE(batch_id_storage.IsValid());

	auto* const config = static_cast<uint8_t*>(config_storage.Data());
	std::memcpy(config, kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(config + sizeof(kSyntheticAtrac9Config), 0, 4);
	std::memset(input_storage.Data(), 0xa5, kSyntheticAtrac9Superframe);
	std::memset(first_output_storage.Data(), 0xa5, 512 * sizeof(int16_t));
	std::memset(second_output_storage.Data(), 0xa5, 512 * sizeof(int16_t));
	std::memset(init_result_storage.Data(), 0xa5, 32);
	std::memset(first_result_storage.Data(), 0xa5, 32);
	std::memset(second_result_storage.Data(), 0xa5, 32);
	*error_storage.Data()    = {};
	*batch_id_storage.Data() = 0;

	alignas(uint64_t) uint8_t batch[64] {};
	ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, instance, config, 8, init_result_storage.Data()), 0);
	std::memset(config, 0, 8); // Initialize snapshots its config when queued.
	ASSERT_EQ(Ajm::AjmBatchJobDecode(batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe, first_output_storage.Data(),
	                                 512 * sizeof(int16_t), first_result_storage.Data()),
	          0);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(batch, instance, nullptr, 0, second_output_storage.Data(), 512 * sizeof(int16_t),
	                                 second_result_storage.Data()),
	          0);

	const auto signal_superframe = BuildAtrac9Superframe(0x1ffu);
	ASSERT_EQ(signal_superframe.size(), kSyntheticAtrac9Superframe);
	std::memcpy(input_storage.Data(), signal_superframe.data(), signal_superframe.size()); // Decode snapshots input at Start.
	EXPECT_TRUE(IsAjmBytePattern(init_result_storage.Data(), 32, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(first_result_storage.Data(), 32, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(second_result_storage.Data(), 32, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(first_output_storage.Data(), 512 * sizeof(int16_t), 0xa5));

	ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	EXPECT_NE(*batch_id_storage.Data(), 0u);
	EXPECT_EQ(error_storage.Data()->error_code, 0);
	EXPECT_TRUE(IsAjmBytePattern(init_result_storage.Data(), 8, 0));
	EXPECT_TRUE(IsAjmBytePattern(static_cast<uint8_t*>(init_result_storage.Data()) + 8, 24, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(static_cast<uint8_t*>(first_result_storage.Data()) + 24, 8, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(static_cast<uint8_t*>(second_result_storage.Data()) + 24, 8, 0xa5));

	const auto* first_result             = static_cast<const uint8_t*>(first_result_storage.Data());
	const auto* second_result            = static_cast<const uint8_t*>(second_result_storage.Data());
	int32_t     first_status             = 0;
	int32_t     first_codec_result       = 0;
	int32_t     first_input_consumed     = 0;
	int32_t     first_output_produced    = 0;
	uint64_t    first_samples_delivered  = 0;
	int32_t     second_input_consumed    = 0;
	int32_t     second_output_produced   = 0;
	uint64_t    second_samples_delivered = 0;
	std::memcpy(&first_status, first_result, sizeof(first_status));
	std::memcpy(&first_codec_result, first_result + 4, sizeof(first_codec_result));
	std::memcpy(&first_input_consumed, first_result + 8, sizeof(first_input_consumed));
	std::memcpy(&first_output_produced, first_result + 12, sizeof(first_output_produced));
	std::memcpy(&first_samples_delivered, first_result + 16, sizeof(first_samples_delivered));
	std::memcpy(&second_input_consumed, second_result + 8, sizeof(second_input_consumed));
	std::memcpy(&second_output_produced, second_result + 12, sizeof(second_output_produced));
	std::memcpy(&second_samples_delivered, second_result + 16, sizeof(second_samples_delivered));
	EXPECT_EQ(first_status, 0);
	EXPECT_EQ(first_codec_result, 0);
	EXPECT_EQ(first_input_consumed, static_cast<int32_t>(kSyntheticAtrac9Superframe));
	EXPECT_EQ(first_output_produced, 512 * static_cast<int32_t>(sizeof(int16_t)));
	EXPECT_EQ(first_samples_delivered, 512u);
	EXPECT_EQ(second_input_consumed, 0);
	EXPECT_EQ(second_output_produced, 512 * static_cast<int32_t>(sizeof(int16_t)));
	EXPECT_EQ(second_samples_delivered, 1024u);
	const auto* first_pcm  = static_cast<const int16_t*>(first_output_storage.Data());
	const auto* second_pcm = static_cast<const int16_t*>(second_output_storage.Data());
	EXPECT_NE(std::count(first_pcm, first_pcm + 512, 0), 512);
	EXPECT_NE(std::count(second_pcm, second_pcm + 512, 0), 512);

	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);
	alignas(uint64_t) uint8_t pending_decode_batch[32] {};
	std::memset(first_output_storage.Data(), 0xa5, 512 * sizeof(int16_t));
	std::memset(first_result_storage.Data(), 0xa5, 32);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(pending_decode_batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe,
	                                 first_output_storage.Data(), 512 * sizeof(int16_t), first_result_storage.Data()),
	          0);
	ASSERT_EQ(Ajm::AjmBatchStart(context, pending_decode_batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	std::memcpy(&first_samples_delivered, static_cast<const uint8_t*>(first_result_storage.Data()) + 16, sizeof(first_samples_delivered));
	EXPECT_EQ(first_samples_delivered, 1536u);
	std::memcpy(&first_input_consumed, static_cast<const uint8_t*>(first_result_storage.Data()) + 8, sizeof(first_input_consumed));
	EXPECT_EQ(first_input_consumed, static_cast<int32_t>(kSyntheticAtrac9Superframe));
	std::memcpy(&first_output_produced, static_cast<const uint8_t*>(first_result_storage.Data()) + 12, sizeof(first_output_produced));
	EXPECT_EQ(first_output_produced, 512 * static_cast<int32_t>(sizeof(int16_t)));
	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);

	alignas(uint64_t) uint8_t clear_batch[32] {};
	GuestReadableBlock        clear_result_storage(32);
	ASSERT_TRUE(clear_result_storage.IsValid());
	std::memset(clear_result_storage.Data(), 0xa5, 32);
	ASSERT_EQ(Ajm::AjmBatchJobClearContext(clear_batch, instance, clear_result_storage.Data()), 0);
	EXPECT_TRUE(IsAjmBytePattern(clear_result_storage.Data(), 32, 0xa5));
	ASSERT_EQ(Ajm::AjmBatchStart(context, clear_batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	EXPECT_TRUE(IsAjmBytePattern(clear_result_storage.Data(), 8, 0));
	EXPECT_TRUE(IsAjmBytePattern(static_cast<uint8_t*>(clear_result_storage.Data()) + 8, 24, 0xa5));
	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);

	alignas(uint64_t) uint8_t clear_decode_batch[64] {};
	std::memset(first_output_storage.Data(), 0xa5, 512 * sizeof(int16_t));
	std::memset(first_result_storage.Data(), 0xa5, 32);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(clear_decode_batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe,
	                                 first_output_storage.Data(), 512 * sizeof(int16_t), first_result_storage.Data()),
	          0);
	ASSERT_EQ(Ajm::AjmBatchStart(context, clear_decode_batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	std::memcpy(&first_samples_delivered, static_cast<const uint8_t*>(first_result_storage.Data()) + 16, sizeof(first_samples_delivered));
	EXPECT_EQ(first_samples_delivered, 512u);
	std::memcpy(&first_input_consumed, static_cast<const uint8_t*>(first_result_storage.Data()) + 8, sizeof(first_input_consumed));
	EXPECT_EQ(first_input_consumed, static_cast<int32_t>(kSyntheticAtrac9Superframe));
	std::memcpy(&first_output_produced, static_cast<const uint8_t*>(first_result_storage.Data()) + 12, sizeof(first_output_produced));
	EXPECT_EQ(first_output_produced, 512 * static_cast<int32_t>(sizeof(int16_t)));
	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);

	alignas(uint64_t) uint8_t invalid_output_batch[32] {};
	std::memset(first_result_storage.Data(), 0xa5, 32);
	std::memset(error_storage.Data(), 0xa5, sizeof(Ajm::AjmBatchError));
	*batch_id_storage.Data() = 0xa5a5a5a5u;
	ASSERT_EQ(Ajm::AjmBatchJobDecode(invalid_output_batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe,
	                                 reinterpret_cast<void*>(static_cast<uintptr_t>(1)), sizeof(int16_t), first_result_storage.Data()),
	          0);
	EXPECT_EQ(Ajm::AjmBatchStart(context, invalid_output_batch, 0, error_storage.Data(), batch_id_storage.Data()),
	          static_cast<int32_t>(0x80930005u));
	EXPECT_TRUE(IsAjmBytePattern(first_result_storage.Data(), 32, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(error_storage.Data(), sizeof(Ajm::AjmBatchError), 0xa5));
	EXPECT_EQ(*batch_id_storage.Data(), 0xa5a5a5a5u);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, AjmAtrac9ResampleEnabledInstanceDecodesDirectAndSplitJobsThroughTheNeutralProfile)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	uint32_t context  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	// Revision 1 with the resample flag, as a shipped title creates it.
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x1001, &instance), 0);

	GuestReadableBlock             config_storage(8);
	GuestReadableBlock             input_storage(kSyntheticAtrac9Superframe);
	GuestReadableBlock             output_storage(512 * sizeof(int16_t));
	GuestReadableBlock             results_storage(32 * 4);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_TRUE(results_storage.IsValid());
	ASSERT_TRUE(error_storage.IsValid());
	ASSERT_TRUE(batch_id_storage.IsValid());
	auto* const config = static_cast<uint8_t*>(config_storage.Data());
	std::memcpy(config, kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	// A shipped title passes 0x20 after the config bytes.
	const uint32_t trailing_word = 0x20;
	std::memcpy(config + sizeof(kSyntheticAtrac9Config), &trailing_word, sizeof(trailing_word));
	const auto signal_superframe = BuildAtrac9Superframe(0x1ffu);
	ASSERT_EQ(signal_superframe.size(), kSyntheticAtrac9Superframe);
	std::memcpy(input_storage.Data(), signal_superframe.data(), signal_superframe.size());
	auto* const results = static_cast<uint8_t*>(results_storage.Data());
	std::memset(results, 0xa5, 32 * 4);
	*error_storage.Data()    = {};
	*batch_id_storage.Data() = 0;

	alignas(uint64_t) uint8_t batch[128] {};
	ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, instance, config, 8, results), 0);
	ASSERT_EQ(Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 0, results + 32), 0);
	ASSERT_EQ(Ajm::AjmBatchJobGetResampleInfo(batch, instance, results + 64), 0);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe, output_storage.Data(),
	                                 512 * sizeof(int16_t), results + 96),
	          0);
	ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	EXPECT_EQ(error_storage.Data()->error_code, 0);
	float reported_ratio = 0.0f;
	std::memcpy(&reported_ratio, results + 64 + 8, sizeof(reported_ratio));
	EXPECT_EQ(reported_ratio, 1.0f);
	int32_t output_produced = 0;
	std::memcpy(&output_produced, results + 96 + 12, sizeof(output_produced));
	EXPECT_EQ(output_produced, 512 * static_cast<int32_t>(sizeof(int16_t)));
	const auto* pcm = static_cast<const int16_t*>(output_storage.Data());
	EXPECT_NE(std::count(pcm, pcm + 512, 0), 512);
	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);

	// A split job is the same decode with its input gathered and its PCM scattered, in descriptor order.
	GuestReadableBlock split_output_storage(512 * sizeof(int16_t));
	GuestReadableBlock descriptors_storage(sizeof(Ajm::AjmBuffer) * 4);
	ASSERT_TRUE(split_output_storage.IsValid());
	ASSERT_TRUE(descriptors_storage.IsValid());
	auto* const input        = static_cast<uint8_t*>(input_storage.Data());
	auto* const split_output = static_cast<uint8_t*>(split_output_storage.Data());
	auto* const descriptors  = static_cast<Ajm::AjmBuffer*>(descriptors_storage.Data());
	descriptors[0]           = {input, kSyntheticAtrac9Superframe / 2};
	descriptors[1]           = {input + kSyntheticAtrac9Superframe / 2, kSyntheticAtrac9Superframe - kSyntheticAtrac9Superframe / 2};
	descriptors[2]           = {split_output, 200 * sizeof(int16_t)};
	descriptors[3]           = {split_output + 200 * sizeof(int16_t), 312 * sizeof(int16_t)};
	std::memset(split_output, 0xa5, 512 * sizeof(int16_t));
	alignas(uint64_t) uint8_t split_batch[128] {};
	ASSERT_EQ(Ajm::AjmBatchJobClearContext(split_batch, instance, results), 0);
	// The guest restarts a stream on a reused instance with the reset marker after Clear.
	ASSERT_EQ(Ajm::AjmBatchJobSetResampleParametersEx(split_batch, instance, 1.0f, 0.0f, 1, results + 32), 0);
	ASSERT_EQ(Ajm::AjmBatchJobDecodeSplit(split_batch, instance, descriptors, 2, descriptors + 2, 2, results + 96), 0);
	ASSERT_EQ(Ajm::AjmBatchStart(context, split_batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
	EXPECT_EQ(std::memcmp(split_output, pcm, 512 * sizeof(int16_t)), 0);
	ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, AjmAtrac9ResamplesAFiniteStreamAtTheGuestRatio)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	constexpr uint32_t kSourceFrames = kSyntheticAtrac9Superframe;
	constexpr uint32_t kOutputFrames = 2 * kSourceFrames + 64;
	GuestReadableBlock             config_storage(8);
	GuestReadableBlock             gapless_storage(8);
	GuestReadableBlock             input_storage(kSyntheticAtrac9Superframe);
	GuestReadableBlock             neutral_output(kSourceFrames * sizeof(int16_t));
	GuestReadableBlock             resampled_output(kOutputFrames * sizeof(int16_t));
	GuestReadableBlock             results_storage(32 * 5);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	for (const GuestReadableBlock* block: {&config_storage, &gapless_storage, &input_storage, &neutral_output, &resampled_output, &results_storage})
	{
		ASSERT_TRUE(block->IsValid());
	}
	ASSERT_TRUE(error_storage.IsValid());
	ASSERT_TRUE(batch_id_storage.IsValid());
	std::memcpy(config_storage.Data(), kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(static_cast<uint8_t*>(config_storage.Data()) + sizeof(kSyntheticAtrac9Config), 0, 4);
	const auto superframe = BuildAtrac9Superframe(0x1ffu);
	ASSERT_EQ(superframe.size(), kSyntheticAtrac9Superframe);
	std::memcpy(input_storage.Data(), superframe.data(), superframe.size());
	// A finite stream of exactly one superframe: total output frames (LE32), no priming, nothing skipped.
	const uint8_t gapless_config[8] = {static_cast<uint8_t>(kSourceFrames), static_cast<uint8_t>(kSourceFrames >> 8u), 0, 0, 0, 0, 0, 0};
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	auto* const results = static_cast<uint8_t*>(results_storage.Data());
	std::memset(results, 0xa5, 32 * 5);
	std::memset(resampled_output.Data(), 0xa5, kOutputFrames * sizeof(int16_t));

	uint32_t context  = 0;
	uint32_t neutral  = 0;
	uint32_t instance = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x1001, &neutral), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x1001, &instance), 0);
	const auto run_batch = [&](uint8_t* batch)
	{
		*batch_id_storage.Data() = 0;
		*error_storage.Data()    = {};
		ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
		EXPECT_EQ(error_storage.Data()->error_code, 0);
		ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);
	};

	alignas(uint64_t) uint8_t neutral_batch[128] {};
	ASSERT_EQ(Ajm::AjmBatchJobInitialize(neutral_batch, neutral, config_storage.Data(), 8, results), 0);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(neutral_batch, neutral, input_storage.Data(), kSyntheticAtrac9Superframe, neutral_output.Data(),
	                                 kSourceFrames * sizeof(int16_t), results + 32),
	          0);
	run_batch(neutral_batch);
	EXPECT_TRUE(IsAjmDecodeResult(results + 32, kSyntheticAtrac9Superframe, kSourceFrames * sizeof(int16_t), kSourceFrames));

	// At ratio 0.5 every source frame gives two output frames: the frame itself, then the midpoint to the next one. The
	// last frame of the finished stream is held, and the decode reports the source frames it consumed.
	alignas(uint64_t) uint8_t batch[192] {};
	ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, instance, config_storage.Data(), 8, results), 0);
	ASSERT_EQ(Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_storage.Data(), 0, results + 64), 0);
	ASSERT_EQ(Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 0.5f, 0.0f, 0, results + 96), 0);
	ASSERT_EQ(Ajm::AjmBatchJobDecode(batch, instance, input_storage.Data(), kSyntheticAtrac9Superframe, resampled_output.Data(),
	                                 kOutputFrames * sizeof(int16_t), results + 128),
	          0);
	run_batch(batch);
	EXPECT_TRUE(IsAjmDecodeResult(results + 128, kSyntheticAtrac9Superframe, 2 * kSourceFrames * sizeof(int16_t), kSourceFrames));
	const auto* source = static_cast<const int16_t*>(neutral_output.Data());
	const auto* output = static_cast<const int16_t*>(resampled_output.Data());
	ASSERT_NE(std::count(source, source + kSourceFrames, 0), static_cast<std::ptrdiff_t>(kSourceFrames));
	for (uint32_t frame = 0; frame < kSourceFrames; ++frame)
	{
		ASSERT_EQ(output[2 * frame], source[frame]) << frame;
		const int next = frame + 1 < kSourceFrames ? source[frame + 1] : source[frame];
		ASSERT_LE(std::abs(2 * output[2 * frame + 1] - (source[frame] + next)), 2) << frame;
	}
	EXPECT_TRUE(IsAjmBytePattern(output + 2 * kSourceFrames, 64 * sizeof(int16_t), 0xa5));

	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, neutral), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
}

TEST(EmulatorAudio, AjmAtrac9GaplessTrimsPrimingAndCapsFiniteOutputAcrossCalls)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	const auto first_superframe  = BuildAtrac9Superframe(0x1ffu);
	const auto second_superframe = BuildAtrac9Superframe(0x0c3u);
	ASSERT_NE(first_superframe, second_superframe);

	// The oracle decodes both superframes as one stream through a separate decoder and applies the AJM S16 rounding.
	const uint8_t extradata[12] = {0, 0, 0, 0, kSyntheticAtrac9Config[0], kSyntheticAtrac9Config[1], kSyntheticAtrac9Config[2],
	                               kSyntheticAtrac9Config[3], 0, 0, 0, 0};
	std::string   error;
	auto oracle = AudioVideoBackend::ElementaryAudioDecoder::Open(AudioVideoBackend::AudioCodec::Atrac9, extradata, sizeof(extradata),
	                                                              kSyntheticAtrac9Superframe, 1, kSyntheticAtrac9Rate, &error);
	ASSERT_TRUE(oracle != nullptr) << error;
	std::vector<int16_t> stream;
	for (const auto* superframe: {&first_superframe, &second_superframe})
	{
		std::vector<float> decoded;
		ASSERT_TRUE(oracle->Decode(superframe->data(), superframe->size(), &decoded)) << oracle->LastError();
		ASSERT_EQ(decoded.size(), static_cast<size_t>(kSyntheticAtrac9Superframe));
		for (const float sample: decoded)
		{
			ASSERT_TRUE(std::isfinite(sample));
			const auto rounded = std::llround(std::clamp(static_cast<double>(sample), -1.0, 1.0) * 32768.0);
			stream.push_back(static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX)));
		}
	}
	const std::vector<int16_t> expected(stream.begin() + kAtrac9GaplessPrimingFrames,
	                                    stream.begin() + kAtrac9GaplessPrimingFrames + kAtrac9GaplessTotalFrames);
	const auto first_part  = expected.begin() + (kSyntheticAtrac9Superframe - kAtrac9GaplessPrimingFrames);
	ASSERT_TRUE(std::any_of(expected.begin(), first_part, [](int16_t value) { return value != 0; }));
	ASSERT_TRUE(std::any_of(first_part, expected.end(), [](int16_t value) { return value != 0; }));
	ASSERT_FALSE(std::equal(stream.begin(), stream.begin() + kSyntheticAtrac9Superframe, stream.begin() + kSyntheticAtrac9Superframe));

	ASSERT_EXIT(std::_Exit(RunAjmAtrac9GaplessScenario(first_superframe, second_superframe, expected, 1)), ::testing::ExitedWithCode(0),
	            "");
	ASSERT_EXIT(std::_Exit(RunAjmAtrac9GaplessScenario(first_superframe, second_superframe, expected, 0)), ::testing::ExitedWithCode(0),
	            "");
#endif
}

TEST(EmulatorAudio, RejectsAjmAtrac9GaplessChangesAfterDecodeBeforeGuestPublication)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	constexpr size_t output_size = 512 * sizeof(int16_t);
	GuestReadableBlock config_storage(8);
	GuestReadableBlock gapless_storage(8);
	GuestReadableBlock input_storage(kSyntheticAtrac9Superframe);
	GuestSharedBlock   pcm_storage(output_size);
	GuestSharedBlock   result_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(gapless_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(pcm_storage.IsValid());
	ASSERT_TRUE(result_storage.IsValid());
	std::memcpy(config_storage.Data(), kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(static_cast<uint8_t*>(config_storage.Data()) + sizeof(kSyntheticAtrac9Config), 0, 4);
	const auto superframe = BuildAtrac9Superframe(0x1ffu);
	std::memcpy(input_storage.Data(), superframe.data(), superframe.size());
	void* const config_address  = config_storage.Data();
	void* const gapless_address = gapless_storage.Data();
	void* const input_address   = input_storage.Data();
	void* const pcm_address     = pcm_storage.Data();
	auto* const shared          = ::new (result_storage.Data()) AjmBatchSharedResults {};
	const auto  outputs_untouched = [shared, pcm = pcm_storage.Data()]
	{
		return IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5) && IsAjmBytePattern(pcm, output_size, 0xa5);
	};

	// A reset may reconfigure a decoded instance; without one, or with no total, the change is refused.
	enum class Scenario : uint8_t
	{
		ResetZeroAfterDecodeInBatch,
		UnlimitedTotal,
	};
	for (const Scenario scenario: {Scenario::ResetZeroAfterDecodeInBatch, Scenario::UnlimitedTotal})
	{
		SCOPED_TRACE(static_cast<int>(scenario));
		std::memset(shared, 0xa5, sizeof(*shared));
		std::memset(pcm_storage.Data(), 0xa5, output_size);
		uint8_t gapless_config[8] = {0xdc, 0x05, 0, 0, 0x00, 0x01, 0, 0}; // 1500 output frames, 256 priming frames.
		if (scenario == Scenario::UnlimitedTotal)
		{
			gapless_config[0] = 0;
			gapless_config[1] = 0;
		}
		std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
		const uint64_t reset = scenario == Scenario::ResetZeroAfterDecodeInBatch ? 0 : 1;

		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [=]
		    {
			    uint32_t                  context  = 0;
			    uint32_t                  instance = 0;
			    alignas(uint64_t) uint8_t batch[256] {};
			    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 1, 0) != 0 ||
			        Ajm::AjmInstanceCreate(context, 1, 0x401, &instance) != 0)
			    {
				    std::_Exit(91);
			    }
			    if (Ajm::AjmBatchJobInitialize(batch, instance, config_address, 8, shared->init_result) != 0)
			    {
				    std::_Exit(93);
			    }
			    const bool decode_first = scenario != Scenario::UnlimitedTotal;
			    if (decode_first && Ajm::AjmBatchJobDecode(batch, instance, input_address, kSyntheticAtrac9Superframe, pcm_address,
			                                               output_size, shared->decode_result) != 0)
			    {
				    std::_Exit(94);
			    }
			    if (Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_address, reset, shared->sideband_output) != 0 ||
			        !outputs_untouched())
			    {
				    std::_Exit(95);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d for ATRAC9 gapless scenario %u\n", start_status,
			                 static_cast<unsigned>(scenario));
			    std::_Exit(96);
		    },
		    &status))
		    << "strict AJM gapless refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}
#endif
}

TEST(EmulatorAudio, AjmAtrac9PreflightCountsPreparedDecodersAgainstLiveLimit)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "ATRAC9 backend is unavailable";
	}

	struct AjmContextCleanup
	{
		~AjmContextCleanup()
		{
			if (context == 0)
			{
				return;
			}
			for (const uint32_t instance: instances)
			{
				if (instance != 0)
				{
					(void)Ajm::AjmInstanceDestroy(context, instance);
				}
			}
			(void)Ajm::AjmFinalize(context);
		}

		uint32_t              context = 0;
		std::vector<uint32_t> instances;
	} cleanup;
	auto&              context       = cleanup.context;
	constexpr uint32_t decoder_limit = 64;
	// Eight result ranges plus error and batch ID fit within the writable guest transaction limit.
	constexpr uint32_t setup_jobs_per_batch  = 8;
	constexpr uint32_t replacement_job_limit = 32;
	ASSERT_EQ(Ajm::AjmInitialize(0, &context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(context, 1, 0), 0);

	GuestReadableBlock             config_storage(8);
	GuestReadableBlock             results_storage(decoder_limit * 32u);
	GuestReadableBlock             replacement_results_storage(replacement_job_limit * 32u);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(results_storage.IsValid());
	ASSERT_TRUE(replacement_results_storage.IsValid());
	ASSERT_TRUE(error_storage.IsValid());
	ASSERT_TRUE(batch_id_storage.IsValid());

	auto* const config = static_cast<uint8_t*>(config_storage.Data());
	std::memcpy(config, kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(config + sizeof(kSyntheticAtrac9Config), 0, 4);

	cleanup.instances.resize(decoder_limit);
	for (uint32_t& instance: cleanup.instances)
	{
		ASSERT_EQ(Ajm::AjmInstanceCreate(context, 1, 0x401, &instance), 0);
	}

	alignas(uint64_t) uint8_t batch[64] {};
	for (uint32_t first_instance = 0; first_instance < decoder_limit; first_instance += setup_jobs_per_batch)
	{
		const uint32_t end_instance = first_instance + setup_jobs_per_batch;
		for (uint32_t index = first_instance; index < end_instance; ++index)
		{
			void* const result = static_cast<uint8_t*>(results_storage.Data()) + index * 32u;
			std::memset(result, 0xa5, 32);
			ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, cleanup.instances[index], config, 8, result), 0);
		}

		ASSERT_EQ(Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()), 0);
		for (uint32_t index = first_instance; index < end_instance; ++index)
		{
			const auto* result = static_cast<const uint8_t*>(results_storage.Data()) + index * 32u;
			EXPECT_TRUE(IsAjmBytePattern(result, 8, 0));
			EXPECT_TRUE(IsAjmBytePattern(result + 8, 24, 0xa5));
		}
		ASSERT_EQ(Ajm::AjmBatchWait(context, *batch_id_storage.Data(), UINT32_MAX, error_storage.Data()), 0);
	}

	std::memset(replacement_results_storage.Data(), 0xa5, replacement_job_limit * 32u);
	std::memset(error_storage.Data(), 0xa5, sizeof(Ajm::AjmBatchError));
	*batch_id_storage.Data() = 0xa5a5a5a5u;
	for (uint32_t index = 0; index < replacement_job_limit; ++index)
	{
		void* const result = static_cast<uint8_t*>(replacement_results_storage.Data()) + index * 32u;
		ASSERT_EQ(Ajm::AjmBatchJobInitialize(batch, cleanup.instances[index], config, 8, result), 0);
	}

	EXPECT_EQ(Ajm::AjmBatchStart(context, batch, 0, error_storage.Data(), batch_id_storage.Data()), static_cast<int32_t>(0x80930007u));
	EXPECT_TRUE(IsAjmBytePattern(replacement_results_storage.Data(), replacement_job_limit * 32u, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(error_storage.Data(), sizeof(Ajm::AjmBatchError), 0xa5));
	EXPECT_EQ(*batch_id_storage.Data(), 0xa5a5a5a5u);

	for (const uint32_t instance: cleanup.instances)
	{
		ASSERT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	}
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
	cleanup.context = 0;
}

TEST(EmulatorAudio, AjmOpusSplitRetainsPartialPacketPcmAndConsumesEachRecordOnce)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t packet_bytes        = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	constexpr size_t packet_frames       = 960;
	constexpr size_t packet_count        = 4;
	constexpr size_t framed_packet_bytes = sizeof(uint16_t) + packet_bytes;
	constexpr size_t input_bytes         = packet_count * framed_packet_bytes;
	static_assert(input_bytes == 648);

	std::array<uint8_t, input_bytes> framed_input {};
	for (size_t packet = 0; packet < packet_count; ++packet)
	{
		const size_t offset      = packet * framed_packet_bytes;
		framed_input[offset]     = static_cast<uint8_t>(packet_bytes);
		framed_input[offset + 1] = static_cast<uint8_t>(packet_bytes >> 8u);
		std::memcpy(framed_input.data() + offset + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	}

	std::string decoder_error;
	auto        oracle = AudioVideoBackend::ElementaryAudioDecoder::OpenOpus(2, 48000, &decoder_error);
	ASSERT_TRUE(oracle != nullptr) << decoder_error;
	std::array<std::vector<uint8_t>, packet_count> expected_packet_pcm;
	for (size_t packet = 0; packet < packet_count; ++packet)
	{
		std::vector<float> decoded;
		ASSERT_TRUE(oracle->Decode(AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes, &decoded)) << oracle->LastError();
		ASSERT_EQ(decoded.size(), packet_frames * 2);
		auto& expected = expected_packet_pcm[packet];
		expected.reserve(decoded.size() * sizeof(int16_t));
		for (const float sample: decoded)
		{
			ASSERT_TRUE(std::isfinite(sample));
			const double bounded = std::clamp(static_cast<double>(sample), -1.0, 1.0);
			const auto   rounded = std::llround(bounded * 32768.0);
			const auto   value   = static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX));
			const auto   bits    = static_cast<uint16_t>(value);
			expected.push_back(static_cast<uint8_t>(bits));
			expected.push_back(static_cast<uint8_t>(bits >> 8u));
		}
	}
	ASSERT_EQ(expected_packet_pcm[0].size(), packet_frames * 2 * sizeof(int16_t));
	EXPECT_TRUE(std::any_of(expected_packet_pcm[0].begin(), expected_packet_pcm[0].end(), [](uint8_t value) { return value != 0; }));

	ASSERT_EXIT(std::_Exit(RunAjmOpusSplitScenario(framed_input, expected_packet_pcm)), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusGaplessFreshResetTrimsPrimingFramesAtNeutralResampleRate)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t packet_bytes        = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	constexpr size_t packet_count        = 4;
	constexpr size_t framed_packet_bytes = sizeof(uint16_t) + packet_bytes;
	constexpr size_t input_bytes         = packet_count * framed_packet_bytes;
	constexpr size_t priming_frames      = 312;
	constexpr size_t output_frames       = 2000;
	constexpr size_t output_bytes        = output_frames * 2 * sizeof(int16_t);
	static_assert(input_bytes == 648);

	std::array<uint8_t, input_bytes> framed_input {};
	for (size_t packet = 0; packet < packet_count; ++packet)
	{
		const size_t offset      = packet * framed_packet_bytes;
		framed_input[offset]     = static_cast<uint8_t>(packet_bytes);
		framed_input[offset + 1] = static_cast<uint8_t>(packet_bytes >> 8u);
		std::memcpy(framed_input.data() + offset + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	}

	std::string decoder_error;
	auto        oracle = AudioVideoBackend::ElementaryAudioDecoder::OpenOpus(2, 48000, &decoder_error);
	ASSERT_TRUE(oracle != nullptr) << decoder_error;
	std::vector<uint8_t> continuous_pcm;
	continuous_pcm.reserve(packet_count * 960 * 2 * sizeof(int16_t));
	for (size_t packet = 0; packet < packet_count; ++packet)
	{
		std::vector<float> decoded;
		ASSERT_TRUE(oracle->Decode(AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes, &decoded)) << oracle->LastError();
		ASSERT_EQ(decoded.size(), 960u * 2u);
		for (const float sample: decoded)
		{
			ASSERT_TRUE(std::isfinite(sample));
			const double bounded = std::clamp(static_cast<double>(sample), -1.0, 1.0);
			const auto   rounded = std::llround(bounded * 32768.0);
			const auto   value   = static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX));
			const auto   bits    = static_cast<uint16_t>(value);
			continuous_pcm.push_back(static_cast<uint8_t>(bits));
			continuous_pcm.push_back(static_cast<uint8_t>(bits >> 8u));
		}
	}
	const size_t trim_bytes = priming_frames * 2 * sizeof(int16_t);
	ASSERT_GE(continuous_pcm.size(), trim_bytes + output_bytes);
	std::vector<uint8_t> expected_output(continuous_pcm.begin() + static_cast<std::ptrdiff_t>(trim_bytes),
	                                     continuous_pcm.begin() + static_cast<std::ptrdiff_t>(trim_bytes + output_bytes));
	ASSERT_TRUE(std::any_of(expected_output.begin(), expected_output.end(), [](uint8_t value) { return value != 0; }));

	ASSERT_EXIT(std::_Exit(RunAjmOpusGaplessScenario(framed_input, expected_output, 0, 0)), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(std::_Exit(RunAjmOpusGaplessScenario(framed_input, expected_output, 1, 0)), ::testing::ExitedWithCode(0), "");
	// Before any decode, reset one configures the same fresh stream as reset zero.
	ASSERT_EXIT(std::_Exit(RunAjmOpusGaplessScenario(framed_input, expected_output, 0, 1)), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusGetResampleInfoReportsNeutralResampleStateAfterDecodeSplit)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t packet_bytes = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	static_assert(packet_bytes + sizeof(uint16_t) == 162);
	std::array<uint8_t, 162> framed_input {};
	framed_input[0] = static_cast<uint8_t>(packet_bytes);
	framed_input[1] = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(framed_input.data() + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);

	std::string decoder_error;
	auto        oracle = AudioVideoBackend::ElementaryAudioDecoder::OpenOpus(2, 48000, &decoder_error);
	ASSERT_TRUE(oracle != nullptr) << decoder_error;
	std::vector<float> decoded;
	ASSERT_TRUE(oracle->Decode(AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes, &decoded)) << oracle->LastError();
	ASSERT_EQ(decoded.size(), 960u * 2u);
	std::vector<uint8_t> expected_output;
	expected_output.reserve(decoded.size() * sizeof(int16_t));
	for (const float sample: decoded)
	{
		ASSERT_TRUE(std::isfinite(sample));
		const double bounded = std::clamp(static_cast<double>(sample), -1.0, 1.0);
		const auto   rounded = std::llround(bounded * 32768.0);
		const auto   value   = static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX));
		const auto   bits    = static_cast<uint16_t>(value);
		expected_output.push_back(static_cast<uint8_t>(bits));
		expected_output.push_back(static_cast<uint8_t>(bits >> 8u));
	}
	ASSERT_EQ(expected_output.size(), 960u * 2u * sizeof(int16_t));
	ASSERT_TRUE(std::any_of(expected_output.begin(), expected_output.end(), [](uint8_t value) { return value != 0; }));

	ASSERT_EXIT(std::_Exit(RunAjmOpusResampleInfoScenario(framed_input, expected_output)), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusNeutralFlag2ProfilePreservesPendingPcmAcrossBatches)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t packet_bytes = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	static_assert(packet_bytes + sizeof(uint16_t) == 162);
	std::array<uint8_t, 162> framed_input {};
	framed_input[0] = static_cast<uint8_t>(packet_bytes);
	framed_input[1] = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(framed_input.data() + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);

	std::string decoder_error;
	auto        oracle = AudioVideoBackend::ElementaryAudioDecoder::OpenOpus(2, 48000, &decoder_error);
	ASSERT_TRUE(oracle != nullptr) << decoder_error;
	std::vector<float> decoded;
	ASSERT_TRUE(oracle->Decode(AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes, &decoded)) << oracle->LastError();
	ASSERT_EQ(decoded.size(), 960u * 2u);
	std::vector<uint8_t> expected_output;
	expected_output.reserve(decoded.size() * sizeof(int16_t));
	for (const float sample: decoded)
	{
		ASSERT_TRUE(std::isfinite(sample));
		const double bounded = std::clamp(static_cast<double>(sample), -1.0, 1.0);
		const auto   rounded = std::llround(bounded * 32768.0);
		const auto   value   = static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX));
		const auto   bits    = static_cast<uint16_t>(value);
		expected_output.push_back(static_cast<uint8_t>(bits));
		expected_output.push_back(static_cast<uint8_t>(bits >> 8u));
	}
	ASSERT_TRUE(std::any_of(expected_output.begin() + 240 * 4, expected_output.end(), [](uint8_t value) { return value != 0; }));

	ASSERT_EXIT(std::_Exit(RunAjmOpusNeutralFlag2Scenario(framed_input, expected_output)), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusResampledOutputIsInvariantToGuestCallSplitsAndEndsOnTheSourceTotal)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}
	ASSERT_EXIT(std::_Exit(RunAjmOpusResampleSplitInvarianceScenario()), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusFlag2EndMarkerKeepsAStreamStartedWithTheResetMarker)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}
	ASSERT_EXIT(std::_Exit(RunAjmOpusResetMarkerStreamEndScenario()), ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, AjmOpusClearedNeutralInstanceRestartsWithoutANewResampleProfile)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}
	ASSERT_EXIT(std::_Exit(RunAjmOpusClearedNeutralRestartScenario()), ::testing::ExitedWithCode(0), "");
#endif
}

// The sample values check Kyty's host filter choice (linear interpolation with a held edge), not a native filter. Dyadic
// ratios keep the source positions exact so the expected values can be computed independently.
namespace {

// Decodes the two-record fixture stream with an independent decoder and keeps the gapless window of source frames.
void DecodeAjmOpusFixtureSource(uint32_t total_frames, uint16_t priming, std::vector<float>* source)
{
	constexpr size_t channels = 2;
	std::string      decoder_error;
	auto             oracle = AudioVideoBackend::ElementaryAudioDecoder::OpenOpus(2, 48000, &decoder_error);
	ASSERT_TRUE(oracle != nullptr) << decoder_error;
	std::vector<float> decoded;
	for (int record = 0; record < 2; ++record)
	{
		std::vector<float> packet;
		ASSERT_TRUE(oracle->Decode(AudioTestFixtures::kGeneratedOpusSinePacket.data(), AudioTestFixtures::kGeneratedOpusSinePacket.size(),
		                           &packet))
		    << oracle->LastError();
		ASSERT_EQ(packet.size(), 960u * channels);
		decoded.insert(decoded.end(), packet.begin(), packet.end());
	}
	ASSERT_GE(decoded.size(), (priming + total_frames) * channels);
	source->assign(decoded.begin() + priming * channels, decoded.begin() + (priming + total_frames) * channels);
}

// Independent model of the resampled stream: each output frame interpolates linearly at the source position, then the
// position advances by the ratio, which moves by the change per sample from the last setter. A is the furthest source
// frame read, C the frames at or after the position, and the result ratio the current one.
void BuildAjmResampleOracle(const std::vector<float>& source, uint32_t total_frames, uint16_t priming,
                            const std::vector<AjmResampleCall>& calls, std::vector<uint8_t>* expected_pcm,
                            std::vector<AjmResampleCallResult>* expected_results)
{
	constexpr size_t channels       = 2;
	double           position       = 0.0;
	uint64_t         admitted       = 0;
	uint64_t         decoded_frames = 0;
	float            anchor         = 1.0f;
	float            change         = 0.0f;
	uint64_t         profile_frames = 0;
	for (const auto& call: calls)
	{
		if (call.send_setter)
		{
			anchor         = call.ratio;
			change         = call.change;
			profile_frames = 0;
		}
		uint32_t frames = 0;
		for (; frames < call.output_frames && position < total_frames; ++frames)
		{
			const auto   index    = static_cast<size_t>(position);
			const double fraction = position - static_cast<double>(index);
			const bool   next     = fraction != 0.0 && index + 1 < total_frames;
			admitted              = std::max<uint64_t>(admitted, index + (fraction != 0.0 ? 2u : 1u));
			for (size_t channel = 0; channel < channels; ++channel)
			{
				const double a     = source[index * channels + channel];
				const double b     = next ? source[(index + 1) * channels + channel] : a;
				const auto   value = static_cast<float>(fraction == 0.0 ? a : a + fraction * (b - a));
				const auto   bits  = static_cast<uint16_t>(static_cast<int16_t>(
                    std::clamp<long long>(std::llround(std::clamp(static_cast<double>(value), -1.0, 1.0) * 32768.0), INT16_MIN, INT16_MAX)));
				expected_pcm->push_back(static_cast<uint8_t>(bits));
				expected_pcm->push_back(static_cast<uint8_t>(bits >> 8u));
			}
			position += static_cast<double>(anchor) + static_cast<double>(change) * static_cast<double>(profile_frames);
			++profile_frames;
		}
		admitted                     = std::min<uint64_t>(admitted, total_frames);
		const uint64_t needed_frames = admitted == 0 ? 0 : priming + admitted;
		const uint64_t packets       = (needed_frames + 959) / 960;
		AjmResampleCallResult result {};
		result.input_consumed  = static_cast<int32_t>((packets - decoded_frames) * AJM_OPUS_FIXTURE_RECORD_SIZE);
		decoded_frames         = packets;
		result.output_bytes    = static_cast<int32_t>(frames * channels * sizeof(int16_t));
		result.admitted_frames = admitted;
		result.held_frames     = static_cast<int32_t>(admitted - std::min<uint64_t>(admitted, static_cast<uint64_t>(position)));
		result.info_ratio =
		    static_cast<float>(static_cast<double>(anchor) + static_cast<double>(change) * static_cast<double>(profile_frames));
		expected_results->push_back(result);
	}
}

} // namespace

TEST(EmulatorAudio, AjmOpusResamplerInterpolatesLinearlyAcrossRatioChangesBetweenBatches)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr uint32_t total_frames = 1500;
	constexpr uint16_t priming      = 312;
	std::vector<float> source;
	ASSERT_NO_FATAL_FAILURE(DecodeAjmOpusFixtureSource(total_frames, priming, &source));

	const std::vector<AjmResampleCall> calls = {{0.9375f, 700}, {1.25f, 4096}, {1.25f, 64}};
	std::vector<uint8_t>               expected_pcm;
	std::vector<AjmResampleCallResult> expected_results;
	BuildAjmResampleOracle(source, total_frames, priming, calls, &expected_pcm, &expected_results);
	ASSERT_EQ(expected_results[0].output_bytes, 700 * 4);
	ASSERT_NE(expected_results[0].held_frames, 0);
	ASSERT_EQ(expected_results[1].admitted_frames, total_frames);
	ASSERT_EQ(expected_results[1].held_frames, 0);
	ASSERT_EQ(expected_results[2].output_bytes, 0);

	ASSERT_EXIT(std::_Exit(RunAjmOpusResampleValueScenario(calls, total_frames, priming, expected_pcm, expected_results)),
	            ::testing::ExitedWithCode(0), "");
#endif
}


TEST(EmulatorAudio, AjmOpusResamplerMovesTheRatioPerOutputFrameAcrossSetterlessCalls)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr uint32_t total_frames = 1500;
	constexpr uint16_t priming      = 312;
	std::vector<float> source;
	ASSERT_NO_FATAL_FAILURE(DecodeAjmOpusFixtureSource(total_frames, priming, &source));

	// Like the guest: a ramp from ratio 0 towards 1 over 3072 output frames, a call without a setter while the ratio and
	// change are unchanged, then a setter re-anchored on the reported ratio with (target - current) / frames.
	constexpr float ramp = 0x1.555556p-12f;
	const float     ramp_end_ratio =
	    static_cast<float>(static_cast<double>(ramp) * 2024.0);
	const float                  settle = (1.0f - ramp_end_ratio) / 3072.0f;
	std::vector<AjmResampleCall> calls  = {{0.0f, 1024, 0, false, ramp},
	                                       {0.0f, 1000, 0, false, ramp, false},
	                                       {ramp_end_ratio, 4096, 0, false, settle},
	                                       {ramp_end_ratio, 64, 0, false, settle, false}};
	std::vector<uint8_t>               expected_pcm;
	std::vector<AjmResampleCallResult> expected_results;
	BuildAjmResampleOracle(source, total_frames, priming, calls, &expected_pcm, &expected_results);
	ASSERT_EQ(expected_results[0].info_ratio, static_cast<float>(static_cast<double>(ramp) * 1024.0));
	ASSERT_EQ(expected_results[1].info_ratio, ramp_end_ratio);
	ASSERT_EQ(expected_results[1].output_bytes, 1000 * 4);
	ASSERT_LT(expected_results[1].admitted_frames, total_frames);
	ASSERT_EQ(expected_results[2].admitted_frames, total_frames);
	ASSERT_EQ(expected_results[2].held_frames, 0);
	ASSERT_EQ(expected_results[3].output_bytes, 0);

	ASSERT_EXIT(std::_Exit(RunAjmOpusResampleValueScenario(calls, total_frames, priming, expected_pcm, expected_results)),
	            ::testing::ExitedWithCode(0), "");
#endif
}

TEST(EmulatorAudio, RejectsUnobservedAjmOpusResampleProfilesAtomically)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	GuestReadableBlock config_storage(12);
	GuestReadableBlock gapless_storage(8);
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(gapless_storage.IsValid());
	constexpr uint8_t opus_config[12]   = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	constexpr uint8_t gapless_config[8] = {0xd0, 0x07, 0, 0, 0x38, 0x01, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	void* const config_address  = config_storage.Data();
	void* const gapless_address = gapless_storage.Data();

	GuestSharedBlock result_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(result_storage.IsValid());
	auto* const shared            = ::new (result_storage.Data()) AjmBatchSharedResults {};
	const auto  outputs_untouched = [shared]
	{
		return IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5) &&
		       IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5);
	};

	struct Profile
	{
		const char* name;
		bool        gapless;
		float       running_ratio;
		float       ratio;
		float       delta;
		uint32_t    flags;
	};
	const Profile profiles[] = {
	    {"non-unity ratio without gapless", false, 0.0f, 0.9375f, 0.0f, 0},
	    {"reset marker at non-unity ratio", true, 0.0f, 0.9375f, 0.0f, 1},
	    {"flags 2 changing a non-unity ratio", true, 0.9375f, 0.875f, 0.0f, 2},
	    {"flags 2 after a non-unity profile", true, 0.9375f, 1.0f, 0.0f, 2},
	    {"moving ratio without gapless", false, 0.0f, 1.0f, 0x1p-20f, 0},
	    {"reset marker on a moving ratio", true, 0.0f, 1.0f, 0x1p-20f, 1},
	    {"zero ratio moving down", true, 0.0f, 0.0f, -0x1p-20f, 0},
	    {"zero ratio", true, 0.0f, 0.0f, 0.0f, 0},
	    {"negative ratio", true, 0.0f, -1.0f, 0.0f, 0},
	};
	for (const Profile& profile: profiles)
	{
		SCOPED_TRACE(profile.name);
		std::memset(shared, 0xa5, sizeof(*shared));
		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [=]
		    {
			    uint32_t                  context  = 0;
			    uint32_t                  instance = 0;
			    alignas(uint64_t) uint8_t batch[256] {};
			    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
			        Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 ||
			        Ajm::AjmBatchJobInitialize(batch, instance, config_address, sizeof(opus_config), shared->init_result) != 0 ||
			        Ajm::AjmBatchJobClearContext(batch, instance, nullptr) != 0)
			    {
				    std::_Exit(90);
			    }
			    if (profile.gapless && Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_address, 0, shared->sideband_output) != 0)
			    {
				    std::_Exit(91);
			    }
			    if (profile.running_ratio != 0.0f &&
			        Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, profile.running_ratio, 0.0f, 0, shared->sideband_output + 8) != 0)
			    {
				    std::_Exit(92);
			    }
			    if (Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, profile.ratio, profile.delta, profile.flags,
			                                                shared->sideband_output + 16) != 0 ||
			        !outputs_untouched())
			    {
				    std::_Exit(93);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d for resample profile %s\n", start_status, profile.name);
			    std::_Exit(96);
		    },
		    &status))
		    << "strict AJM resample refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}
#endif
}

TEST(EmulatorAudio, RejectsAjmResampleUseWithoutAnAdmittedProfileAtomically)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t   packet_bytes = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	GuestReadableBlock config_storage(12);
	GuestReadableBlock gapless_storage(8);
	GuestReadableBlock input_storage(sizeof(uint16_t) + packet_bytes);
	GuestReadableBlock descriptors_storage(2 * sizeof(Ajm::AjmBuffer));
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(gapless_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(descriptors_storage.IsValid());
	constexpr uint8_t opus_config[12]   = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	constexpr uint8_t gapless_config[8] = {0xd0, 0x07, 0, 0, 0x38, 0x01, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	auto* const framed = static_cast<uint8_t*>(input_storage.Data());
	framed[0]          = static_cast<uint8_t>(packet_bytes);
	framed[1]          = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(framed + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	void* const config_address  = config_storage.Data();
	void* const gapless_address = gapless_storage.Data();
	auto* const descriptors     = static_cast<Ajm::AjmBuffer*>(descriptors_storage.Data());

	GuestSharedBlock result_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(result_storage.IsValid());
	auto* const shared = ::new (result_storage.Data()) AjmBatchSharedResults {};
	std::memset(shared, 0xa5, sizeof(*shared));
	const auto outputs_untouched = [shared]
	{
		return IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5) &&
		       IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5);
	};

	// A cleared ratio-1 instance keeps its profile; whether a non-unity one survives Clear is not observed.
	enum class Scenario : uint8_t
	{
		InfoBeforeAnyProfile,
		DecodeAfterClearingNonUnityProfile,
	};
	const Scenario scenarios[] = {Scenario::InfoBeforeAnyProfile, Scenario::DecodeAfterClearingNonUnityProfile};
	for (const Scenario scenario: scenarios)
	{
		SCOPED_TRACE(static_cast<int>(scenario));
		std::memset(shared, 0xa5, sizeof(*shared));
		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [=]
		    {
			    uint32_t                  context  = 0;
			    uint32_t                  instance = 0;
			    alignas(uint64_t) uint8_t batch[512] {};
			    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
			        Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 ||
			        Ajm::AjmBatchJobInitialize(batch, instance, config_address, sizeof(opus_config), shared->init_result) != 0)
			    {
				    std::_Exit(90);
			    }
			    if (scenario == Scenario::DecodeAfterClearingNonUnityProfile)
			    {
				    descriptors[0] = {framed, sizeof(uint16_t) + packet_bytes};
				    descriptors[1] = {shared->data_output, sizeof(shared->data_output)};
				    if (Ajm::AjmBatchJobClearContext(batch, instance, nullptr) != 0 ||
				        Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_address, 0, nullptr) != 0 ||
				        Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 0.9375f, 0.0f, 0, shared->sideband_output) != 0 ||
				        Ajm::AjmBatchJobClearContext(batch, instance, nullptr) != 0 ||
				        Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_address, 0, nullptr) != 0 ||
				        Ajm::AjmBatchJobDecodeSplit(batch, instance, descriptors, 1, descriptors + 1, 1, shared->decode_result) != 0)
				    {
					    std::_Exit(91);
				    }
			    }
			    if ((scenario == Scenario::InfoBeforeAnyProfile &&
			         Ajm::AjmBatchJobGetResampleInfo(batch, instance, shared->decode_result) != 0) ||
			        !outputs_untouched())
			    {
				    std::_Exit(92);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d (0x%08x) for resample-info scenario %u\n", start_status,
			                 static_cast<uint32_t>(start_status), static_cast<unsigned>(scenario));
			    std::_Exit(96);
		    },
		    &status))
		    << "strict AJM resample-info refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}
#endif
}

TEST(EmulatorAudio, RejectsUnsupportedAjmGaplessProfilesBeforeGuestPublication)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t   packet_bytes = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	constexpr size_t   packet_size  = sizeof(uint16_t) + packet_bytes;
	constexpr size_t   output_size  = 960 * 2 * sizeof(int16_t);
	GuestReadableBlock config_storage(12);
	GuestReadableBlock gapless_storage(8);
	GuestReadableBlock input_storage(packet_size);
	GuestReadableBlock input_descriptor_storage(sizeof(Ajm::AjmBuffer));
	GuestReadableBlock output_descriptor_storage(sizeof(Ajm::AjmBuffer));
	GuestSharedBlock   pcm_storage(output_size);
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(gapless_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(input_descriptor_storage.IsValid());
	ASSERT_TRUE(output_descriptor_storage.IsValid());
	ASSERT_TRUE(pcm_storage.IsValid());

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	void* const config_address         = config_storage.Data();
	void* const gapless_config_address = gapless_storage.Data();
	auto* const input_bytes            = static_cast<uint8_t*>(input_storage.Data());
	input_bytes[0]                     = static_cast<uint8_t>(packet_bytes);
	input_bytes[1]                     = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(input_bytes + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	auto* const input_buffers  = static_cast<Ajm::AjmBuffer*>(input_descriptor_storage.Data());
	auto* const output_buffers = static_cast<Ajm::AjmBuffer*>(output_descriptor_storage.Data());
	input_buffers[0]           = {input_bytes, packet_size};
	output_buffers[0]          = {pcm_storage.Data(), output_size};

	enum class Scenario : uint8_t
	{
		ResetOneAfterDecode,
		PriorSkippedFrames,
		ChangedProfileAfterDecode,
		UnlimitedTotal,
	};
	const Scenario scenarios[] = {Scenario::ResetOneAfterDecode, Scenario::PriorSkippedFrames, Scenario::ChangedProfileAfterDecode,
	                              Scenario::UnlimitedTotal};
	GuestSharedBlock result_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(result_storage.IsValid());
	auto* const shared = ::new (result_storage.Data()) AjmBatchSharedResults {};
	std::memset(shared, 0xa5, sizeof(*shared));
	const auto outputs_untouched = [shared, pcm = pcm_storage.Data()]
	{
		return IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5) &&
		       IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5) && IsAjmBytePattern(pcm, output_size, 0xa5);
	};
	for (const Scenario scenario: scenarios)
	{
		SCOPED_TRACE(static_cast<int>(scenario));
		std::memset(shared, 0xa5, sizeof(*shared));
		std::memset(pcm_storage.Data(), 0xa5, output_size);
		uint8_t  gapless_config[8] = {0xd0, 0x07, 0, 0, 0x38, 0x01, 0, 0};
		uint64_t reset             = 0;
		if (scenario == Scenario::ResetOneAfterDecode)
		{
			reset = 1;
		} else if (scenario == Scenario::PriorSkippedFrames)
		{
			gapless_config[6] = 1;
		} else if (scenario == Scenario::UnlimitedTotal)
		{
			gapless_config[0] = 0;
			gapless_config[1] = 0;
		} else
		{
			gapless_config[0] = 0xe8; // Changed total output limit: 2024 frames.
			gapless_config[1] = 0x07;
		}
		std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));

		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [=]
		    {
			    uint32_t                  context  = 0;
			    uint32_t                  instance = 0;
			    alignas(uint64_t) uint8_t batch[256] {};
			    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
			        Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 ||
			        Ajm::AjmBatchJobInitialize(batch, instance, config_address, 12, shared->init_result) != 0 ||
			        Ajm::AjmBatchJobClearContext(batch, instance, nullptr) != 0)
			    {
				    std::_Exit(91);
			    }
			    if (scenario == Scenario::ChangedProfileAfterDecode || scenario == Scenario::ResetOneAfterDecode)
			    {
				    if (Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_config_address, 0, shared->sideband_output) != 0 ||
				        Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 0, nullptr) != 0 ||
				        Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 1, output_buffers, 1, shared->decode_result) != 0)
				    {
					    std::_Exit(92);
				    }
				    uint8_t changed_config[8] = {0xe8, 0x03, 0, 0, 0x38, 0x01, 0, 0}; // 1000 output frames.
				    std::memcpy(gapless_config_address, changed_config, sizeof(changed_config));
				    if (Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_config_address, reset, nullptr) != 0)
				    {
					    std::_Exit(93);
				    }
			    } else if (Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_config_address, reset, shared->sideband_output) != 0)
			    {
				    std::_Exit(94);
			    }
			    if (!outputs_untouched())
			    {
				    std::_Exit(95);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d (0x%08x) for gapless scenario %u\n", start_status,
			                 static_cast<uint32_t>(start_status), static_cast<unsigned>(scenario));
			    std::_Exit(96);
		    },
		    &status))
		    << "strict AJM profile refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}
#endif
}

TEST(EmulatorAudio, RejectsAjmResampleResetMarkerOutsideAClearedWindowAtomically)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t   packet_bytes = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	constexpr size_t   packet_size  = sizeof(uint16_t) + packet_bytes;
	constexpr size_t   output_size  = 960 * 2 * sizeof(int16_t);
	GuestReadableBlock config_storage(12);
	GuestReadableBlock gapless_storage(8);
	GuestReadableBlock input_storage(packet_size);
	GuestReadableBlock input_descriptor_storage(sizeof(Ajm::AjmBuffer));
	GuestReadableBlock output_descriptor_storage(sizeof(Ajm::AjmBuffer));
	GuestSharedBlock   pcm_storage(output_size);
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(gapless_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(input_descriptor_storage.IsValid());
	ASSERT_TRUE(output_descriptor_storage.IsValid());
	ASSERT_TRUE(pcm_storage.IsValid());

	constexpr uint8_t opus_config[12]   = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	constexpr uint8_t gapless_config[8] = {0xd0, 0x07, 0, 0, 0x38, 0x01, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	std::memcpy(gapless_storage.Data(), gapless_config, sizeof(gapless_config));
	auto* const input_bytes = static_cast<uint8_t*>(input_storage.Data());
	input_bytes[0]          = static_cast<uint8_t>(packet_bytes);
	input_bytes[1]          = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(input_bytes + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	auto* const input_buffers             = static_cast<Ajm::AjmBuffer*>(input_descriptor_storage.Data());
	auto* const output_buffers            = static_cast<Ajm::AjmBuffer*>(output_descriptor_storage.Data());
	input_buffers[0]                      = {input_bytes, packet_size};
	output_buffers[0]                     = {pcm_storage.Data(), output_size};
	void* const config_address            = config_storage.Data();
	void* const gapless_config_address    = gapless_storage.Data();
	const Ajm::AjmBuffer* const input_descriptor_address  = input_buffers;
	const Ajm::AjmBuffer* const output_descriptor_address = output_buffers;

	enum class Scenario : uint8_t
	{
		BeforeClear,
		AfterResampleConfiguration,
		AfterDecode,
		Flag2WithoutNeutralProfile,
		UnknownFlag3,
	};
	const Scenario   scenarios[] = {Scenario::BeforeClear, Scenario::AfterResampleConfiguration, Scenario::AfterDecode,
	                                Scenario::Flag2WithoutNeutralProfile, Scenario::UnknownFlag3};
	GuestSharedBlock result_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(result_storage.IsValid());
	auto* const shared            = ::new (result_storage.Data()) AjmBatchSharedResults {};
	const auto  outputs_untouched = [shared, pcm = pcm_storage.Data()]
	{
		return IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5) &&
		       IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5) && IsAjmBytePattern(pcm, output_size, 0xa5);
	};
	for (const Scenario scenario: scenarios)
	{
		SCOPED_TRACE(static_cast<int>(scenario));
		std::memset(shared, 0xa5, sizeof(*shared));
		std::memset(pcm_storage.Data(), 0xa5, output_size);
		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [=]
		    {
			    uint32_t                  context  = 0;
			    uint32_t                  instance = 0;
			    alignas(uint64_t) uint8_t batch[256] {};
			    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
			        Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 ||
			        Ajm::AjmBatchJobInitialize(batch, instance, config_address, sizeof(opus_config), shared->init_result) != 0)
			    {
				    std::_Exit(90);
			    }
			    if (scenario != Scenario::BeforeClear && Ajm::AjmBatchJobClearContext(batch, instance, nullptr) != 0)
			    {
				    std::_Exit(91);
			    }
			    if (Ajm::AjmBatchJobSetGaplessDecode(batch, instance, gapless_config_address, 0, shared->sideband_output) != 0)
			    {
				    std::_Exit(92);
			    }
			    if (scenario == Scenario::AfterResampleConfiguration &&
			        Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, 0, shared->sideband_output + 8) != 0)
			    {
				    std::_Exit(93);
			    }
			    if (scenario == Scenario::AfterDecode &&
			        Ajm::AjmBatchJobDecodeSplit(batch, instance, input_descriptor_address, 1, output_descriptor_address, 1,
			                                    shared->decode_result) != 0)
			    {
				    std::_Exit(94);
			    }
			    uint32_t reset_flags = 1;
			    if (scenario == Scenario::Flag2WithoutNeutralProfile)
			    {
				    reset_flags = 2;
			    } else if (scenario == Scenario::UnknownFlag3)
			    {
				    reset_flags = 3;
			    }
			    if (Ajm::AjmBatchJobSetResampleParametersEx(batch, instance, 1.0f, 0.0f, reset_flags, shared->sideband_output + 16) != 0 ||
			        !outputs_untouched())
			    {
				    std::_Exit(96);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d (0x%08x) for resample reset-marker scenario %u\n",
			                 start_status, static_cast<uint32_t>(start_status), static_cast<unsigned>(scenario));
			    std::_Exit(97);
		    },
		    &status))
		    << "strict AJM resample reset-marker refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}
#endif
}

TEST(EmulatorAudio, RejectsMalformedAjmOpusRecordsBeforePublishingGuestWrites)
{
#if !defined(__linux__)
	GTEST_SKIP() << "isolated AJM decoder check requires Linux";
#else
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!AudioVideoBackend::Decoder::IsAvailable())
	{
		GTEST_SKIP() << "Opus backend is unavailable";
	}

	constexpr size_t packet_bytes        = AudioTestFixtures::kGeneratedOpusSinePacket.size();
	constexpr size_t framed_packet_bytes = sizeof(uint16_t) + packet_bytes;
	constexpr size_t output_capacity     = AJM_OPUS_TEST_OUTPUT_CAPACITY;
	constexpr size_t result_offset       = output_capacity;
	constexpr size_t split_result_offset = result_offset + 32;
	constexpr size_t error_offset        = split_result_offset + 32;
	static_assert(sizeof(Ajm::AjmBatchError) % alignof(uint32_t) == 0);
	constexpr size_t batch_id_offset = error_offset + sizeof(Ajm::AjmBatchError);

	GuestReadableBlock config_storage(12);
	GuestReadableBlock input_storage(framed_packet_bytes + 1);
	GuestReadableBlock input_descriptor_storage(2 * sizeof(Ajm::AjmBuffer));
	GuestReadableBlock output_descriptor_storage(sizeof(Ajm::AjmBuffer));
	GuestSharedBlock   output_storage(batch_id_offset + sizeof(uint32_t));
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	ASSERT_TRUE(input_descriptor_storage.IsValid());
	ASSERT_TRUE(output_descriptor_storage.IsValid());
	ASSERT_TRUE(output_storage.IsValid());

	constexpr uint8_t opus_config[12] = {2, 0, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
	std::memcpy(config_storage.Data(), opus_config, sizeof(opus_config));
	auto* const config      = static_cast<const uint8_t*>(config_storage.Data());
	auto* const input_bytes = static_cast<uint8_t*>(input_storage.Data());
	input_bytes[0]          = static_cast<uint8_t>(packet_bytes);
	input_bytes[1]          = static_cast<uint8_t>(packet_bytes >> 8u);
	std::memcpy(input_bytes + sizeof(uint16_t), AudioTestFixtures::kGeneratedOpusSinePacket.data(), packet_bytes);
	input_bytes[framed_packet_bytes] = 0xa5; // Incomplete next record after a valid packet.

	auto* const input_buffers  = static_cast<Ajm::AjmBuffer*>(input_descriptor_storage.Data());
	auto* const output_buffers = static_cast<Ajm::AjmBuffer*>(output_descriptor_storage.Data());
	input_buffers[0]           = {input_bytes, framed_packet_bytes};
	input_buffers[1]           = {input_bytes + framed_packet_bytes, 1};
	auto* const output_bytes   = static_cast<uint8_t*>(output_storage.Data());
	std::memset(output_bytes, 0xa5, batch_id_offset + sizeof(uint32_t));
	output_buffers[0]             = {output_bytes, output_capacity};
	void* const initialize_result = output_bytes + result_offset;
	void* const split_result      = output_bytes + split_result_offset;
	auto* const error             = reinterpret_cast<Ajm::AjmBatchError*>(output_bytes + error_offset);
	auto* const batch_id          = reinterpret_cast<uint32_t*>(output_bytes + batch_id_offset);

	int child_status = 0;
	ASSERT_TRUE(RunAjmStrictChild(
	    [=]
	    {
		    uint32_t                  context  = 0;
		    uint32_t                  instance = 0;
		    alignas(uint64_t) uint8_t batch[128] {};
		    if (Ajm::AjmInitialize(0, &context) != 0 || Ajm::AjmModuleRegister(context, 24, 0) != 0 ||
		        Ajm::AjmInstanceCreate(context, 24, 0x1002, &instance) != 0 ||
		        Ajm::AjmBatchJobInitialize(batch, instance, config, 12, initialize_result) != 0 ||
		        Ajm::AjmBatchJobDecodeSplit(batch, instance, input_buffers, 2, output_buffers, 1, split_result) != 0)
		    {
			    std::_Exit(81);
		    }
		    (void)Ajm::AjmBatchStart(context, batch, 0, error, batch_id);
		    std::_Exit(82);
	    },
	    &child_status));
	ASSERT_TRUE(WIFEXITED(child_status));
	EXPECT_EQ(WEXITSTATUS(child_status), AJM_STRICT_EXIT_STATUS);
	EXPECT_TRUE(IsAjmBytePattern(output_bytes, batch_id_offset + sizeof(uint32_t), 0xa5));
#endif
}

TEST(EmulatorAudio, AjmBatchUsesFullInstanceIdentityBeforePublishingResults)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t owner_context = 0;
	uint32_t other_context = 0;
	uint32_t instance      = 0;
	ASSERT_EQ(Ajm::AjmInitialize(0, &owner_context), 0);
	ASSERT_EQ(Ajm::AjmInitialize(0, &other_context), 0);
	ASSERT_EQ(Ajm::AjmModuleRegister(owner_context, 1, 0), 0);
	ASSERT_EQ(Ajm::AjmInstanceCreate(owner_context, 1, 0x401, &instance), 0);

	GuestReadableBlock             output_storage(sizeof(int16_t));
	GuestReadableBlock             result_storage(32);
	GuestValue<Ajm::AjmBatchError> error_storage;
	GuestValue<uint32_t>           batch_id_storage;
	ASSERT_TRUE(output_storage.IsValid());
	ASSERT_TRUE(result_storage.IsValid());
	ASSERT_TRUE(error_storage.IsValid());
	ASSERT_TRUE(batch_id_storage.IsValid());
	std::memset(output_storage.Data(), 0xa5, sizeof(int16_t));
	std::memset(result_storage.Data(), 0xa5, 32);
	std::memset(error_storage.Data(), 0xa5, sizeof(Ajm::AjmBatchError));
	*batch_id_storage.Data() = 0xa5a5a5a5u;

	alignas(uint64_t) uint8_t batch[32] {};
	ASSERT_EQ(Ajm::AjmBatchJobDecode(batch, instance, nullptr, 0, output_storage.Data(), sizeof(int16_t), result_storage.Data()), 0);
	EXPECT_EQ(Ajm::AjmBatchStart(other_context, batch, 0, error_storage.Data(), batch_id_storage.Data()),
	          static_cast<int32_t>(0x80930003u));
	EXPECT_TRUE(IsAjmBytePattern(output_storage.Data(), sizeof(int16_t), 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(result_storage.Data(), 32, 0xa5));
	EXPECT_TRUE(IsAjmBytePattern(error_storage.Data(), sizeof(Ajm::AjmBatchError), 0xa5));
	EXPECT_EQ(*batch_id_storage.Data(), 0xa5a5a5a5u);

	EXPECT_EQ(Ajm::AjmFinalize(owner_context), 0);
	EXPECT_EQ(Ajm::AjmFinalize(other_context), 0);
}

TEST(EmulatorAudio, RejectsUnsupportedAjmDirectControlsAtomicallyAfterDecodePrefix)
{
#if !defined(__linux__)
	GTEST_SKIP() << "strict AJM refusal check requires Linux";
#else
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

	GuestReadableBlock config_storage(8);
	const auto         superframe = BuildSilentAtrac9Superframe();
	GuestReadableBlock input_storage(superframe.size());
	ASSERT_TRUE(config_storage.IsValid());
	ASSERT_TRUE(input_storage.IsValid());
	auto* const config = static_cast<uint8_t*>(config_storage.Data());
	std::memcpy(config, kSyntheticAtrac9Config, sizeof(kSyntheticAtrac9Config));
	std::memset(config + sizeof(kSyntheticAtrac9Config), 0, 4);
	std::memcpy(input_storage.Data(), superframe.data(), superframe.size());

	enum class UnsupportedControl : uint8_t
	{
		SetGapless,
		GetGapless,
		SetResample,
		SetResampleEx,
		GetResampleInfo,
		GetInfo,
		GetCodecInfo,
		GetStatistics,
		Control,
	};
	// A gapless reset after the decode is admitted, so SetGapless is not listed.
	const UnsupportedControl operations[] = {UnsupportedControl::GetGapless,      UnsupportedControl::SetResample,
	                                         UnsupportedControl::SetResampleEx,   UnsupportedControl::GetResampleInfo,
	                                         UnsupportedControl::GetInfo,         UnsupportedControl::GetCodecInfo,
	                                         UnsupportedControl::GetStatistics,   UnsupportedControl::Control};

	alignas(uint64_t) uint8_t batch[64] {};
	void* const               batch_address = batch;
	GuestSharedBlock results_storage(sizeof(AjmBatchSharedResults));
	ASSERT_TRUE(results_storage.IsValid());
	auto* const shared = ::new (results_storage.Data()) AjmBatchSharedResults {};
	std::memset(shared, 0xa5, sizeof(*shared));
	const auto  outputs_untouched = [shared]
	{
		return IsAjmBytePattern(shared->data_output, sizeof(shared->data_output), 0xa5) &&
		       IsAjmBytePattern(shared->init_result, sizeof(shared->init_result), 0xa5) &&
		       IsAjmBytePattern(shared->decode_result, sizeof(shared->decode_result), 0xa5) &&
		       IsAjmBytePattern(shared->sideband_output, sizeof(shared->sideband_output), 0xa5) &&
		       IsAjmBytePattern(&shared->error, sizeof(shared->error), 0xa5) &&
		       IsAjmBytePattern(&shared->batch_id, sizeof(shared->batch_id), 0xa5);
	};

	for (const auto operation: operations)
	{
		std::memset(shared, 0xa5, sizeof(*shared));
		int status = 0;
		ASSERT_TRUE(RunAjmStrictChild(
		    [context, instance, batch_address, config, input = input_storage.Data(), shared, operation, outputs_untouched]
		    {
			    if (Ajm::AjmBatchJobInitialize(batch_address, instance, config, 8, shared->init_result) != 0 || !outputs_untouched())
			    {
				    std::_Exit(90);
			    }
			    if (Ajm::AjmBatchJobDecode(batch_address, instance, input, kSyntheticAtrac9Superframe, shared->data_output,
			                               sizeof(shared->data_output), shared->decode_result) != 0 ||
			        !outputs_untouched())
			    {
				    std::_Exit(91);
			    }

			    int enqueue_status = 0;
			    switch (operation)
			    {
				    case UnsupportedControl::SetGapless:
					    enqueue_status = Ajm::AjmBatchJobSetGaplessDecode(batch_address, instance, config, 1, shared->sideband_output);
					    break;
				    case UnsupportedControl::GetGapless:
					    enqueue_status = Ajm::AjmBatchJobGetGaplessDecode(batch_address, instance, shared->sideband_output);
					    break;
				    case UnsupportedControl::SetResample:
					    enqueue_status = Ajm::AjmBatchJobSetResampleParameters(batch_address, instance, 1.0f, 0, shared->sideband_output);
					    break;
				    case UnsupportedControl::SetResampleEx:
					    enqueue_status =
					        Ajm::AjmBatchJobSetResampleParametersEx(batch_address, instance, 1.0f, 0.0f, 0, shared->sideband_output);
					    break;
				    case UnsupportedControl::GetResampleInfo:
					    enqueue_status = Ajm::AjmBatchJobGetResampleInfo(batch_address, instance, shared->sideband_output);
					    break;
				    case UnsupportedControl::GetInfo:
					    enqueue_status = Ajm::AjmBatchJobGetInfo(batch_address, instance, shared->sideband_output);
					    break;
				    case UnsupportedControl::GetCodecInfo:
					    enqueue_status =
					        Ajm::AjmBatchJobGetCodecInfo(batch_address, instance, shared->sideband_output, sizeof(shared->sideband_output));
					    break;
				    case UnsupportedControl::GetStatistics:
					    enqueue_status = Ajm::AjmBatchJobGetStatistics(batch_address, 0.0f, shared->sideband_output);
					    break;
				    case UnsupportedControl::Control:
					    enqueue_status = Ajm::AjmBatchJobControl(batch_address, instance, 0, nullptr, 0, shared->sideband_output,
					                                             sizeof(shared->sideband_output));
					    break;
			    }
			    if (enqueue_status != 0 || !outputs_untouched())
			    {
				    std::_Exit(92);
			    }
			    const int32_t start_status = Ajm::AjmBatchStart(context, batch_address, 0, &shared->error, &shared->batch_id);
			    std::fprintf(stderr, "AjmBatchStart unexpectedly returned %d (0x%08x) for unsupported control %u\n", start_status,
			                 static_cast<uint32_t>(start_status), static_cast<unsigned>(operation));
			    std::_Exit(96);
		    },
		    &status))
		    << "strict AJM refusal timed out";
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), AJM_STRICT_EXIT_STATUS);
		EXPECT_TRUE(outputs_untouched());
	}

	EXPECT_EQ(Ajm::AjmInstanceDestroy(context, instance), 0);
	EXPECT_EQ(Ajm::AjmFinalize(context), 0);
#endif
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
	blocks[0].frames      = {sample * gain};
	blocks[0].num_samples = 1;
	blocks[0].num_repeats = Ngs2Sampler::kRepeatForever;
	ASSERT_TRUE(playback.ReplaceQueue(std::move(blocks)));
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
