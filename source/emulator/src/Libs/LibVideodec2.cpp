#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

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

struct PictureSource
{
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct Decoder
{
	std::mutex                                       mutex;
	uint32_t                                         codec_type = 0;
	std::unique_ptr<Backend::ElementaryVideoDecoder> backend;
	int64_t                                          next_tag = 0;
	std::map<int64_t, PictureSource>                 sources;
};

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1u) / alignment * alignment;
}

// NV12 with the chroma rows right after the luma rows, both at the same pitch.
static uint64_t FrameBufferSize(uint32_t width, uint32_t height)
{
	const uint64_t pitch = AlignUp(width, FRAME_PITCH_ALIGNMENT);
	return pitch * height + pitch * ((height + 1u) / 2u);
}

static bool SizeIs(uint64_t this_size, uint64_t expected, const char* name)
{
	if (this_size == expected)
	{
		return true;
	}
	KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 %s size %" PRIu64 " is not the known %" PRIu64 "\n", name, this_size,
	               expected);
	return false;
}

static KYTY_SYSV_ABI int Videodec2QueryComputeMemoryInfo(ComputeMemoryInfo* info)
{
	PRINT_NAME();
	if (info == nullptr || !SizeIs(info->this_size, sizeof(ComputeMemoryInfo), "ComputeMemoryInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Decoding runs on the host; the compute queue needs no guest memory.
	info->compute_memory_size = 0;
	info->compute_memory      = nullptr;
	return OK;
}

static KYTY_SYSV_ABI int Videodec2AllocateComputeQueue(const ComputeConfigInfo* config, const ComputeMemoryInfo* memory,
                                                       ComputeQueue** queue)
{
	PRINT_NAME();
	if (config == nullptr || memory == nullptr || queue == nullptr ||
	    !SizeIs(config->this_size, sizeof(ComputeConfigInfo), "ComputeConfigInfo") ||
	    !SizeIs(memory->this_size, sizeof(ComputeMemoryInfo), "ComputeMemoryInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	*queue = new ComputeQueue {config->compute_pipe_id, config->compute_queue_id};
	return OK;
}

static KYTY_SYSV_ABI int Videodec2ReleaseComputeQueue(ComputeQueue* queue)
{
	PRINT_NAME();
	if (queue == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	delete queue;
	return OK;
}

static KYTY_SYSV_ABI int Videodec2QueryDecoderMemoryInfo(const DecoderConfigInfo* config, DecoderMemoryInfo* memory)
{
	PRINT_NAME();
	if (config == nullptr || memory == nullptr || !SizeIs(config->this_size, sizeof(DecoderConfigInfo), "DecoderConfigInfo") ||
	    !SizeIs(memory->this_size, sizeof(DecoderMemoryInfo), "DecoderMemoryInfo") || config->max_frame_width <= 0 ||
	    config->max_frame_height <= 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	KYTY_LOG_DEBUG("\t codec = %u, profile = %u, level = %u, max = %dx%d, dpb = %d\n", config->codec_type, config->profile,
	               config->max_level, config->max_frame_width, config->max_frame_height, config->max_dpb_frame_count);
	// The host decoder keeps its own state; only the frames live in guest memory.
	memory->cpu_memory_size        = 0;
	memory->gpu_memory_size        = 0;
	memory->cpu_gpu_memory_size    = 0;
	memory->max_frame_buffer_size  = FrameBufferSize(static_cast<uint32_t>(config->max_frame_width),
	                                                  static_cast<uint32_t>(config->max_frame_height));
	memory->frame_buffer_alignment = FRAME_BUFFER_ALIGNMENT;
	return OK;
}

static KYTY_SYSV_ABI int Videodec2CreateDecoder(const DecoderConfigInfo* config, const DecoderMemoryInfo* memory, Decoder** decoder)
{
	PRINT_NAME();
	if (config == nullptr || memory == nullptr || decoder == nullptr ||
	    !SizeIs(config->this_size, sizeof(DecoderConfigInfo), "DecoderConfigInfo") ||
	    !SizeIs(memory->this_size, sizeof(DecoderMemoryInfo), "DecoderMemoryInfo") ||
	    (config->codec_type != CODEC_AVC && config->codec_type != CODEC_HEVC))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::string error;
	auto        backend = Backend::ElementaryVideoDecoder::Open(
	           config->codec_type == CODEC_HEVC ? Backend::VideoCodec::Hevc : Backend::VideoCodec::Avc, &error);
	if (backend == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decoder unavailable: %s\n", error.c_str());
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	auto* created       = new Decoder;
	created->codec_type = config->codec_type;
	created->backend    = std::move(backend);
	*decoder            = created;
	return OK;
}

static KYTY_SYSV_ABI int Videodec2DeleteDecoder(Decoder* decoder)
{
	PRINT_NAME();
	if (decoder == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	delete decoder;
	return OK;
}

// Writes the next decoded picture, if any, into the guest frame buffer.
static int OutputPicture(Decoder* decoder, FrameBuffer* frame_buffer, OutputInfo* output)
{
	output->is_valid          = false;
	output->is_error_frame    = false;
	output->picture_count     = 0;
	frame_buffer->is_accepted = false;

	Backend::VideoFrame picture;
	int64_t             tag = 0;
	if (!decoder->backend->Receive(&picture, &tag))
	{
		return OK;
	}
	decoder->sources.erase(tag);
	const uint32_t pitch       = AlignUp(picture.width, FRAME_PITCH_ALIGNMENT);
	const uint64_t frame_bytes = FrameBufferSize(picture.width, picture.height);
	if (frame_buffer->frame_buffer == nullptr || frame_buffer->frame_buffer_size < frame_bytes)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 frame buffer of %" PRIu64 " bytes is smaller than %" PRIu64 "\n",
		               frame_buffer->frame_buffer_size, frame_bytes);
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	auto* destination = static_cast<uint8_t*>(frame_buffer->frame_buffer);
	{
		const Emulator::VideoFrameMemory::HostWriteLease write_lease(reinterpret_cast<uint64_t>(destination), frame_bytes);
		const uint32_t                                   rows = picture.height + (picture.height + 1u) / 2u;
		for (uint32_t row = 0; row < rows; ++row)
		{
			std::memcpy(destination + static_cast<uint64_t>(row) * pitch,
			            picture.data.data() + static_cast<uint64_t>(row) * picture.width, picture.width);
		}
	}
	Emulator::VideoFrameMemory::RegisterLinearFrame(reinterpret_cast<uint64_t>(destination), frame_bytes, pitch);

	frame_buffer->is_accepted = true;
	output->is_valid          = true;
	output->picture_count     = 1;
	output->codec_type        = decoder->codec_type;
	output->frame_width       = picture.width;
	output->frame_pitch       = pitch;
	output->frame_height      = picture.height;
	output->frame_buffer      = destination;
	output->frame_buffer_size = frame_bytes;
	return OK;
}

static KYTY_SYSV_ABI int Videodec2Decode(Decoder* decoder, const InputData* input, FrameBuffer* frame_buffer, OutputInfo* output)
{
	PRINT_NAME();
	if (decoder == nullptr || input == nullptr || frame_buffer == nullptr || output == nullptr ||
	    !SizeIs(input->this_size, sizeof(InputData), "InputData") || !SizeIs(frame_buffer->this_size, sizeof(FrameBuffer), "FrameBuffer") ||
	    !SizeIs(output->this_size, sizeof(OutputInfo), "OutputInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard<std::mutex> lock(decoder->mutex);
	if (input->au_data != nullptr && input->au_size != 0)
	{
		const int64_t tag     = decoder->next_tag++;
		decoder->sources[tag] = {input->pts_data, input->dts_data, input->attached_data};
		if (!decoder->backend->Send(static_cast<const uint8_t*>(input->au_data), input->au_size, tag))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 decode failed: %s\n", decoder->backend->LastError());
			decoder->sources.erase(tag);
		}
	}
	return OutputPicture(decoder, frame_buffer, output);
}

static KYTY_SYSV_ABI int Videodec2Flush(Decoder* decoder, FrameBuffer* frame_buffer, OutputInfo* output)
{
	PRINT_NAME();
	if (decoder == nullptr || frame_buffer == nullptr || output == nullptr ||
	    !SizeIs(frame_buffer->this_size, sizeof(FrameBuffer), "FrameBuffer") || !SizeIs(output->this_size, sizeof(OutputInfo), "OutputInfo"))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard<std::mutex> lock(decoder->mutex);
	if (!decoder->backend->Drain())
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Videodec2 flush failed: %s\n", decoder->backend->LastError());
	}
	return OutputPicture(decoder, frame_buffer, output);
}

static KYTY_SYSV_ABI int Videodec2Reset(Decoder* decoder)
{
	PRINT_NAME();
	if (decoder == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard<std::mutex> lock(decoder->mutex);
	decoder->backend->Reset();
	decoder->sources.clear();
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
