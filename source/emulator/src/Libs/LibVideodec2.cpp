#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Videodec2", 1, "Videodec2", 1, 1);

namespace Videodec2 {

namespace Backend = Emulator::AudioVideoBackend;

// Every structure starts with its own size; a size the decoder does not know
// is refused rather than read or written past the guest's object.
struct ComputeMemoryInfo
{
	uint64_t this_size;
	uint64_t compute_memory_size;
	void*    compute_memory;
};

struct ComputeConfigInfo
{
	uint64_t this_size;
	uint16_t compute_pipe_id;
	uint16_t compute_queue_id;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint16_t reserved1;
};

struct DecoderConfigInfo
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

struct DecoderMemoryInfo
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

struct InputData
{
	uint64_t this_size;
	void*    au_data;
	uint64_t au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct FrameBuffer
{
	uint64_t this_size;
	void*    frame_buffer;
	uint64_t frame_buffer_size;
	bool     is_accepted;
};

struct OutputInfo
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

static_assert(sizeof(ComputeMemoryInfo) == 0x18 && sizeof(ComputeConfigInfo) == 0x10 && sizeof(DecoderConfigInfo) == 0x48 &&
              sizeof(DecoderMemoryInfo) == 0x48 && sizeof(InputData) == 0x30 && sizeof(FrameBuffer) == 0x20 &&
              sizeof(OutputInfo) == 0x30);

constexpr uint32_t CODEC_AVC  = 1;
constexpr uint32_t CODEC_HEVC = 2;
// Rows of both NV12 planes start on the 256-byte boundary GPU linear surfaces need.
constexpr uint32_t FRAME_PITCH_ALIGNMENT  = 256;
constexpr uint32_t FRAME_BUFFER_ALIGNMENT = 0x10000;

struct ComputeQueue
{
	uint16_t pipe_id  = 0;
	uint16_t queue_id = 0;
};

struct Decoder
{
	std::mutex                                       mutex;
	uint32_t                                         codec_type = 0;
	std::unique_ptr<Backend::ElementaryVideoDecoder> backend;
	int64_t                                          next_tag = 0;
};

static bool SizeIs(uint64_t this_size, uint64_t expected, const char* name);

template <typename T>
struct GuestStructSnapshot
{
	T                                         value   = {};
	Core::VirtualMemory::GuestMappingSnapshot mapping = {};
};

template <typename T>
static bool CopyGuestStruct(const T* guest, T* local, const char* name)
{
	if (guest == nullptr || local == nullptr || !Core::VirtualMemory::CopyFromGuest(local, reinterpret_cast<uint64_t>(guest), sizeof(T)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s is not readable\n", name);
		return false;
	}
	return SizeIs(local->this_size, sizeof(T), name);
}

template <typename T>
static bool CopyGuestStructForOutput(T* guest, GuestStructSnapshot<T>* snapshot, const char* name)
{
	if (guest == nullptr || snapshot == nullptr ||
	    !Core::VirtualMemory::CaptureGuestMappingSnapshot(reinterpret_cast<uint64_t>(guest), sizeof(T), &snapshot->mapping) ||
	    !Core::VirtualMemory::CopyFromGuest(&snapshot->value, reinterpret_cast<uint64_t>(guest), sizeof(T)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s is not readable guest memory\n", name);
		return false;
	}
	if (!SizeIs(snapshot->value.this_size, sizeof(T), name))
	{
		return false;
	}
	if (!Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(guest), sizeof(T)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s is not writable guest memory\n", name);
		return false;
	}
	return true;
}

template <typename T>
static bool CaptureGuestOutput(T* guest, Core::VirtualMemory::GuestMappingSnapshot* snapshot, const char* name)
{
	if (guest == nullptr || snapshot == nullptr ||
	    !Core::VirtualMemory::CaptureGuestMappingSnapshot(reinterpret_cast<uint64_t>(guest), sizeof(T), snapshot))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s is not guest memory\n", name);
		return false;
	}
	return true;
}

template <typename T>
static bool CopyGuestOutput(const Core::VirtualMemory::GuestMappingSnapshot& snapshot, const T& value, const char* name)
{
	if (!Core::VirtualMemory::CopyToGuestIfMappingMatches(snapshot, &value, sizeof(T)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s mapping is no longer writable\n", name);
		return false;
	}
	return true;
}

template <typename Object, uintptr_t HandleType>
class OpaqueHandleRegistry
{
public:
	using Handle = Object*;

	class Lease
	{
	public:
		Lease() = default;
		Lease(OpaqueHandleRegistry* registry, size_t slot, Object* object): m_registry(registry), m_slot(slot), m_object(object) {}
		Lease(const Lease&)            = delete;
		Lease& operator=(const Lease&) = delete;
		Lease(Lease&& other) noexcept { MoveFrom(std::move(other)); }
		Lease& operator=(Lease&& other) noexcept
		{
			if (this != &other)
			{
				Release();
				MoveFrom(std::move(other));
			}
			return *this;
		}
		~Lease() { Release(); }

		[[nodiscard]] explicit operator bool() const { return m_object != nullptr; }
		[[nodiscard]] Object*  operator->() const { return m_object; }
		[[nodiscard]] Object*  get() const { return m_object; }

	private:
		void MoveFrom(Lease&& other) noexcept
		{
			m_registry       = other.m_registry;
			m_slot           = other.m_slot;
			m_object         = other.m_object;
			other.m_registry = nullptr;
			other.m_object   = nullptr;
		}

		void Release()
		{
			if (m_registry != nullptr)
			{
				m_object = nullptr;
				m_registry->ReleaseLease(m_slot);
				m_registry = nullptr;
			}
		}

		OpaqueHandleRegistry* m_registry = nullptr;
		size_t                m_slot     = 0;
		Object*               m_object   = nullptr;
	};

	Handle Insert(std::unique_ptr<Object> object)
	{
		Core::LockGuard lock(m_mutex);
		size_t          index = 0;
		for (; index < m_size; index++)
		{
			const auto& slot = m_slots[index];
			if (slot.object == nullptr && !slot.closing && !slot.retired)
			{
				break;
			}
		}
		if (index == m_size)
		{
			if (index >= kIndexMask)
			{
				return nullptr;
			}
			if (index == m_capacity && !Grow())
			{
				return nullptr;
			}
			++m_size;
		}
		if (index >= kIndexMask)
		{
			return nullptr;
		}

		auto& slot  = m_slots[index];
		slot.object = std::move(object);
		return EncodeHandle(index, slot.generation);
	}

	[[nodiscard]] Lease Acquire(Handle handle)
	{
		size_t    index      = 0;
		uintptr_t generation = 0;
		if (!DecodeHandle(handle, &index, &generation))
		{
			return {};
		}

		Core::LockGuard lock(m_mutex);
		if (index >= m_size)
		{
			return {};
		}
		auto& slot = m_slots[index];
		if (slot.generation != generation || slot.closing || slot.retired || slot.object == nullptr)
		{
			return {};
		}
		++slot.active_leases;
		return Lease(this, index, slot.object.get());
	}

	bool Remove(Handle handle)
	{
		size_t    index      = 0;
		uintptr_t generation = 0;
		if (!DecodeHandle(handle, &index, &generation))
		{
			return false;
		}

		std::unique_ptr<Object> object;
		{
			Core::LockGuard lock(m_mutex);
			if (index >= m_size)
			{
				return false;
			}
			auto& slot = m_slots[index];
			if (slot.generation != generation || slot.closing || slot.retired || slot.object == nullptr)
			{
				return false;
			}
			slot.closing = true;
			while (m_slots[index].active_leases != 0)
			{
				Kernel::PthreadInterruptibleWait(&m_condition, &m_mutex);
			}
			object = std::move(m_slots[index].object);
			if (m_slots[index].generation == kGenerationMax)
			{
				m_slots[index].retired = true;
			} else
			{
				++m_slots[index].generation;
			}
			m_slots[index].closing = false;
		}
		object.reset();
		return true;
	}

private:
	struct Slot
	{
		std::unique_ptr<Object> object;
		uintptr_t               generation    = 1;
		uint64_t                active_leases = 0;
		bool                    closing       = false;
		bool                    retired       = false;
	};

	static constexpr unsigned  kIndexBits      = sizeof(uintptr_t) >= 8 ? 24u : 16u;
	static constexpr unsigned  kGenerationBits = sizeof(uintptr_t) >= 8 ? 16u : 12u;
	static constexpr unsigned  kTypeBits       = sizeof(uintptr_t) >= 8 ? 8u : 4u;
	static constexpr unsigned  kTypeShift      = kIndexBits + kGenerationBits;
	static constexpr uintptr_t kIndexMask      = (uintptr_t {1} << kIndexBits) - 1u;
	static constexpr uintptr_t kGenerationMask = (uintptr_t {1} << kGenerationBits) - 1u;
	static constexpr uintptr_t kTypeMask       = (uintptr_t {1} << kTypeBits) - 1u;
	static constexpr uintptr_t kGenerationMax  = kGenerationMask;
	static_assert(HandleType != 0 && HandleType <= kTypeMask);

	static bool DecodeHandle(Handle handle, size_t* index, uintptr_t* generation)
	{
		const uintptr_t value   = reinterpret_cast<uintptr_t>(handle);
		const uintptr_t slot_id = value & kIndexMask;
		const uintptr_t version = (value >> kIndexBits) & kGenerationMask;
		const uintptr_t type    = value >> kTypeShift;
		if (value == 0 || slot_id == 0 || version == 0 || type != HandleType)
		{
			return false;
		}
		*index      = static_cast<size_t>(slot_id - 1u);
		*generation = version;
		return true;
	}

	static Handle EncodeHandle(size_t index, uintptr_t generation)
	{
		const uintptr_t value = (HandleType << kTypeShift) | (generation << kIndexBits) | (static_cast<uintptr_t>(index) + 1u);
		return reinterpret_cast<Handle>(value);
	}

	bool Grow()
	{
		size_t new_capacity = m_capacity == 0 ? 8u : m_capacity * 2u;
		if (new_capacity < m_capacity || new_capacity > kIndexMask)
		{
			new_capacity = static_cast<size_t>(kIndexMask);
		}
		if (new_capacity <= m_capacity)
		{
			return false;
		}

		std::unique_ptr<Slot[]> new_slots(new (std::nothrow) Slot[new_capacity]);
		if (new_slots == nullptr)
		{
			return false;
		}
		for (size_t index = 0; index < m_size; ++index)
		{
			new_slots[index] = std::move(m_slots[index]);
		}
		m_slots    = std::move(new_slots);
		m_capacity = new_capacity;
		return true;
	}

	void ReleaseLease(size_t index)
	{
		Core::LockGuard lock(m_mutex);
		if (index < m_size && m_slots[index].active_leases != 0)
		{
			--m_slots[index].active_leases;
			if (m_slots[index].active_leases == 0)
			{
				m_condition.SignalAll();
			}
		}
	}

	Core::Mutex             m_mutex;
	Core::CondVar           m_condition;
	std::unique_ptr<Slot[]> m_slots;
	size_t                  m_size     = 0;
	size_t                  m_capacity = 0;
};

using DecoderHandleRegistry      = OpaqueHandleRegistry<Decoder, 1>;
using ComputeQueueHandleRegistry = OpaqueHandleRegistry<ComputeQueue, 2>;

static DecoderHandleRegistry& DecoderHandles()
{
	static DecoderHandleRegistry registry;
	return registry;
}

static ComputeQueueHandleRegistry& ComputeQueueHandles()
{
	static ComputeQueueHandleRegistry registry;
	return registry;
}

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
	return static_cast<uint32_t>((static_cast<uint64_t>(value) + alignment - 1u) / alignment * alignment);
}

// NV12 with the chroma rows right after the luma rows, both at the same pitch.
static uint64_t FrameBufferSize(uint32_t width, uint32_t height)
{
	const uint64_t pitch        = AlignUp(width, FRAME_PITCH_ALIGNMENT);
	const uint64_t frame_height = height;
	return pitch * frame_height + pitch * ((frame_height + 1u) / 2u);
}

static bool SizeIs(uint64_t this_size, uint64_t expected, const char* name)
{
	if (this_size == expected)
	{
		return true;
	}
	KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s size %" PRIu64 " is not the known %" PRIu64 "\n", name, this_size, expected);
	return false;
}

static KYTY_SYSV_ABI int Videodec2QueryComputeMemoryInfo(ComputeMemoryInfo* info)
{
	PRINT_NAME();
	GuestStructSnapshot<ComputeMemoryInfo> snapshot;
	if (!CopyGuestStructForOutput(info, &snapshot, "ComputeMemoryInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Decoding runs on the host; the compute queue needs no guest memory.
	snapshot.value.compute_memory_size = 0;
	snapshot.value.compute_memory      = nullptr;
	return CopyGuestOutput(snapshot.mapping, snapshot.value, "ComputeMemoryInfo") ? OK : LibKernel::KERNEL_ERROR_EINVAL;
}

static KYTY_SYSV_ABI int Videodec2AllocateComputeQueue(const ComputeConfigInfo* config, const ComputeMemoryInfo* memory,
                                                       ComputeQueue** queue)
{
	PRINT_NAME();
	ComputeConfigInfo                         config_local {};
	ComputeMemoryInfo                         memory_local {};
	Core::VirtualMemory::GuestMappingSnapshot queue_mapping {};
	if (!CopyGuestStruct(config, &config_local, "ComputeConfigInfo") || !CopyGuestStruct(memory, &memory_local, "ComputeMemoryInfo") ||
	    !CaptureGuestOutput(queue, &queue_mapping, "compute queue output") ||
	    !CopyGuestOutput(queue_mapping, static_cast<ComputeQueue*>(nullptr), "compute queue output"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::unique_ptr<ComputeQueue> created(new (std::nothrow) ComputeQueue {config_local.compute_pipe_id, config_local.compute_queue_id});
	if (created == nullptr)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	ComputeQueue* handle = ComputeQueueHandles().Insert(std::move(created));
	if (handle == nullptr)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	if (!CopyGuestOutput(queue_mapping, handle, "compute queue output"))
	{
		(void)ComputeQueueHandles().Remove(handle);
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

static KYTY_SYSV_ABI int Videodec2ReleaseComputeQueue(ComputeQueue* queue)
{
	PRINT_NAME();
	if (!ComputeQueueHandles().Remove(queue))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 compute queue handle is invalid\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

static KYTY_SYSV_ABI int Videodec2QueryDecoderMemoryInfo(const DecoderConfigInfo* config, DecoderMemoryInfo* memory)
{
	PRINT_NAME();
	DecoderConfigInfo                      config_local {};
	GuestStructSnapshot<DecoderMemoryInfo> memory_snapshot;
	if (!CopyGuestStruct(config, &config_local, "DecoderConfigInfo") ||
	    !CopyGuestStructForOutput(memory, &memory_snapshot, "DecoderMemoryInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (config_local.max_frame_width <= 0 || config_local.max_frame_height <= 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	KYTY_LOG_DEBUG("\t codec = %u, profile = %u, level = %u, max = %dx%d, dpb = %d\n", config_local.codec_type, config_local.profile,
	               config_local.max_level, config_local.max_frame_width, config_local.max_frame_height, config_local.max_dpb_frame_count);
	// The host decoder keeps its own state; only the frames live in guest memory.
	memory_snapshot.value.cpu_memory_size     = 0;
	memory_snapshot.value.gpu_memory_size     = 0;
	memory_snapshot.value.cpu_gpu_memory_size = 0;
	memory_snapshot.value.max_frame_buffer_size =
	    FrameBufferSize(static_cast<uint32_t>(config_local.max_frame_width), static_cast<uint32_t>(config_local.max_frame_height));
	memory_snapshot.value.frame_buffer_alignment = FRAME_BUFFER_ALIGNMENT;
	return CopyGuestOutput(memory_snapshot.mapping, memory_snapshot.value, "DecoderMemoryInfo") ? OK : LibKernel::KERNEL_ERROR_EINVAL;
}

static KYTY_SYSV_ABI int Videodec2CreateDecoder(const DecoderConfigInfo* config, const DecoderMemoryInfo* memory, Decoder** decoder)
{
	PRINT_NAME();
	DecoderConfigInfo                         config_local {};
	DecoderMemoryInfo                         memory_local {};
	Core::VirtualMemory::GuestMappingSnapshot decoder_mapping {};
	if (!CopyGuestStruct(config, &config_local, "DecoderConfigInfo") || !CopyGuestStruct(memory, &memory_local, "DecoderMemoryInfo") ||
	    !CaptureGuestOutput(decoder, &decoder_mapping, "decoder output") ||
	    !CopyGuestOutput(decoder_mapping, static_cast<Decoder*>(nullptr), "decoder output"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if ((config_local.codec_type != CODEC_AVC && config_local.codec_type != CODEC_HEVC) || config_local.max_frame_width <= 0 ||
	    config_local.max_frame_height <= 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::string error;
	auto        backend =
	    Backend::ElementaryVideoDecoder::Open(config_local.codec_type == CODEC_HEVC ? Backend::VideoCodec::Hevc : Backend::VideoCodec::Avc,
	                                          config_local.max_frame_width, config_local.max_frame_height, &error);
	if (backend == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder unavailable: %s\n", error.c_str());
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::unique_ptr<Decoder> created(new (std::nothrow) Decoder);
	if (created == nullptr)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	created->codec_type = config_local.codec_type;
	created->backend    = std::move(backend);
	Decoder* handle     = DecoderHandles().Insert(std::move(created));
	if (handle == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder handle registry is full\n");
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	if (!CopyGuestOutput(decoder_mapping, handle, "decoder output"))
	{
		(void)DecoderHandles().Remove(handle);
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

static KYTY_SYSV_ABI int Videodec2DeleteDecoder(Decoder* decoder)
{
	PRINT_NAME();
	if (!DecoderHandles().Remove(decoder))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder handle is invalid\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

struct FramePublicationContext
{
	uint64_t       frame_address        = 0;
	uint64_t       frame_bytes          = 0;
	uint32_t       frame_width          = 0;
	uint32_t       frame_height         = 0;
	uint32_t       frame_pitch          = 0;
	const uint8_t* frame_data           = nullptr;
	uint64_t       frame_buffer_address = 0;
	FrameBuffer    frame_buffer_value {};
	uint64_t       output_address         = 0;
	OutputInfo     output_value           {};
	bool           publication_succeeded = false;
};

static void PublishFrameTransaction(const Core::VirtualMemory::GuestMappingIdentitySnapshot* frame_identity, void* context) noexcept
{
	auto&          publish     = *static_cast<FramePublicationContext*>(context);
	if (!Emulator::VideoFrameMemory::RegisterLinearFrame(publish.frame_address, static_cast<size_t>(publish.frame_bytes),
	                                                     publish.frame_pitch, *frame_identity))
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
	publish.publication_succeeded = true;
}

struct MetadataPublicationContext
{
	uint64_t    frame_buffer_address = 0;
	FrameBuffer frame_buffer_value {};
	uint64_t    output_address = 0;
	OutputInfo  output_value {};
};

static void PublishMetadataTransaction(const Core::VirtualMemory::GuestMappingIdentitySnapshot*, void* context) noexcept
{
	auto& publish = *static_cast<MetadataPublicationContext*>(context);
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(publish.frame_buffer_address)), &publish.frame_buffer_value,
	            sizeof(publish.frame_buffer_value));
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(publish.output_address)), &publish.output_value,
	            sizeof(publish.output_value));
}

static int PublishEmptyOutput(const GuestStructSnapshot<FrameBuffer>& frame_buffer, const GuestStructSnapshot<OutputInfo>& output)
{
	const Core::VirtualMemory::WritableGuestTransactionRange ranges[] = {
	    {frame_buffer.mapping.address, sizeof(FrameBuffer), &frame_buffer.mapping},
	    {output.mapping.address, sizeof(OutputInfo), &output.mapping},
	};
	MetadataPublicationContext context {frame_buffer.mapping.address, frame_buffer.value, output.mapping.address, output.value};
	return Core::VirtualMemory::VisitWritableGuestTransaction(ranges, 2, -1, PublishMetadataTransaction, &context)
	           ? OK
	           : LibKernel::KERNEL_ERROR_EINVAL;
}

static int PublishFrame(GuestStructSnapshot<FrameBuffer>& frame_buffer, GuestStructSnapshot<OutputInfo>& output,
                        const Backend::VideoFrame& picture, uint32_t pitch, uint64_t frame_bytes)
{
	const uint64_t destination = reinterpret_cast<uint64_t>(frame_buffer.value.frame_buffer);
	if (frame_buffer.value.frame_buffer == nullptr || frame_buffer.value.frame_buffer_size < frame_bytes ||
	    frame_buffer.value.frame_buffer_size > std::numeric_limits<uint64_t>::max() - destination)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 frame buffer range is not writable or is too small\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (frame_bytes > std::numeric_limits<size_t>::max())
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}

	const uint64_t rows = static_cast<uint64_t>(picture.height) + (static_cast<uint64_t>(picture.height) + 1u) / 2u;
	if (picture.width == 0 || pitch == 0 || rows > std::numeric_limits<uint64_t>::max() / picture.width ||
	    rows > std::numeric_limits<uint64_t>::max() / pitch)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint64_t source_bytes = rows * picture.width;
	if (source_bytes > picture.data.size() || rows * pitch != frame_bytes)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 backend frame is truncated or has an invalid extent\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	frame_buffer.value.is_accepted = true;
	output.value.is_valid          = true;
	output.value.picture_count     = 1;
	output.value.frame_width       = picture.width;
	output.value.frame_pitch       = pitch;
	output.value.frame_height      = picture.height;
	output.value.frame_buffer      = frame_buffer.value.frame_buffer;
	output.value.frame_buffer_size = frame_bytes;

	const Core::VirtualMemory::WritableGuestTransactionRange ranges[] = {
	    {destination, frame_buffer.value.frame_buffer_size, nullptr},
	    {destination, frame_bytes, nullptr},
	    {frame_buffer.mapping.address, sizeof(FrameBuffer), &frame_buffer.mapping},
	    {output.mapping.address, sizeof(OutputInfo), &output.mapping},
	};
	FramePublicationContext                          context {destination,
	                                                          frame_bytes,
	                                                          picture.width,
	                                                          picture.height,
	                                                          pitch,
	                                                          picture.data.data(),
	                                                          frame_buffer.mapping.address,
	                                                          frame_buffer.value,
	                                                          output.mapping.address,
	                                                          output.value};
	Emulator::VideoFrameMemory::PruneStaleLayouts();
	const Emulator::VideoFrameMemory::HostWriteLease write_lease(destination, frame_bytes);
	if (!Core::VirtualMemory::VisitWritableGuestTransaction(ranges, 4, 1, PublishFrameTransaction, &context))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 guest frame publication was rejected\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (!context.publication_succeeded)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 frame layout admission was rejected\n");
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	return OK;
}

// Writes the next decoded picture, if any, into the guest frame buffer.
static int OutputPicture(Decoder* decoder, GuestStructSnapshot<FrameBuffer>* frame_buffer, GuestStructSnapshot<OutputInfo>* output)
{
	frame_buffer->value.is_accepted = false;
	output->value.is_valid          = false;
	output->value.is_error_frame    = false;
	output->value.picture_count     = 0;
	Backend::VideoFrame picture;
	int64_t             tag = 0;
	if (!decoder->backend->Receive(&picture, &tag))
	{
		return PublishEmptyOutput(*frame_buffer, *output);
	}
	(void)tag;
	if (picture.width == 0 || picture.height == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t pitch       = AlignUp(picture.width, FRAME_PITCH_ALIGNMENT);
	const uint64_t frame_bytes = FrameBufferSize(picture.width, picture.height);
	if (frame_bytes == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	output->value.codec_type = decoder->codec_type;
	return PublishFrame(*frame_buffer, *output, picture, pitch, frame_bytes);
}

static KYTY_SYSV_ABI int Videodec2Decode(Decoder* decoder, const InputData* input, FrameBuffer* frame_buffer, OutputInfo* output)
{
	PRINT_NAME();
	auto lease = DecoderHandles().Acquire(decoder);
	if (!lease)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder handle is invalid\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	InputData                        input_local {};
	GuestStructSnapshot<FrameBuffer> frame_buffer_snapshot;
	GuestStructSnapshot<OutputInfo>  output_snapshot;
	if (!CopyGuestStruct(input, &input_local, "InputData") ||
	    !CopyGuestStructForOutput(frame_buffer, &frame_buffer_snapshot, "FrameBuffer") ||
	    !CopyGuestStructForOutput(output, &output_snapshot, "OutputInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (input_local.au_data == nullptr && input_local.au_size != 0)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 access unit data is null for a nonzero size\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (input_local.au_data != nullptr && input_local.au_size != 0 &&
	    input_local.au_size > static_cast<uint64_t>(std::numeric_limits<int>::max()))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 access unit is too large for the video backend\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::unique_ptr<uint8_t, decltype(&std::free)> access_unit(nullptr, &std::free);
	if (input_local.au_data != nullptr && input_local.au_size != 0)
	{
		access_unit.reset(static_cast<uint8_t*>(std::malloc(static_cast<size_t>(input_local.au_size))));
		if (access_unit == nullptr)
		{
			return LibKernel::KERNEL_ERROR_ENOMEM;
		}
		if (!Core::VirtualMemory::CopyFromGuest(access_unit.get(), reinterpret_cast<uint64_t>(input_local.au_data), input_local.au_size))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 access unit range is not readable\n");
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
	}
	std::lock_guard<std::mutex> lock(lease->mutex);
	if (access_unit != nullptr)
	{
		const int64_t tag = lease->next_tag++;
		if (!lease->backend->Send(access_unit.get(), static_cast<size_t>(input_local.au_size), tag))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decode failed: %s\n", lease->backend->LastError());
		}
	}
	return OutputPicture(lease.get(), &frame_buffer_snapshot, &output_snapshot);
}

static KYTY_SYSV_ABI int Videodec2Flush(Decoder* decoder, FrameBuffer* frame_buffer, OutputInfo* output)
{
	PRINT_NAME();
	auto lease = DecoderHandles().Acquire(decoder);
	if (!lease)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder handle is invalid\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	GuestStructSnapshot<FrameBuffer> frame_buffer_snapshot;
	GuestStructSnapshot<OutputInfo>  output_snapshot;
	if (!CopyGuestStructForOutput(frame_buffer, &frame_buffer_snapshot, "FrameBuffer") ||
	    !CopyGuestStructForOutput(output, &output_snapshot, "OutputInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard<std::mutex> lock(lease->mutex);
	if (!lease->backend->Drain())
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 flush failed: %s\n", lease->backend->LastError());
	}
	return OutputPicture(lease.get(), &frame_buffer_snapshot, &output_snapshot);
}

static KYTY_SYSV_ABI int Videodec2Reset(Decoder* decoder)
{
	PRINT_NAME();
	auto lease = DecoderHandles().Acquire(decoder);
	if (!lease)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder handle is invalid\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard<std::mutex> lock(lease->mutex);
	lease->backend->Reset();
	return OK;
}

} // namespace Videodec2

LIB_DEFINE(InitVideodec2_1)
{
	LIB_FUNC("RnDibcGCPKw", Videodec2::Videodec2QueryComputeMemoryInfo);
	LIB_FUNC("eD+X2SmxUt4", Videodec2::Videodec2AllocateComputeQueue);
	LIB_FUNC("UvtA3FAiF4Y", Videodec2::Videodec2ReleaseComputeQueue);
	LIB_FUNC("qqMCwlULR+E", Videodec2::Videodec2QueryDecoderMemoryInfo);
	LIB_FUNC("CNNRoRYd8XI", Videodec2::Videodec2CreateDecoder);
	LIB_FUNC("jwImxXRGSKA", Videodec2::Videodec2DeleteDecoder);
	LIB_FUNC("852F5+q6+iM", Videodec2::Videodec2Decode);
	LIB_FUNC("l1hXwscLuCY", Videodec2::Videodec2Flush);
	LIB_FUNC("wJXikG6QFN8", Videodec2::Videodec2Reset);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
