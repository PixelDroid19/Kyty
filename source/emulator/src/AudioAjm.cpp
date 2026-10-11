#include "Emulator/Audio.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {
namespace Ajm {

LIB_NAME("Ajm", "Ajm");

namespace {

constexpr int32_t  AJM_ERROR_INVALID_CONTEXT          = static_cast<int32_t>(0x80930002u);
constexpr int32_t  AJM_ERROR_INVALID_INSTANCE         = static_cast<int32_t>(0x80930003u);
constexpr int32_t  AJM_ERROR_INVALID_BATCH            = static_cast<int32_t>(0x80930004u);
constexpr int32_t  AJM_ERROR_INVALID_PARAMETER        = static_cast<int32_t>(0x80930005u);
constexpr int32_t  AJM_ERROR_OUT_OF_RESOURCES         = static_cast<int32_t>(0x80930007u);
constexpr int32_t  AJM_ERROR_CODEC_ALREADY_REGISTERED = static_cast<int32_t>(0x80930009u);
constexpr int32_t  AJM_ERROR_CODEC_NOT_REGISTERED     = static_cast<int32_t>(0x8093000au);
constexpr int32_t  AJM_ERROR_WRONG_REVISION_FLAG      = static_cast<int32_t>(0x8093000bu);
constexpr int32_t  AJM_ERROR_MALFORMED_BATCH          = static_cast<int32_t>(0x80930011u);
constexpr uint32_t AJM_MAX_CODEC_TYPE                 = 25;
constexpr int64_t  AJM_GEN5_INITIALIZATION_FLAGS      = INT64_C(0x300000000);
constexpr uint32_t AJM_MAX_INSTANCE_SLOT              = 0x2fff;
constexpr uint32_t AJM_INSTANCE_SLOT_MASK             = 0x3fff;
constexpr uint64_t AJM_FLAG_RUN_GET_CODEC_INFO        = 1ull << 11u;
constexpr uint64_t AJM_FLAG_RUN_MULTIPLE_FRAMES       = 1ull << 12u;
constexpr uint64_t AJM_FLAG_SIDEBAND_RESAMPLE_INFO    = 1ull << 44u;
constexpr uint64_t AJM_FLAG_SIDEBAND_GAPLESS          = 1ull << 45u;
constexpr uint64_t AJM_FLAG_SIDEBAND_FORMAT             = 1ull << 46u;
constexpr uint64_t AJM_FLAG_SIDEBAND_STREAM             = 1ull << 47u;
constexpr uint32_t AJM_SIDEBAND_RESULT_SIZE             = 8u;
constexpr uint32_t AJM_SIDEBAND_RESAMPLE_INFO_SIZE      = 40u;
constexpr uint32_t AJM_DIRECT_RESAMPLE_INFO_PREFIX_SIZE = 16u;
constexpr uint32_t AJM_SIDEBAND_GAPLESS_SIZE            = 8u;
constexpr uint32_t AJM_SIDEBAND_FORMAT_SIZE             = 24u;
constexpr uint32_t AJM_SIDEBAND_STREAM_SIZE             = 16u;
constexpr uint32_t AJM_SIDEBAND_MULTIPLE_FRAMES_SIZE    = 8u;
constexpr size_t   AJM_MAX_QUEUED_BATCHES               = 128u;
constexpr size_t   AJM_MAX_COMPLETED_BATCHES            = 256u;
// This ceiling includes committed decoders and decoders staged for atomic batch preflight.
constexpr size_t AJM_MAX_LIVE_DECODERS             = 64u;
constexpr size_t AJM_MAX_DIRECT_JOB_INPUT_SIZE     = 1024u * 1024u;
constexpr size_t AJM_MAX_DIRECT_BUFFER_DESCRIPTORS = 16u;
constexpr size_t AJM_MAX_DIRECT_SUPERFRAMES        = 64u;
constexpr size_t AJM_MAX_OPUS_PACKETS              = 64u;
// The guest sizes its batch storage itself, so a batch is bounded only by the guest ranges one host transaction publishes:
// the batch error and identifier plus each job's result and outputs.
constexpr size_t AJM_MAX_GUEST_WRITE_PLANS = Core::VirtualMemory::kMaxWritableGuestTransactionRanges;
constexpr size_t AJM_BATCH_WRITE_PLANS     = 2u;

using AjmDecoder = Emulator::AudioVideoBackend::ElementaryAudioDecoder;

constexpr uint32_t AJM_MAX_OPUS_PENDING_FRAMES = AjmDecoder::kOpusMaxPacketSamples;

struct AjmGaplessDecodeState
{
	bool     configured               = false;
	bool     has_decoded              = false;
	uint32_t total_output_frames      = 0;
	uint32_t priming_frames_remaining = 0;
	uint32_t output_frames            = 0;
};

// Linear interpolation needs the source frame at the current position and, between frames, the next one.
constexpr uint32_t AJM_RESAMPLE_HELD_FRAMES        = 2u;
constexpr uint32_t AJM_RESAMPLE_MAX_CHANNELS       = AjmDecoder::kAtrac9MaxChannels;
constexpr uint32_t AJM_OPUS_SPLIT_MAX_CHANNELS     = 2u;

struct AjmResampleState
{
	bool                        configured              = false;
	float                       ratio_start             = 1.0f;
	float                       ratio_change_per_sample = 0.0f;
	uint32_t                    flags                   = 0;
	// Clear opens the window in which the guest restarts a stream with the reset marker; a profile or a decode closes it.
	bool reset_window = false;
	// Output frames since the setter: the ratio moves by ratio_change_per_sample per output frame from ratio_start.
	uint64_t profile_frames = 0;
	// Clear dropped a non-neutral profile; the stream needs a new one before it decodes.
	bool profile_required = false;
	// Source position relative to the first held frame; held frames entered the resampler but are not consumed yet.
	double                                                                   source_position = 0.0;
	uint32_t                                                                 held_frames     = 0;
	std::array<float, AJM_RESAMPLE_HELD_FRAMES * AJM_RESAMPLE_MAX_CHANNELS> held_samples {};
};

struct AjmInstanceState
{
	uint32_t                    codec = 0;
	uint64_t                    flags = 0;
	std::array<uint8_t, 12>     config {};
	std::unique_ptr<AjmDecoder> decoder;
	uint32_t                    channels    = 0;
	uint32_t                    sample_rate = 0;
	uint32_t                    block_align = 0;
	std::vector<float>          pending_pcm;
	size_t                      pending_sample_offset = 0;
	uint64_t                    delivered_samples     = 0;
	AjmGaplessDecodeState       gapless;
	AjmResampleState            resample;
};

struct AjmContextState
{
	std::unordered_set<uint32_t>                   registered_codecs;
	std::unordered_map<uint32_t, uint32_t>         instances_by_slot;
	std::unordered_map<uint32_t, AjmInstanceState> instances;
	std::unordered_set<uint32_t>                   completed_batches;
	uint32_t                                       next_instance_slot = 0;
	uint32_t                                       next_batch_id      = 0;
};

enum class AjmDataOperation : uint8_t
{
	Initialize,
	Clear,
	Decode,
	SetGaplessDecode,
	GetGaplessDecode,
	SetResampleParameters,
	SetResampleParametersEx,
	GetResampleInfo,
	GetInfo,
	GetCodecInfo,
	GetStatistics,
	Control,
	DecodeSingle,
	DecodeSplit,
	Encode,
	Run,
	RunSplit,
};

struct AjmBatch2DataJob
{
	AjmDataOperation        operation      = AjmDataOperation::Decode;
	uint32_t                instance       = 0;
	uint64_t                input_address  = 0;
	uint64_t                input_size     = 0;
	uint64_t                output_address = 0;
	uint64_t                output_size    = 0;
	uint64_t                result_address = 0;
	uint64_t                gapless_reset  = 0;
	std::array<uint8_t, 8>  gapless_config {};
	float ratio_start = 1.0f;
	float ratio_change_per_sample = 0.0f;
	uint32_t resample_flags = 0;
	std::array<uint8_t, 12> config {};
	std::vector<AjmBuffer> input_buffers;
	std::vector<AjmBuffer> output_buffers;
};

struct AjmQueuedBatch
{
	std::vector<AjmBatch2DataJob> jobs;
	size_t                        write_plans = AJM_BATCH_WRITE_PLANS;
};

std::mutex                                                     g_ajm_mutex;
std::unordered_map<uint32_t, AjmContextState>                  g_ajm_contexts;
std::unordered_map<uintptr_t, AjmQueuedBatch>                  g_ajm_batch2_jobs;
size_t                                                         g_ajm_live_decoder_count = 0;

constexpr size_t AJM_MAX_BATCH2_BUFFER_SIZE = 64u * 1024u * 1024u;

bool ValidateAjmBuffers(const AjmBuffer* buffers, size_t count, uint32_t* total_size)
{
	if (total_size == nullptr || (buffers == nullptr && count != 0))
	{
		return false;
	}
	uint64_t total = 0;
	for (size_t index = 0; index < count; index++)
	{
		if (buffers[index].address == nullptr || buffers[index].size > AJM_MAX_BATCH2_BUFFER_SIZE)
		{
			return false;
		}
		total += buffers[index].size;
		if (total > AJM_MAX_BATCH2_BUFFER_SIZE)
		{
			return false;
		}
	}
	*total_size = static_cast<uint32_t>(total);
	return true;
}

bool SnapshotAjmBuffers(const AjmBuffer* buffers, size_t count, size_t max_total_size, std::vector<AjmBuffer>* snapshot,
                        uint64_t* total_size)
{
	if (snapshot == nullptr || total_size == nullptr || count > AJM_MAX_DIRECT_BUFFER_DESCRIPTORS ||
	    (buffers == nullptr && count != 0))
	{
		return false;
	}

	snapshot->resize(count);
	if (count != 0 && !Core::VirtualMemory::CopyFromGuest(snapshot->data(), reinterpret_cast<uint64_t>(buffers), count * sizeof(AjmBuffer)))
	{
		return false;
	}

	uint64_t total = 0;
	for (const auto& buffer: *snapshot)
	{
		if (buffer.address == nullptr || buffer.size > max_total_size || total > max_total_size - buffer.size)
		{
			return false;
		}
		total += buffer.size;
	}
	*total_size = total;
	return true;
}

const char* AjmDataOperationName(AjmDataOperation operation)
{
	switch (operation)
	{
		case AjmDataOperation::Initialize: return "initialize";
		case AjmDataOperation::Clear: return "clear";
		case AjmDataOperation::Decode: return "decode";
		case AjmDataOperation::SetGaplessDecode: return "set-gapless-decode";
		case AjmDataOperation::GetGaplessDecode: return "get-gapless-decode";
		case AjmDataOperation::SetResampleParameters: return "set-resample-parameters";
		case AjmDataOperation::SetResampleParametersEx: return "set-resample-parameters-ex";
		case AjmDataOperation::GetResampleInfo: return "get-resample-info";
		case AjmDataOperation::GetInfo: return "get-info";
		case AjmDataOperation::GetCodecInfo: return "get-codec-info";
		case AjmDataOperation::GetStatistics: return "get-statistics";
		case AjmDataOperation::Control: return "control";
		case AjmDataOperation::DecodeSingle: return "decode-single";
		case AjmDataOperation::DecodeSplit: return "decode-split";
		case AjmDataOperation::Encode: return "encode";
		case AjmDataOperation::Run: return "run";
		case AjmDataOperation::RunSplit: return "run-split";
	}
	return "unknown";
}

[[noreturn]] void RejectUnsupportedAjmDataOperation(AjmDataOperation operation)
{
	EXIT("AJM %s operation is not implemented\n", AjmDataOperationName(operation));
	std::abort();
}

[[noreturn]] void RejectMalformedAjmData(const char* reason)
{
	EXIT("AJM batch rejected: %s\n", reason);
	std::abort();
}

// Upper bound of the guest ranges a job publishes; every job is charged at least one so the job count stays bounded too.
size_t AjmJobWritePlanBound(const AjmBatch2DataJob& job)
{
	const size_t plans = job.output_buffers.size() + (job.output_address != 0 ? 1u : 0u) + (job.result_address != 0 ? 1u : 0u);
	return std::max<size_t>(plans, 1u);
}

bool QueueAjmDataJob(AjmBatch2DataJob job, void* batch)
{
	if (batch == nullptr || (job.instance == 0 && job.operation != AjmDataOperation::GetStatistics))
	{
		return false;
	}

	std::unique_lock lock(g_ajm_mutex);
	const uintptr_t  key = reinterpret_cast<uintptr_t>(batch);
	auto             it  = g_ajm_batch2_jobs.find(key);
	if (it == g_ajm_batch2_jobs.end())
	{
		if (g_ajm_batch2_jobs.size() >= AJM_MAX_QUEUED_BATCHES)
		{
			return false;
		}
		it = g_ajm_batch2_jobs.emplace(key, AjmQueuedBatch {}).first;
	}
	// The guest ignores enqueue errors and builds later batches on the dropped jobs, so a full host batch must stop here.
	const size_t job_plans = AjmJobWritePlanBound(job);
	if (job_plans > AJM_MAX_GUEST_WRITE_PLANS - it->second.write_plans)
	{
		lock.unlock();
		RejectMalformedAjmData("batch exceeds the host write-range capacity");
	}
	it->second.write_plans += job_plans;
	it->second.jobs.push_back(std::move(job));
	return true;
}

int QueueUnsupportedAjmControl(void* batch, uint32_t instance, AjmDataOperation operation, const void* input, void* output,
                               size_t output_size = 0, size_t input_size = 0)
{
	AjmBatch2DataJob job {};
	job.operation      = operation;
	job.instance       = instance;
	job.input_address  = reinterpret_cast<uint64_t>(input);
	job.input_size     = input_size;
	job.output_address = reinterpret_cast<uint64_t>(output);
	job.output_size    = output_size;
	job.result_address = reinterpret_cast<uint64_t>(output);
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

bool IsAjmInstanceIdInUse(uint32_t instance)
{
	const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
	for (const auto& [context_id, context]: g_ajm_contexts)
	{
		(void)context_id;
		auto instance_it = context.instances_by_slot.find(slot);
		if (instance_it != context.instances_by_slot.end() && instance_it->second == instance)
		{
			return true;
		}
	}
	return false;
}

AjmInstanceState* FindAjmInstanceState(AjmContextState& context, uint32_t instance)
{
	const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
	auto           slot_it = context.instances_by_slot.find(slot);
	if (slot == 0 || slot_it == context.instances_by_slot.end() || slot_it->second != instance)
	{
		return nullptr;
	}
	auto state_it = context.instances.find(instance);
	return state_it == context.instances.end() ? nullptr : &state_it->second;
}

const AjmInstanceState* FindAjmInstanceState(const AjmContextState& context, uint32_t instance)
{
	const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
	auto           slot_it = context.instances_by_slot.find(slot);
	if (slot == 0 || slot_it == context.instances_by_slot.end() || slot_it->second != instance)
	{
		return nullptr;
	}
	auto state_it = context.instances.find(instance);
	return state_it == context.instances.end() ? nullptr : &state_it->second;
}

void RemoveQueuedAjmJobs(uint32_t instance)
{
	for (auto batch_it = g_ajm_batch2_jobs.begin(); batch_it != g_ajm_batch2_jobs.end();)
	{
		auto& jobs = batch_it->second.jobs;
		jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [instance](const auto& job) { return job.instance == instance; }), jobs.end());
		if (jobs.empty())
		{
			batch_it = g_ajm_batch2_jobs.erase(batch_it);
		} else
		{
			++batch_it;
		}
	}
}

uint32_t AllocateContextId()
{
	uint32_t id = 1;
	while (g_ajm_contexts.find(id) != g_ajm_contexts.end())
	{
		++id;
	}
	return id;
}

constexpr uint32_t MakeAjmChunkHeader(uint32_t identifier, uint32_t payload = 0)
{
	return (identifier & 0x3fu) | ((payload & 0xfffffu) << 6u);
}

void WriteAjmU32(uint8_t* destination, uint32_t value)
{
	std::memcpy(destination, &value, sizeof(value));
}

uint32_t ReadAjmU32(const uint8_t* source)
{
	uint32_t value = 0;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

void* ReadAjmPointer(const uint8_t* source)
{
	void* value = nullptr;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

void WriteAjmPointer(uint8_t* destination, const void* value)
{
	std::memcpy(destination, &value, sizeof(value));
}

uint8_t* WriteAjmBufferChunk(uint8_t* destination, uint32_t identifier, void* address, size_t size)
{
	if (size > UINT32_MAX)
	{
		return nullptr;
	}
	WriteAjmU32(destination, MakeAjmChunkHeader(identifier));
	WriteAjmU32(destination + 4u, static_cast<uint32_t>(size));
	WriteAjmPointer(destination + 8u, address);
	return destination + 16u;
}

struct AjmOutputChunk
{
	void*    address = nullptr;
	uint32_t size    = 0;
	bool     control = false;
};

enum class AjmBufferBatchOperation : uint8_t
{
	Control,
	Run,
};

struct AjmParsedBufferJob
{
	AjmBufferBatchOperation     operation   = AjmBufferBatchOperation::Control;
	uint32_t                    instance    = 0;
	uint64_t                    flags       = 0;
	uint64_t                    input_size  = 0;
	uint64_t                    output_size = 0;
	std::vector<AjmOutputChunk> outputs;
};

enum class AjmBufferBatchStatus : uint8_t
{
	Ready,
	Malformed,
	UnsupportedDataOperation,
};

uint32_t AjmCodecInfoSize(uint32_t instance)
{
	switch (instance >> 14u)
	{
		case 0u: return 16u;
		case 1u: return 16u;
		case 2u: return 8u;
		case 24u: return 4u;
		default: return 0u;
	}
}

void WriteAjmRunSideband(uint64_t flags, void* output, uint32_t output_size, uint32_t instance, uint64_t input_size,
                         uint64_t data_output_size, uint64_t total_decoded_samples)
{
	if (output == nullptr || output_size == 0)
	{
		return;
	}

	std::memset(output, 0, output_size);
	auto*    sideband        = static_cast<uint8_t*>(output);
	uint32_t sideband_offset = AJM_SIDEBAND_RESULT_SIZE;

	if ((flags & AJM_FLAG_RUN_GET_CODEC_INFO) != 0)
	{
		const uint32_t codec_info_size = AjmCodecInfoSize(instance);
		if (codec_info_size != 0 && output_size >= sideband_offset + codec_info_size)
		{
			sideband_offset += codec_info_size;
		}
	}
	if ((flags & AJM_FLAG_SIDEBAND_RESAMPLE_INFO) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_RESAMPLE_INFO_SIZE)
	{
		const float ratio = 1.0f;
		std::memcpy(sideband + sideband_offset, &ratio, sizeof(ratio));
		sideband_offset += AJM_SIDEBAND_RESAMPLE_INFO_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_GAPLESS) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_GAPLESS_SIZE)
	{
		sideband_offset += AJM_SIDEBAND_GAPLESS_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_FORMAT) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_FORMAT_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset + 0u, 2u);
		WriteAjmU32(sideband + sideband_offset + 4u, 3u);
		WriteAjmU32(sideband + sideband_offset + 8u, 48000u);
		sideband_offset += AJM_SIDEBAND_FORMAT_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_STREAM) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_STREAM_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset, static_cast<uint32_t>(std::min<uint64_t>(input_size, INT32_MAX)));
		WriteAjmU32(sideband + sideband_offset + 4u, static_cast<uint32_t>(std::min<uint64_t>(data_output_size, INT32_MAX)));
		std::memcpy(sideband + sideband_offset + 8u, &total_decoded_samples, sizeof(total_decoded_samples));
		sideband_offset += AJM_SIDEBAND_STREAM_SIZE;
	}
	if ((flags & AJM_FLAG_RUN_MULTIPLE_FRAMES) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_MULTIPLE_FRAMES_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset, input_size != 0 ? 1u : 0u);
	}
}

bool ParseAjmJobBuffer(const uint8_t* job, uint32_t job_size, uint32_t instance, const AjmContextState& context,
                       AjmParsedBufferJob* parsed)
{
	if (job == nullptr || parsed == nullptr)
	{
		return false;
	}
	if (instance != 0x80000u)
	{
		const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
		auto           slot_it = context.instances_by_slot.find(slot);
		if (slot == 0 || slot_it == context.instances_by_slot.end() || slot_it->second != instance)
		{
			return false;
		}
	}

	parsed->instance = instance;
	uint32_t offset  = 0;
	while (offset < job_size)
	{
		if (job_size - offset < sizeof(uint32_t))
		{
			return false;
		}
		const uint32_t header     = ReadAjmU32(job + offset);
		const uint32_t identifier = header & 0x3fu;
		if (identifier == 3u || identifier == 4u)
		{
			if (job_size - offset < 8u)
			{
				return false;
			}
			if (identifier == 4u)
			{
				parsed->operation = AjmBufferBatchOperation::Run;
			}
			parsed->flags = (static_cast<uint64_t>((header >> 6u) & 0xfffffu) << 32u) | ReadAjmU32(job + offset + 4u);
			offset += 8u;
			continue;
		}
		if (identifier != 1u && identifier != 2u && identifier != 6u && identifier != 17u && identifier != 18u)
		{
			return false;
		}
		if (job_size - offset < 16u)
		{
			return false;
		}
		const uint32_t size    = ReadAjmU32(job + offset + 4u);
		void* const    address = ReadAjmPointer(job + offset + 8u);
		if (size != 0 && address == nullptr)
		{
			return false;
		}
		if (identifier == 1u)
		{
			parsed->input_size += size;
		} else if (identifier == 17u)
		{
			parsed->output_size += size;
			parsed->outputs.push_back({address, size, false});
		} else if (identifier == 18u)
		{
			parsed->outputs.push_back({address, size, true});
		}
		offset += 16u;
	}

	return true;
}

AjmBufferBatchStatus ParseAjmBatchBuffer(const uint8_t* batch, uint32_t batch_size, const AjmContextState& context,
                                         std::vector<AjmParsedBufferJob>* parsed_jobs)
{
	if (batch == nullptr || parsed_jobs == nullptr)
	{
		return AjmBufferBatchStatus::Malformed;
	}
	bool     unsupported_data_operation = false;
	uint32_t offset = 0;
	while (offset < batch_size)
	{
		if (batch_size - offset < 8u)
		{
			return AjmBufferBatchStatus::Malformed;
		}
		const uint32_t header     = ReadAjmU32(batch + offset);
		const uint32_t identifier = header & 0x3fu;
		const uint32_t payload    = (header >> 6u) & 0xfffffu;
		const uint32_t size       = ReadAjmU32(batch + offset + 4u);
		offset += 8u;
		if (size > batch_size - offset)
		{
			return AjmBufferBatchStatus::Malformed;
		}
		if (identifier == 0u)
		{
			AjmParsedBufferJob parsed_job {};
			if (!ParseAjmJobBuffer(batch + offset, size, payload, context, &parsed_job))
			{
				return AjmBufferBatchStatus::Malformed;
			}
			unsupported_data_operation |= parsed_job.operation == AjmBufferBatchOperation::Run;
			parsed_jobs->push_back(std::move(parsed_job));
		} else if (identifier != 7u)
		{
			return AjmBufferBatchStatus::Malformed;
		}
		offset += size;
	}
	if (offset != batch_size)
	{
		return AjmBufferBatchStatus::Malformed;
	}
	return unsupported_data_operation ? AjmBufferBatchStatus::UnsupportedDataOperation : AjmBufferBatchStatus::Ready;
}

void ApplyAjmBatchBuffer(const std::vector<AjmParsedBufferJob>& parsed_jobs)
{
	for (const auto& job: parsed_jobs)
	{
		for (const auto& output: job.outputs)
		{
			if (output.size == 0)
			{
				continue;
			}
			std::memset(output.address, 0, output.size);
			if (output.control)
			{
				WriteAjmRunSideband(job.flags, output.address, output.size, job.instance, job.input_size, job.output_size,
				                    job.output_size / 4u);
			}
		}
	}
}

struct AjmDecodeResultPrefix
{
	int32_t  status                = 0;
	int32_t  codec_result          = 0;
	int32_t  input_bytes_consumed  = 0;
	int32_t  output_bytes_produced = 0;
	uint64_t delivered_samples     = 0;
};

static_assert(sizeof(AjmDecodeResultPrefix) == 24);

struct AjmStagedGuestWrite
{
	size_t               plan_index = 0;
	std::vector<uint8_t> bytes;
};

struct AjmGuestWritePlan
{
	uint64_t                                  address = 0;
	uint64_t                                  size    = 0;
	Core::VirtualMemory::GuestMappingSnapshot mapping {};
};

struct AjmOpusPacket
{
	size_t   record_offset       = 0;
	size_t   payload_offset      = 0;
	size_t   payload_size        = 0;
	uint32_t samples_per_channel = 0;
};

struct AjmOpusDecodePlan
{
	size_t                 packet_count            = 0;
	uint64_t               input_bytes_consumed   = 0;
	uint32_t               output_frames_produced = 0;
	uint32_t               pending_frames_after   = 0;
	uint64_t               admitted_frames        = 0;
	AjmGaplessDecodeState gapless_after {};
	AjmResampleState      resample_after {};
};

struct AjmPreparedBatchJob
{
	const AjmBatch2DataJob*     queued = nullptr;
	std::unique_ptr<AjmDecoder> decoder;
	std::vector<uint8_t>        input_data;
	std::vector<size_t>         output_plans;
	std::vector<AjmOpusPacket>  opus_packets;
	AjmOpusDecodePlan           opus_plan {};
	size_t                      output_plan = SIZE_MAX;
	size_t                      result_plan = SIZE_MAX;
};

struct AjmResampleInfoPrefix
{
	int32_t result          = 0;
	int32_t internal_result = 0;
	float   ratio           = 1.0f;
	int32_t sample_count    = 0;
};

static_assert(sizeof(AjmResampleInfoPrefix) == AJM_DIRECT_RESAMPLE_INFO_PREFIX_SIZE);

struct AjmDecoderFormat
{
	uint32_t codec       = 0;
	uint32_t channels    = 0;
	uint32_t sample_rate = 0;
	uint32_t block_align = 0;
};

enum class AjmOpusPacketScanResult : uint8_t
{
	Valid,
	Malformed,
	TooManyPackets,
};

struct AjmOpusScanBoundary
{
	uint32_t invalid_offset     = 0;
	uint32_t declared_size      = 0;
	uint32_t available_bytes    = 0;
	uint32_t valid_prefix_bytes = 0;
};

struct AjmGuestWriteTransactionContext
{
	const std::vector<AjmGuestWritePlan>*   plans  = nullptr;
	const std::vector<AjmStagedGuestWrite>* writes = nullptr;
};

bool AjmAddressRangeOverlaps(uint64_t first_address, uint64_t first_size, uint64_t second_address, uint64_t second_size)
{
	if (first_size == 0 || second_size == 0)
	{
		return false;
	}
	if (first_address <= second_address)
	{
		return second_address - first_address < first_size;
	}
	return first_address - second_address < second_size;
}

bool AddAjmGuestWritePlan(std::vector<AjmGuestWritePlan>* plans, uint64_t address, uint64_t size, size_t* plan_index)
{
	if (plans == nullptr || plan_index == nullptr || size == 0 || address == 0 || address > UINT64_MAX - size ||
	    size > Core::VirtualMemory::GuestMappingSnapshot::kMaxBytes ||
	    plans->size() >= AJM_MAX_GUEST_WRITE_PLANS)
	{
		return false;
	}
	for (const auto& plan: *plans)
	{
		if (AjmAddressRangeOverlaps(address, size, plan.address, plan.size))
		{
			return false;
		}
	}
	AjmGuestWritePlan plan {};
	plan.address = address;
	plan.size    = size;
	if (!Core::VirtualMemory::CaptureGuestMappingSnapshot(address, size, &plan.mapping))
	{
		return false;
	}
	*plan_index = plans->size();
	plans->push_back(std::move(plan));
	return true;
}

void AjmNoopWritableVisitor(const Core::VirtualMemory::GuestMappingIdentitySnapshot*, void*) noexcept {}

bool VisitAjmWritablePlans(const std::vector<AjmGuestWritePlan>& plans, Core::VirtualMemory::WritableGuestTransactionVisitor visitor,
                           void* context)
{
	if (plans.empty())
	{
		return true;
	}
	if (plans.size() > AJM_MAX_GUEST_WRITE_PLANS)
	{
		return false;
	}
	auto leases = std::make_unique<std::optional<Emulator::VideoFrameMemory::HostWriteLease>[]>(plans.size());
	std::vector<Core::VirtualMemory::WritableGuestTransactionRange> ranges(plans.size());
	for (size_t i = 0; i < plans.size(); ++i)
	{
		leases[i].emplace(plans[i].address, plans[i].size);
		ranges[i] = {plans[i].address, plans[i].size, &plans[i].mapping};
	}
	return Core::VirtualMemory::VisitWritableGuestTransaction(ranges.data(), static_cast<uint32_t>(ranges.size()), -1, visitor,
	                                                          context);
}

void AjmCopyStagedGuestWrites(const Core::VirtualMemory::GuestMappingIdentitySnapshot*, void* context) noexcept
{
	auto* const transaction = static_cast<AjmGuestWriteTransactionContext*>(context);
	if (transaction == nullptr || transaction->plans == nullptr || transaction->writes == nullptr)
	{
		return;
	}
	for (const auto& write: *transaction->writes)
	{
		if (write.plan_index < transaction->plans->size() && !write.bytes.empty())
		{
			const auto& plan = (*transaction->plans)[write.plan_index];
			if (write.bytes.size() <= plan.size)
			{
				std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(plan.address)), write.bytes.data(), write.bytes.size());
			}
		}
	}
}

bool CommitAjmGuestWrites(const std::vector<AjmGuestWritePlan>& plans, const std::vector<AjmStagedGuestWrite>& writes)
{
	for (const auto& write: writes)
	{
		if (write.plan_index >= plans.size() || write.bytes.size() > plans[write.plan_index].size)
		{
			return false;
		}
	}
	AjmGuestWriteTransactionContext transaction {&plans, &writes};
	return VisitAjmWritablePlans(plans, &AjmCopyStagedGuestWrites, &transaction);
}

bool AjmGuestRangeIsReadable(uint64_t address, uint64_t size)
{
	return size == 0 || (address != 0 && address <= UINT64_MAX - size && Core::VirtualMemory::IsRangeReadable(address, size));
}

uint32_t AjmReadLittleEndianU32(const uint8_t* source)
{
	return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8u) | (static_cast<uint32_t>(source[2]) << 16u) |
	       (static_cast<uint32_t>(source[3]) << 24u);
}

uint16_t AjmReadLittleEndianU16(const uint8_t* source)
{
	return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8u);
}

uint64_t AjmGaplessRemainingFrames(const AjmGaplessDecodeState& state)
{
	return state.configured ? static_cast<uint64_t>(state.total_output_frames) - state.output_frames : UINT64_MAX;
}

// The setter is admitted only before the stream decodes, so no pending PCM or decoder history exists and reset values 0
// and 1 configure the same fresh stream. Both fields are per-channel frames; total counts the frames after priming.
bool ApplyAjmGaplessConfiguration(AjmGaplessDecodeState* state, const std::array<uint8_t, 8>& config, uint64_t reset,
                                  bool reset_after_decode)
{
	const uint32_t total_output_frames  = AjmReadLittleEndianU32(config.data());
	const uint16_t priming_frames       = AjmReadLittleEndianU16(config.data() + 4);
	const uint16_t prior_skipped_frames = AjmReadLittleEndianU16(config.data() + 6);
	// Titles reset the gapless state of decoded ATRAC9 instances to start the accounting of a new stream; otherwise the
	// configuration must precede the first decode.
	if (state == nullptr || reset > 1 || (state->has_decoded && (reset == 0 || !reset_after_decode)) || total_output_frames == 0 ||
	    prior_skipped_frames != 0)
	{
		return false;
	}
	state->configured               = true;
	state->has_decoded              = false;
	state->total_output_frames      = total_output_frames;
	state->priming_frames_remaining = priming_frames;
	state->output_frames            = 0;
	return true;
}

// Removes the priming frames still owed from the start of one decoded unit and returns how many of its remaining frames
// fit the finite output limit; frames beyond the limit are discarded.
uint64_t TrimAjmGaplessUnit(AjmGaplessDecodeState* state, uint32_t unit_frames, uint32_t* skipped_frames)
{
	const uint32_t skipped = std::min(unit_frames, state->priming_frames_remaining);
	state->priming_frames_remaining -= skipped;
	state->has_decoded = true;
	*skipped_frames    = skipped;
	return std::min<uint64_t>(unit_frames - skipped, AjmGaplessRemainingFrames(*state));
}

// Callers never deliver more than AjmGaplessRemainingFrames.
void AddAjmGaplessOutputFrames(AjmGaplessDecodeState* state, uint64_t frames)
{
	if (state->configured)
	{
		state->output_frames += static_cast<uint32_t>(frames);
	}
}

uint32_t AjmOpusSamplesPerFrame(uint32_t toc_config)
{
	static constexpr std::array<uint32_t, 32> samples_per_frame = {480,  960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920,
	                                                               2880, 480, 960,  480,  960, 120, 240,  480,  960, 120, 240,
	                                                               480,  960, 120,  240,  480, 960, 120,  240,  480, 960};
	return samples_per_frame[toc_config & 0x1fu];
}

uint32_t AjmOpusPacketSamplesPerChannel(const uint8_t* packet, size_t packet_size)
{
	if (packet == nullptr || packet_size == 0)
	{
		return 0;
	}
	const uint32_t config             = packet[0] >> 3u;
	const uint32_t samples_per_frame = AjmOpusSamplesPerFrame(config);
	const uint32_t frame_code        = packet[0] & 0x3u;
	uint32_t       frame_count       = frame_code == 0 ? 1u : 2u;
	if (frame_code == 3)
	{
		if (packet_size < 2)
		{
			return 0;
		}
		frame_count = packet[1] & 0x3fu;
		if (frame_count == 0)
		{
			return 0;
		}
	}
	const uint64_t packet_samples = static_cast<uint64_t>(samples_per_frame) * frame_count;
	return packet_samples > AjmDecoder::kOpusMaxPacketSamples ? 0 : static_cast<uint32_t>(packet_samples);
}

AjmOpusPacketScanResult ScanAjmOpusPacketRecords(const std::vector<uint8_t>& input_data, std::vector<AjmOpusPacket>* packets)
{
	if (packets == nullptr)
	{
		return AjmOpusPacketScanResult::Malformed;
	}
	packets->clear();
	for (size_t offset = 0; offset < input_data.size();)
	{
		if (input_data.size() - offset < sizeof(uint16_t))
		{
			return AjmOpusPacketScanResult::Malformed;
		}
		const size_t packet_size    = static_cast<size_t>(input_data[offset]) | (static_cast<size_t>(input_data[offset + 1]) << 8u);
		const size_t payload_offset = offset + sizeof(uint16_t);
		if (packet_size == 0 || packet_size > AjmDecoder::kOpusMaxPacketBytes || packet_size > input_data.size() - payload_offset)
		{
			return AjmOpusPacketScanResult::Malformed;
		}
if (packets->size() >= AJM_MAX_OPUS_PACKETS)
		{
			return AjmOpusPacketScanResult::TooManyPackets;
		}
		const uint32_t samples = AjmOpusPacketSamplesPerChannel(input_data.data() + payload_offset, packet_size);
		if (samples == 0)
		{
			return AjmOpusPacketScanResult::Malformed;
		}
		packets->push_back({offset, payload_offset, packet_size, samples});
		offset = payload_offset + packet_size;
	}
	return AjmOpusPacketScanResult::Valid;
}

// The ratio is the source advance per output frame. It moves linearly from the setter's ratio: the guest sends
// (target - current) / output frames as the change per sample and reads the current ratio back from GetResampleInfo.
double AjmResampleRatio(const AjmResampleState& state)
{
	return static_cast<double>(state.ratio_start) +
	       static_cast<double>(state.ratio_change_per_sample) * static_cast<double>(state.profile_frames);
}

float AjmReportedResampleRatio(const AjmResampleState& state)
{
	return static_cast<float>(AjmResampleRatio(state));
}

// Profiles observed from the guest: a non-negative ratio, constant or moving, that is not frozen at zero, and anything
// but the neutral profile only on a finite (gapless) stream; the reset marker at ratio 1 in the window after a Clear;
// flags 2, which the guest sets on the last calls of a finite stream from the current ratio; and both bits together on a
// short stream whose first call is also its last, where the cleared stream's current ratio is the neutral one. The
// stream end stays the gapless limit.
bool IsAjmResampleProfileAdmitted(const AjmResampleState& running, const AjmGaplessDecodeState& gapless, const AjmBatch2DataJob& job)
{
	const float ratio   = job.ratio_start;
	const float change  = job.ratio_change_per_sample;
	const bool  neutral = ratio == 1.0f && change == 0.0f;
	if (!(ratio >= 0.0f) || (ratio == 0.0f && !(change > 0.0f)) || (!neutral && !gapless.configured))
	{
		return false;
	}
	switch (job.resample_flags)
	{
		case 0u: return true;
		case 1u: return neutral && running.reset_window;
		case 2u: return running.configured && AjmReportedResampleRatio(running) == ratio;
		case 3u: return neutral && running.reset_window && AjmReportedResampleRatio(running) == ratio;
		default: return false;
	}
}

// Every instance observed setting a resample profile has flag 0x1000; the low bits are the revision.
bool AjmInstanceResamples(const AjmInstanceState& instance)
{
	return instance.codec == 24 ? instance.flags == 0x1002u : instance.codec == 1 && (instance.flags & 0x1000u) != 0;
}

// A new profile changes the step of the running stream; its position and held frames carry over.
void ApplyAjmResampleProfile(AjmResampleState* state, const AjmBatch2DataJob& job)
{
	state->configured              = true;
	state->ratio_start             = job.ratio_start;
	state->ratio_change_per_sample = job.ratio_change_per_sample;
	state->flags                   = job.resample_flags;
	state->reset_window            = false;
	state->profile_frames          = 0;
}

// Clear restarts the stream. The guest reuses a cleared neutral instance with no new setter and reads its profile back,
// so the neutral profile survives Clear; any other profile surviving Clear is not observed, so it must be set again.
void ClearAjmResampleState(AjmResampleState* state)
{
	const bool       neutral = state->ratio_start == 1.0f && state->ratio_change_per_sample == 0.0f;
	AjmResampleState cleared {};
	cleared.configured        = state->configured && neutral;
	cleared.flags             = cleared.configured ? state->flags : 0u;
	cleared.profile_required  = state->configured && !neutral;
	cleared.reset_window      = true;
	*state = cleared;
}

// Supplies source frames to the resampler: first the pending PCM, then the packets of one split job in order, each trimmed
// by the gapless state. Without an instance it only counts frames, which is how the batch preflight plans the job.
class AjmOpusSourceFrames
{
public:
	AjmOpusSourceFrames(const std::vector<AjmOpusPacket>& packets, size_t packet_limit, const AjmGaplessDecodeState& gapless,
	                    uint64_t pending_frames, AjmInstanceState* instance, const std::vector<uint8_t>* input)
	    : m_packets(packets), m_packet_limit(std::min(packet_limit, packets.size())), m_gapless(gapless),
	      m_pending_frames(pending_frames), m_instance(instance), m_input(input)
	{
	}

	// Copies the next source frame to frame when it is not null and charges it to the gapless output limit.
	bool Pull(float* frame)
	{
		while (m_pending_frames == 0)
		{
			if (m_failed || m_packets_decoded >= m_packet_limit || AjmGaplessRemainingFrames(m_gapless) == 0 || !DecodeNextPacket())
			{
				return false;
			}
		}
		if (m_instance != nullptr)
		{
			const uint32_t channels = m_instance->channels;
			if (frame != nullptr)
			{
				std::copy_n(m_instance->pending_pcm.begin() + static_cast<ptrdiff_t>(m_instance->pending_sample_offset), channels, frame);
			}
			m_instance->pending_sample_offset += channels;
			if (m_instance->pending_sample_offset == m_instance->pending_pcm.size())
			{
				m_instance->pending_pcm.clear();
				m_instance->pending_sample_offset = 0;
			}
		}
		--m_pending_frames;
		++m_admitted_frames;
		AddAjmGaplessOutputFrames(&m_gapless, 1);
		return true;
	}

	// A finite stream whose output limit is reached holds no further source frames.
	[[nodiscard]] bool Exhausted() const { return m_gapless.configured && AjmGaplessRemainingFrames(m_gapless) == 0; }
	[[nodiscard]] bool Failed() const { return m_failed; }
	[[nodiscard]] size_t PacketsDecoded() const { return m_packets_decoded; }
	[[nodiscard]] uint64_t InputConsumed() const { return m_input_consumed; }
	[[nodiscard]] uint64_t PendingFrames() const { return m_pending_frames; }
	[[nodiscard]] uint64_t AdmittedFrames() const { return m_admitted_frames; }
	[[nodiscard]] const AjmGaplessDecodeState& Gapless() const { return m_gapless; }

private:
	bool DecodeNextPacket()
	{
		const AjmOpusPacket& packet         = m_packets[m_packets_decoded];
		uint32_t             skipped_frames = 0;
		AjmGaplessDecodeState gapless       = m_gapless;
		const uint64_t       allowed_frames = TrimAjmGaplessUnit(&gapless, packet.samples_per_channel, &skipped_frames);
		if (m_instance != nullptr && !DecodePacketPcm(packet, skipped_frames, allowed_frames))
		{
			m_failed = true;
			return false;
		}
		m_gapless        = gapless;
		m_pending_frames = allowed_frames;
		m_input_consumed = static_cast<uint64_t>(packet.record_offset) + sizeof(uint16_t) + packet.payload_size;
		++m_packets_decoded;
		return true;
	}

	bool DecodePacketPcm(const AjmOpusPacket& packet, uint32_t skipped_frames, uint64_t allowed_frames)
	{
		const uint32_t channels = m_instance->channels;
		if (m_input == nullptr || packet.payload_offset > m_input->size() || packet.payload_size > m_input->size() - packet.payload_offset)
		{
			return false;
		}
		std::vector<float> decoded;
		if (!m_instance->decoder->Decode(m_input->data() + packet.payload_offset, packet.payload_size, &decoded) ||
		    decoded.size() != static_cast<size_t>(packet.samples_per_channel) * channels ||
		    !std::all_of(decoded.begin(), decoded.end(), [](float sample) { return std::isfinite(sample); }))
		{
			return false;
		}
		const size_t first_sample = static_cast<size_t>(skipped_frames) * channels;
		decoded.resize(first_sample + static_cast<size_t>(allowed_frames) * channels);
		m_instance->pending_pcm           = allowed_frames == 0 ? std::vector<float> {} : std::move(decoded);
		m_instance->pending_sample_offset = allowed_frames == 0 ? 0 : first_sample;
		return true;
	}

	const std::vector<AjmOpusPacket>& m_packets;
	size_t                            m_packet_limit    = 0;
	size_t                            m_packets_decoded = 0;
	AjmGaplessDecodeState             m_gapless;
	uint64_t                          m_pending_frames  = 0;
	uint64_t                          m_input_consumed  = 0;
	uint64_t                          m_admitted_frames = 0;
	AjmInstanceState*                 m_instance        = nullptr;
	const std::vector<uint8_t>*       m_input           = nullptr;
	bool                              m_failed          = false;
};

void DropAjmResampleConsumedFrames(AjmResampleState* state, uint32_t channels)
{
	while (state->source_position >= 1.0 && state->held_frames != 0)
	{
		std::copy_n(state->held_samples.begin() + channels, channels, state->held_samples.begin());
		--state->held_frames;
		state->source_position -= 1.0;
	}
}

// Kyty's host filter: linear interpolation between the held frames. Past the last frame of a finished stream the value of
// that frame is held.
void AppendAjmResampledFrame(const AjmResampleState& state, uint32_t channels, std::vector<float>* output)
{
	const double fraction = state.source_position;
	for (uint32_t channel = 0; channel < channels; ++channel)
	{
		const double a = state.held_samples[channel];
		const double b = state.held_frames > 1 ? state.held_samples[channels + channel] : a;
		output->push_back(static_cast<float>(fraction == 0.0 ? a : a + fraction * (b - a)));
	}
}

// Produces up to output_frame_capacity frames, advancing the source position once per output frame by the ratio before
// that frame's change, so the output does not depend on how the guest splits its calls. The next source frame is pulled
// only between frames: at ratio 1 every output frame is exactly one source frame. Planning passes no output and counts
// frames only. A moving ratio that turns negative is not observed and is refused. The source supplies the codec's frames
// through Pull and reports through Exhausted that a finite stream has no more.
template <typename Source>
bool RunAjmResampler(AjmResampleState* state, uint32_t channels, uint64_t output_frame_capacity, Source* source,
                     std::vector<float>* output, uint64_t* frames_produced)
{
	uint64_t frames = 0;
	for (;;)
	{
		DropAjmResampleConsumedFrames(state, channels);
		if (frames >= output_frame_capacity)
		{
			break;
		}
		while (state->source_position >= 1.0 && source->Pull(nullptr))
		{
			state->source_position -= 1.0;
		}
		if (state->source_position >= 1.0)
		{
			break;
		}
		const uint32_t needed_frames = state->source_position > 0.0 ? 2u : 1u;
		while (state->held_frames < needed_frames && source->Pull(state->held_samples.data() + state->held_frames * channels))
		{
			++state->held_frames;
		}
		if (state->held_frames == 0 || (state->held_frames < needed_frames && !source->Exhausted()))
		{
			break;
		}
		const double step = AjmResampleRatio(*state);
		if (!(step >= 0.0) || !std::isfinite(step))
		{
			return false;
		}
		if (output != nullptr)
		{
			AppendAjmResampledFrame(*state, channels, output);
		}
		++frames;
		++state->profile_frames;
		state->source_position += step;
	}
	*frames_produced = frames;
	return true;
}

enum class AjmOpusPlanResult : uint8_t
{
	Planned,
	Inconsistent,
	UnsupportedRatio,
};

AjmOpusPlanResult PlanAjmOpusSplitDecode(const std::vector<AjmOpusPacket>& packets, const AjmGaplessDecodeState& gapless,
                                         const AjmResampleState& resample, uint32_t channels, uint32_t pending_frames,
                                         uint64_t output_frame_capacity, AjmOpusDecodePlan* plan)
{
	if (plan == nullptr || channels == 0 || channels > AJM_OPUS_SPLIT_MAX_CHANNELS || pending_frames > AJM_MAX_OPUS_PENDING_FRAMES ||
	    (gapless.configured && gapless.output_frames > gapless.total_output_frames) ||
	    pending_frames > AjmGaplessRemainingFrames(gapless) || resample.held_frames > AJM_RESAMPLE_HELD_FRAMES)
	{
		return AjmOpusPlanResult::Inconsistent;
	}

	*plan                = {};
	plan->resample_after = resample;
	AjmOpusSourceFrames source(packets, packets.size(), gapless, pending_frames, nullptr, nullptr);
	uint64_t            frames = 0;
	if (!RunAjmResampler(&plan->resample_after, channels, output_frame_capacity, &source, nullptr, &frames))
	{
		return AjmOpusPlanResult::UnsupportedRatio;
	}
	if (frames > UINT32_MAX || source.PendingFrames() > AJM_MAX_OPUS_PENDING_FRAMES)
	{
		return AjmOpusPlanResult::Inconsistent;
	}
	plan->packet_count           = source.PacketsDecoded();
	plan->input_bytes_consumed   = source.InputConsumed();
	plan->output_frames_produced = static_cast<uint32_t>(frames);
	plan->pending_frames_after   = static_cast<uint32_t>(source.PendingFrames());
	plan->admitted_frames        = source.AdmittedFrames();
	plan->gapless_after          = source.Gapless();
	return AjmOpusPlanResult::Planned;
}

bool AjmAppendPcm16Samples(const std::vector<float>& samples, size_t first_sample, size_t sample_count, std::vector<uint8_t>* output)
{
	if (output == nullptr || first_sample > samples.size() || sample_count > samples.size() - first_sample ||
	    sample_count > (SIZE_MAX - output->size()) / sizeof(int16_t))
	{
		return false;
	}
	for (size_t index = first_sample; index < first_sample + sample_count; ++index)
	{
		const float sample = samples[index];
		if (!std::isfinite(sample))
		{
			return false;
		}
		const double bounded = std::clamp(static_cast<double>(sample), -1.0, 1.0);
		const auto   rounded = std::llround(bounded * 32768.0);
		const auto   value   = static_cast<int16_t>(std::clamp<long long>(rounded, INT16_MIN, INT16_MAX));
		const auto   bits    = static_cast<uint16_t>(value);
		output->push_back(static_cast<uint8_t>(bits));
		output->push_back(static_cast<uint8_t>(bits >> 8u));
	}
	return true;
}

// Supplies the source frames of one ATRAC9 job to the resampler: first the pending PCM, then the job's superframes in order.
// A superframe is decoded whole when its first frame is needed; only its frames after priming and within the output limit
// are retained.
class AjmAtrac9SourceFrames
{
public:
	AjmAtrac9SourceFrames(AjmInstanceState* instance, const std::vector<uint8_t>& input): m_instance(instance), m_input(input) {}

	// Copies the next source frame to frame when it is not null and charges it to the gapless output limit.
	bool Pull(float* frame)
	{
		while (m_instance->pending_sample_offset == m_instance->pending_pcm.size())
		{
			if (m_failed || AjmGaplessRemainingFrames(m_instance->gapless) == 0 || !DecodeNextSuperframe())
			{
				return false;
			}
		}
		const uint32_t channels = m_instance->channels;
		if (frame != nullptr)
		{
			std::copy_n(m_instance->pending_pcm.begin() + static_cast<ptrdiff_t>(m_instance->pending_sample_offset), channels, frame);
		}
		m_instance->pending_sample_offset += channels;
		if (m_instance->pending_sample_offset == m_instance->pending_pcm.size())
		{
			m_instance->pending_pcm.clear();
			m_instance->pending_sample_offset = 0;
		}
		++m_admitted_frames;
		AddAjmGaplessOutputFrames(&m_instance->gapless, 1);
		return true;
	}

	// A finite stream whose output limit is reached holds no further source frames.
	[[nodiscard]] bool Exhausted() const
	{
		return m_instance->gapless.configured && AjmGaplessRemainingFrames(m_instance->gapless) == 0;
	}
	[[nodiscard]] bool     Failed() const { return m_failed; }
	[[nodiscard]] uint64_t InputConsumed() const { return m_input_consumed; }
	[[nodiscard]] uint64_t AdmittedFrames() const { return m_admitted_frames; }

private:
	bool DecodeNextSuperframe()
	{
		m_instance->pending_pcm.clear();
		m_instance->pending_sample_offset = 0;
		const uint32_t block_align        = m_instance->block_align;
		const uint32_t channels           = m_instance->channels;
		if (m_input.size() - m_input_consumed < block_align)
		{
			return false;
		}
		std::array<uint8_t, AjmDecoder::kAtrac9MaxSuperframeBytes> compressed {};
		std::memcpy(compressed.data(), m_input.data() + m_input_consumed, block_align);
		std::vector<float> decoded;
		if (!m_instance->decoder->Decode(compressed.data(), block_align, &decoded) ||
		    decoded.size() > static_cast<size_t>(AjmDecoder::kAtrac9MaxSuperframeSamples) * channels || decoded.size() % channels != 0 ||
		    !std::all_of(decoded.begin(), decoded.end(), [](float sample) { return std::isfinite(sample); }))
		{
			m_failed = true;
			return false;
		}
		m_input_consumed += block_align;
		uint32_t       skipped_frames  = 0;
		const uint64_t eligible_frames = TrimAjmGaplessUnit(&m_instance->gapless, static_cast<uint32_t>(decoded.size() / channels),
		                                                    &skipped_frames);
		decoded.resize((static_cast<size_t>(skipped_frames) + static_cast<size_t>(eligible_frames)) * channels);
		m_instance->pending_sample_offset = eligible_frames == 0 ? 0 : static_cast<size_t>(skipped_frames) * channels;
		m_instance->pending_pcm           = eligible_frames == 0 ? std::vector<float> {} : std::move(decoded);
		return true;
	}

	AjmInstanceState*           m_instance        = nullptr;
	const std::vector<uint8_t>& m_input;
	uint64_t                    m_input_consumed  = 0;
	uint64_t                    m_admitted_frames = 0;
	bool                        m_failed          = false;
};

// The whole superframes a job consumes pass through the instance's resampler, which at the neutral profile copies every
// source frame. The decode result's cumulative counter reports the source frames admitted to the resampler.
bool ProcessAjmAtrac9Decode(AjmInstanceState* instance, const AjmBatch2DataJob& job, const std::vector<uint8_t>& input_data,
                            std::vector<uint8_t>* output, AjmDecodeResultPrefix* result)
{
	if (instance == nullptr || instance->codec != 1 || instance->decoder == nullptr || output == nullptr || result == nullptr ||
	    instance->channels == 0 || instance->channels > AjmDecoder::kAtrac9MaxChannels || instance->block_align == 0 ||
	    instance->block_align > AjmDecoder::kAtrac9MaxSuperframeBytes || input_data.size() != job.input_size ||
	    instance->pending_sample_offset > instance->pending_pcm.size() || instance->pending_pcm.size() % instance->channels != 0 ||
	    instance->pending_sample_offset % instance->channels != 0 || instance->resample.held_frames > AJM_RESAMPLE_HELD_FRAMES)
	{
		return false;
	}
	const uint32_t channels               = instance->channels;
	const size_t   bytes_per_sample_frame = static_cast<size_t>(channels) * sizeof(int16_t);
	if (job.output_size % bytes_per_sample_frame != 0 ||
	    (instance->pending_pcm.size() - instance->pending_sample_offset) / channels > AjmGaplessRemainingFrames(instance->gapless))
	{
		return false;
	}

	AjmAtrac9SourceFrames source(instance, input_data);
	AjmResampleState      resample = instance->resample;
	std::vector<float>    samples;
	uint64_t              frames = 0;
	if (!RunAjmResampler(&resample, channels, job.output_size / bytes_per_sample_frame, &source, &samples, &frames) || source.Failed() ||
	    samples.size() != frames * channels || !AjmAppendPcm16Samples(samples, 0, samples.size(), output) ||
	    source.InputConsumed() > INT32_MAX || output->size() > INT32_MAX ||
	    source.AdmittedFrames() > UINT64_MAX - instance->delivered_samples)
	{
		return false;
	}
	instance->resample = resample;
	instance->delivered_samples += source.AdmittedFrames();
	result->input_bytes_consumed  = static_cast<int32_t>(source.InputConsumed());
	result->output_bytes_produced = static_cast<int32_t>(output->size());
	result->delivered_samples     = instance->delivered_samples;
	return true;
}

bool AjmGaplessStatesMatch(const AjmGaplessDecodeState& a, const AjmGaplessDecodeState& b)
{
	return a.configured == b.configured && a.has_decoded == b.has_decoded && a.total_output_frames == b.total_output_frames &&
	       a.priming_frames_remaining == b.priming_frames_remaining && a.output_frames == b.output_frames;
}

// The decode result's cumulative counter reports source frames admitted to the resampler; GetResampleInfo reports the held
// ones. The guest's consumer treats their difference as the source frames behind the output it receives.
bool ProcessAjmOpusSplitDecode(AjmInstanceState* instance, const AjmPreparedBatchJob& prepared, std::vector<uint8_t>* output,
                               AjmDecodeResultPrefix* result)
{
	if (instance == nullptr || instance->codec != 24 || instance->flags != 0x1002u || instance->decoder == nullptr || output == nullptr ||
	    result == nullptr || instance->channels == 0 || instance->channels > AJM_OPUS_SPLIT_MAX_CHANNELS || instance->sample_rate != 48000 ||
	    prepared.queued == nullptr || prepared.input_data.size() != prepared.queued->input_size ||
	    prepared.opus_plan.packet_count > prepared.opus_packets.size())
	{
		return false;
	}

	const uint32_t channels              = instance->channels;
	const uint64_t bytes_per_frame       = static_cast<uint64_t>(channels) * sizeof(int16_t);
	const uint64_t output_frame_capacity = prepared.queued->output_size / bytes_per_frame;
	if (instance->pending_sample_offset > instance->pending_pcm.size() || instance->pending_pcm.size() % channels != 0 ||
	    instance->pending_sample_offset % channels != 0 || instance->pending_pcm.size() / channels > AJM_MAX_OPUS_PENDING_FRAMES ||
	    (instance->gapless.configured && instance->gapless.output_frames > instance->gapless.total_output_frames) ||
	    instance->resample.held_frames > AJM_RESAMPLE_HELD_FRAMES)
	{
		return false;
	}
	const uint64_t pending_frames = (instance->pending_pcm.size() - instance->pending_sample_offset) / channels;
	if (pending_frames > AjmGaplessRemainingFrames(instance->gapless))
	{
		return false;
	}

	AjmOpusSourceFrames source(prepared.opus_packets, prepared.opus_plan.packet_count, instance->gapless, pending_frames, instance,
	                           &prepared.input_data);
	AjmResampleState    resample = instance->resample;
	std::vector<float>  samples;
	uint64_t            frames   = 0;
	const AjmOpusDecodePlan& plan = prepared.opus_plan;
	if (!RunAjmResampler(&resample, channels, output_frame_capacity, &source, &samples, &frames) || source.Failed() || source.PacketsDecoded() != plan.packet_count || source.InputConsumed() != plan.input_bytes_consumed ||
	    frames != plan.output_frames_produced || source.PendingFrames() != plan.pending_frames_after ||
	    source.AdmittedFrames() != plan.admitted_frames || !AjmGaplessStatesMatch(source.Gapless(), plan.gapless_after) ||
	    resample.source_position != plan.resample_after.source_position ||
	    resample.held_frames != plan.resample_after.held_frames ||
	    resample.profile_frames != plan.resample_after.profile_frames || samples.size() != frames * channels ||
	    !AjmAppendPcm16Samples(samples, 0, samples.size(), output) || output->size() != frames * bytes_per_frame ||
	    source.InputConsumed() > INT32_MAX || output->size() > INT32_MAX ||
	    source.AdmittedFrames() > UINT64_MAX - instance->delivered_samples)
	{
		return false;
	}
	instance->delivered_samples += source.AdmittedFrames();
	instance->gapless  = source.Gapless();
	instance->resample = resample;
	result->input_bytes_consumed  = static_cast<int32_t>(source.InputConsumed());
	result->output_bytes_produced = static_cast<int32_t>(output->size());
	result->delivered_samples     = instance->delivered_samples;
	return true;
}


void AddAjmStagedWrite(std::vector<AjmStagedGuestWrite>* writes, size_t plan_index, const void* data, size_t size)
{
	if (writes == nullptr || plan_index == SIZE_MAX || data == nullptr || size == 0)
	{
		return;
	}
	AjmStagedGuestWrite write {};
	write.plan_index  = plan_index;
	const auto* bytes = static_cast<const uint8_t*>(data);
	write.bytes.assign(bytes, bytes + size);
	writes->push_back(std::move(write));
}

bool AddAjmPreflightOutputRange(std::vector<AjmGuestWritePlan>* plans, uint64_t address, uint64_t size, size_t* plan_index)
{
	if (size == 0)
	{
		if (plan_index != nullptr)
		{
			*plan_index = SIZE_MAX;
		}
		return true;
	}
	return AddAjmGuestWritePlan(plans, address, size, plan_index);
}

// A direct decode job snapshots its one input range and plans its one output range.
bool PrepareAjmContiguousDecode(const AjmBatch2DataJob& job, std::vector<AjmGuestWritePlan>* write_plans, AjmPreparedBatchJob* prepared)
{
	if (!AjmGuestRangeIsReadable(job.input_address, job.input_size))
	{
		return false;
	}
	prepared->input_data.resize(static_cast<size_t>(job.input_size));
	if (job.input_size != 0 && !Core::VirtualMemory::CopyFromGuest(prepared->input_data.data(), job.input_address, prepared->input_data.size()))
	{
		return false;
	}
	return AddAjmPreflightOutputRange(write_plans, job.output_address, job.output_size, &prepared->output_plan);
}

// A split job reads its input descriptors in order as one stream.
bool GatherAjmSplitInput(const AjmBatch2DataJob& job, std::vector<uint8_t>* input)
{
	input->clear();
	input->reserve(static_cast<size_t>(job.input_size));
	for (const auto& input_buffer: job.input_buffers)
	{
		if (input_buffer.size > job.input_size - input->size() ||
		    !AjmGuestRangeIsReadable(reinterpret_cast<uint64_t>(input_buffer.address), input_buffer.size))
		{
			return false;
		}
		const size_t offset = input->size();
		input->resize(offset + input_buffer.size);
		if (input_buffer.size != 0 &&
		    !Core::VirtualMemory::CopyFromGuest(input->data() + offset, reinterpret_cast<uint64_t>(input_buffer.address), input_buffer.size))
		{
			return false;
		}
	}
	return input->size() == job.input_size;
}

// A split job writes its PCM over its output descriptors in order; each descriptor gets one write plan.
bool PlanAjmSplitOutputs(const AjmBatch2DataJob& job, std::vector<AjmGuestWritePlan>* write_plans, std::vector<size_t>* output_plans)
{
	uint64_t output_size = 0;
	output_plans->reserve(job.output_buffers.size());
	for (const auto& output_buffer: job.output_buffers)
	{
		size_t output_plan = SIZE_MAX;
		if (output_buffer.size > job.output_size - output_size ||
		    !AddAjmPreflightOutputRange(write_plans, reinterpret_cast<uint64_t>(output_buffer.address), output_buffer.size, &output_plan))
		{
			return false;
		}
		output_plans->push_back(output_plan);
		output_size += output_buffer.size;
	}
	return output_size == job.output_size;
}

// Stages decoded PCM over the planned output descriptors in order. False if the PCM does not fit them.
bool StageAjmSplitOutputs(const AjmBatch2DataJob& job, const AjmPreparedBatchJob& prepared, const std::vector<AjmGuestWritePlan>& write_plans,
                          const std::vector<uint8_t>& pcm, std::vector<AjmStagedGuestWrite>* staged_writes)
{
	size_t pcm_offset = 0;
	for (size_t output_index = 0; output_index < job.output_buffers.size() && pcm_offset < pcm.size(); ++output_index)
	{
		const size_t chunk_size = std::min(job.output_buffers[output_index].size, pcm.size() - pcm_offset);
		const size_t plan_index = prepared.output_plans[output_index];
		if (chunk_size == 0)
		{
			continue;
		}
		if (plan_index == SIZE_MAX || chunk_size > write_plans[plan_index].size)
		{
			return false;
		}
		AddAjmStagedWrite(staged_writes, plan_index, pcm.data() + pcm_offset, chunk_size);
		pcm_offset += chunk_size;
	}
	return pcm_offset == pcm.size();
}

} // namespace

int KYTY_SYSV_ABI AjmInitialize(int64_t reserved, uint32_t* context)
{
	PRINT_NAME();

	if (context == nullptr || (reserved != 0 && reserved != AJM_GEN5_INITIALIZATION_FLAGS))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	*context = AllocateContextId();
	g_ajm_contexts.emplace(*context, AjmContextState {});

	return OK;
}

int KYTY_SYSV_ABI AjmFinalize(uint32_t context)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t context = %u\n", context);

	std::scoped_lock lock(g_ajm_mutex);
	auto context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	const auto& state = context_it->second;
	for (const auto& [instance, instance_state]: state.instances)
	{
		(void)instance;
		if (instance_state.decoder != nullptr && g_ajm_live_decoder_count != 0)
		{
			--g_ajm_live_decoder_count;
		}
	}
	for (auto batch_it = g_ajm_batch2_jobs.begin(); batch_it != g_ajm_batch2_jobs.end();)
	{
		auto& jobs = batch_it->second.jobs;
		jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&state](const auto& job) {
			           const uint32_t slot = job.instance & AJM_INSTANCE_SLOT_MASK;
			           auto           it   = state.instances_by_slot.find(slot);
			           return it != state.instances_by_slot.end() && it->second == job.instance;
		       }),
		           jobs.end());
		if (jobs.empty())
		{
			batch_it = g_ajm_batch2_jobs.erase(batch_it);
		} else
		{
			++batch_it;
		}
	}
	g_ajm_contexts.erase(context_it);
	return OK;
}

int KYTY_SYSV_ABI AjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved)
{
	PRINT_NAME();

	if (codec >= AJM_MAX_CODEC_TYPE || reserved != 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (!context_it->second.registered_codecs.insert(codec).second)
	{
		return AJM_ERROR_CODEC_ALREADY_REGISTERED;
	}

	KYTY_LOG_DEBUG("\t codec = %u\n", codec);

	switch (codec)
	{
		case 1: KYTY_LOG_DEBUG("\t %s\n", "ATRAC9 decoder"); break;
		case 2: KYTY_LOG_DEBUG("\t %s\n", "MPEG4-AAC decoder"); break;
		case 0: KYTY_LOG_DEBUG("\t %s\n", "MP3 decoder"); break;
		case 4: KYTY_LOG_DEBUG("\t %s\n", "CELP8 encoder"); break;
		case 3: KYTY_LOG_DEBUG("\t %s\n", "CELP8 decoder"); break;
		case 13: KYTY_LOG_DEBUG("\t %s\n", "CELP16 encoder"); break;
		case 12: KYTY_LOG_DEBUG("\t %s\n", "CELP16 decoder"); break;
		default: KYTY_LOG_DEBUG("\t codec %u\n", codec); break;
	}

	return OK;
}

int KYTY_SYSV_ABI AjmModuleUnregister(uint32_t context, uint32_t codec)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t context = %u\n", context);
	KYTY_LOG_DEBUG("\t codec   = %u\n", codec);

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.registered_codecs.erase(codec) == 0)
	{
		return AJM_ERROR_CODEC_NOT_REGISTERED;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmMemoryRegister(uint32_t context, const void* address, size_t num_pages)
{
	PRINT_NAME();

	if (address == nullptr || num_pages == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	if (g_ajm_contexts.find(context) == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}

	// AJM hardware requires explicit shared-memory registration. HLE decoders
	// already share the guest address space, so the validated range needs no
	// additional host mapping.
	return OK;
}

int KYTY_SYSV_ABI AjmBatchInitializeBuffer(void* buffer, size_t buffer_size, void* control)
{
	PRINT_NAME();

	if (buffer == nullptr || buffer_size == 0 || control == nullptr)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	// Gen5 Wwise supplies its own batch storage and control object. Initializing the control starts an empty batch: the
	// guest initializes before building every batch and may abandon one it never started, so jobs queued earlier on the
	// same control are discarded.
	std::scoped_lock lock(g_ajm_mutex);
	g_ajm_batch2_jobs.erase(reinterpret_cast<uintptr_t>(control));
	return OK;
}

int KYTY_SYSV_ABI AjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance)
{
	PRINT_NAME();

	if (codec >= AJM_MAX_CODEC_TYPE || instance == nullptr)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if ((flags & 0x7u) == 0)
	{
		return AJM_ERROR_WRONG_REVISION_FLAG;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	auto& state = context_it->second;
	if (state.registered_codecs.find(codec) == state.registered_codecs.end())
	{
		return AJM_ERROR_CODEC_NOT_REGISTERED;
	}
	uint32_t slot = state.next_instance_slot;
	uint32_t instance_id = 0;
	for (uint32_t attempts = 0; attempts < AJM_MAX_INSTANCE_SLOT; ++attempts)
	{
		slot = slot % AJM_MAX_INSTANCE_SLOT + 1;
		const uint32_t candidate = (codec << 14u) | slot;
		if (state.instances_by_slot.find(slot) == state.instances_by_slot.end() && !IsAjmInstanceIdInUse(candidate))
		{
			instance_id = candidate;
			break;
		}
	}
	if (instance_id == 0)
	{
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	state.instances_by_slot.emplace(slot, instance_id);
	AjmInstanceState instance_state {};
	instance_state.codec = codec;
	instance_state.flags = flags;
	state.instances.emplace(instance_id, std::move(instance_state));
	state.next_instance_slot = slot;
	*instance                = instance_id;

	KYTY_LOG_DEBUG("\t context  = %u\n", context);
	KYTY_LOG_DEBUG("\t codec    = %u\n", codec);
	KYTY_LOG_DEBUG("\t flags    = 0x%016" PRIx64 "\n", flags);
	KYTY_LOG_DEBUG("\t instance = 0x%08" PRIx32 "\n", instance_id);

	return OK;
}

int KYTY_SYSV_ABI AjmInstanceDestroy(uint32_t context, uint32_t instance)
{
	PRINT_NAME();

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}

	auto* instance_state = FindAjmInstanceState(context_it->second, instance);
	const uint32_t slot  = instance & AJM_INSTANCE_SLOT_MASK;
	if (instance_state == nullptr)
	{
		return AJM_ERROR_INVALID_INSTANCE;
	}
	if (instance_state->decoder != nullptr && g_ajm_live_decoder_count != 0)
	{
		--g_ajm_live_decoder_count;
	}
	context_it->second.instances.erase(instance);
	context_it->second.instances_by_slot.erase(slot);
	RemoveQueuedAjmJobs(instance);

	KYTY_LOG_DEBUG("\t context  = %u\n", context);
	KYTY_LOG_DEBUG("\t instance = 0x%08" PRIx32 "\n", instance);
	return OK;
}

void* KYTY_SYSV_ABI AjmBatchJobControlBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* sideband_input,
                                               size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                                               void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || sideband_input_size > UINT32_MAX || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	current = WriteAjmBufferChunk(current, 2u, sideband_input, sideband_input_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	constexpr uint64_t control_flags_mask = 0x000060000000e7ffull;
	flags &= control_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(3u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

void* KYTY_SYSV_ABI AjmBatchJobInlineBuffer(void* buffer, const void* data_input, size_t data_input_size, const void** batch_address)
{
	PRINT_NAME();
	if (buffer == nullptr || batch_address == nullptr || (data_input == nullptr && data_input_size != 0) ||
	    data_input_size > UINT32_MAX - 7u)
	{
		return nullptr;
	}

	auto* const  begin        = static_cast<uint8_t*>(buffer);
	const size_t aligned_size = (data_input_size + 7u) & ~size_t {7u};
	WriteAjmU32(begin, MakeAjmChunkHeader(7u));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(aligned_size));
	*batch_address = begin + 8u;
	if (data_input_size != 0)
	{
		std::memcpy(begin + 8u, data_input, data_input_size);
	}
	if (aligned_size > data_input_size)
	{
		std::memset(begin + 8u + data_input_size, 0, aligned_size - data_input_size);
	}
	return begin + 8u + aligned_size;
}

void* KYTY_SYSV_ABI AjmBatchJobRunBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* data_input, size_t data_input_size,
                                           void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size,
                                           void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || data_input_size > UINT32_MAX || data_output_size > UINT32_MAX || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	current = WriteAjmBufferChunk(current, 1u, data_input, data_input_size);

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	flags &= run_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(4u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	current = WriteAjmBufferChunk(current, 17u, data_output, data_output_size);
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

void* KYTY_SYSV_ABI AjmBatchJobRunSplitBufferRa(void* buffer, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                                size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                                size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size,
                                                void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || (data_input_buffers == nullptr && data_input_buffer_count != 0) ||
	    (data_output_buffers == nullptr && data_output_buffer_count != 0) || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	for (size_t index = 0; index < data_input_buffer_count; ++index)
	{
		current = WriteAjmBufferChunk(current, 1u, data_input_buffers[index].address, data_input_buffers[index].size);
		if (current == nullptr)
		{
			return nullptr;
		}
	}

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	flags &= run_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(4u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	for (size_t index = 0; index < data_output_buffer_count; ++index)
	{
		current = WriteAjmBufferChunk(current, 17u, data_output_buffers[index].address, data_output_buffers[index].size);
		if (current == nullptr)
		{
			return nullptr;
		}
	}
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

int KYTY_SYSV_ABI AjmBatchStartBuffer(uint32_t context, uint8_t* batch_buffer, uint32_t batch_size, int priority, AjmBatchError* error,
                                      uint32_t* batch_id)
{
	PRINT_NAME();
	const auto clear_error = [error]
	{
		if (error != nullptr)
		{
			std::memset(error, 0, sizeof(*error));
		}
	};
	if (batch_buffer == nullptr || batch_id == nullptr || (batch_size & 7u) != 0 || priority < 0)
	{
		clear_error();
		return (batch_size & 7u) != 0 ? AJM_ERROR_MALFORMED_BATCH : AJM_ERROR_INVALID_PARAMETER;
	}

	std::unique_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		clear_error();
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.completed_batches.size() >= AJM_MAX_COMPLETED_BATCHES)
	{
		clear_error();
		return AJM_ERROR_OUT_OF_RESOURCES;
	}
	std::vector<AjmParsedBufferJob> parsed_jobs;
	const auto                      parse_status = ParseAjmBatchBuffer(batch_buffer, batch_size, context_it->second, &parsed_jobs);
	if (parse_status == AjmBufferBatchStatus::Malformed)
	{
		clear_error();
		return AJM_ERROR_MALFORMED_BATCH;
	}
	if (parse_status == AjmBufferBatchStatus::UnsupportedDataOperation)
	{
		lock.unlock();
		RejectUnsupportedAjmDataOperation(AjmDataOperation::Run);
	}

	clear_error();
	ApplyAjmBatchBuffer(parsed_jobs);
	auto& state = context_it->second;
	do
	{
		++state.next_batch_id;
		if (state.next_batch_id == 0)
		{
			++state.next_batch_id;
		}
	} while (state.completed_batches.find(state.next_batch_id) != state.completed_batches.end());
	state.completed_batches.insert(state.next_batch_id);
	*batch_id = state.next_batch_id;
	return OK;
}

int KYTY_SYSV_ABI AjmBatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, AjmBatchError* error)
{
	PRINT_NAME();
	(void)timeout;
	if (error != nullptr)
	{
		std::memset(error, 0, sizeof(*error));
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.completed_batches.erase(batch_id) == 0)
	{
		return AJM_ERROR_INVALID_BATCH;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchCancel(uint32_t context, uint32_t batch_id)
{
	PRINT_NAME();
	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.completed_batches.erase(batch_id) == 0)
	{
		return AJM_ERROR_INVALID_BATCH;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobInitialize(void* batch, uint32_t instance, const void* config, size_t config_size, void* result)
{
	PRINT_NAME();
	const uint32_t codec = instance >> 14u;
	const size_t   expected_config_size = codec == 1u ? 8u : (codec == 24u ? 12u : 0u);
	if (batch == nullptr || instance == 0 || expected_config_size == 0 || config == nullptr || config_size != expected_config_size)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::Initialize;
	job.instance       = instance;
	job.result_address = reinterpret_cast<uint64_t>(result);
	if (!Core::VirtualMemory::CopyFromGuest(job.config.data(), reinterpret_cast<uint64_t>(config), config_size))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobClearContext(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::Clear;
	job.instance       = instance;
	job.result_address = reinterpret_cast<uint64_t>(result);
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobSetGaplessDecode(void* batch, uint32_t instance, const void* config, uint64_t reset, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || config == nullptr || reset > 1)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::SetGaplessDecode;
	job.instance       = instance;
	job.gapless_reset  = reset;
	job.result_address = reinterpret_cast<uint64_t>(result);
	if (!Core::VirtualMemory::CopyFromGuest(job.gapless_config.data(), reinterpret_cast<uint64_t>(config), job.gapless_config.size()))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobGetGaplessDecode(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	return QueueUnsupportedAjmControl(batch, instance, AjmDataOperation::GetGaplessDecode, nullptr, result);
}

int KYTY_SYSV_ABI AjmBatchJobSetResampleParameters(void* batch, uint32_t instance, float ratio, uint32_t flags, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || !std::isfinite(ratio))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	KYTY_LOG_DEBUG("\t instance = 0x%08" PRIx32 "\n", instance);
	KYTY_LOG_DEBUG("\t ratio    = %f\n", static_cast<double>(ratio));
	KYTY_LOG_DEBUG("\t flags    = 0x%08" PRIx32 "\n", flags);
	return QueueUnsupportedAjmControl(batch, instance, AjmDataOperation::SetResampleParameters, nullptr, result);
}

int KYTY_SYSV_ABI AjmBatchJobSetResampleParametersEx(void* batch, uint32_t instance, float ratio_start, float ratio_change_per_sample,
                                                     uint32_t flags, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || !std::isfinite(ratio_start) || !std::isfinite(ratio_change_per_sample))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	KYTY_LOG_DEBUG("\t instance                = 0x%08" PRIx32 "\n", instance);
	KYTY_LOG_DEBUG("\t ratio_start             = %f\n", static_cast<double>(ratio_start));
	KYTY_LOG_DEBUG("\t ratio_change_per_sample = %f\n", static_cast<double>(ratio_change_per_sample));
	KYTY_LOG_DEBUG("\t flags                   = 0x%08" PRIx32 "\n", flags);
	AjmBatch2DataJob job {};
	job.operation               = AjmDataOperation::SetResampleParametersEx;
	job.instance                = instance;
	job.ratio_start             = ratio_start;
	job.ratio_change_per_sample = ratio_change_per_sample;
	job.resample_flags          = flags;
	job.result_address          = reinterpret_cast<uint64_t>(result);
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobGetResampleInfo(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::GetResampleInfo;
	job.instance       = instance;
	job.result_address = reinterpret_cast<uint64_t>(result);
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobDecode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                    size_t data_output_size, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_DIRECT_JOB_INPUT_SIZE || data_output_size > AJM_MAX_DIRECT_JOB_INPUT_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::Decode;
	job.instance       = instance;
	job.input_address  = reinterpret_cast<uint64_t>(data_input);
	job.input_size     = data_input_size;
	job.output_address = reinterpret_cast<uint64_t>(data_output);
	job.output_size    = data_output_size;
	job.result_address = reinterpret_cast<uint64_t>(result);
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobDecodeSingle(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                          size_t data_output_size, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	AjmBatch2DataJob job {};
	job.operation = AjmDataOperation::DecodeSingle;
	job.instance  = instance;
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobDecodeSplit(void* batch, uint32_t instance, const AjmBuffer* data_input_buffers,
                                         size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                         size_t data_output_buffer_count, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	AjmBatch2DataJob job {};
	job.operation      = AjmDataOperation::DecodeSplit;
	job.instance       = instance;
	job.result_address = reinterpret_cast<uint64_t>(result);
	if (!SnapshotAjmBuffers(data_input_buffers, data_input_buffer_count, AJM_MAX_DIRECT_JOB_INPUT_SIZE, &job.input_buffers,
	                        &job.input_size) ||
	    !SnapshotAjmBuffers(data_output_buffers, data_output_buffer_count, AJM_MAX_BATCH2_BUFFER_SIZE, &job.output_buffers,
	                        &job.output_size) ||
	    job.output_size == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobEncode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                    size_t data_output_size, void* result)
{
	PRINT_NAME();
	(void)result;
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	AjmBatch2DataJob job {};
	job.operation = AjmDataOperation::Encode;
	job.instance  = instance;
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobGetInfo(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	return QueueUnsupportedAjmControl(batch, instance, AjmDataOperation::GetInfo, nullptr, result);
}

int KYTY_SYSV_ABI AjmBatchJobGetCodecInfo(void* batch, uint32_t instance, void* result, size_t result_size)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (result == nullptr && result_size != 0))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return QueueUnsupportedAjmControl(batch, instance, AjmDataOperation::GetCodecInfo, nullptr, result, result_size);
}

int KYTY_SYSV_ABI AjmBatchJobGetStatistics(void* batch, float interval, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || !std::isfinite(interval))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return QueueUnsupportedAjmControl(batch, 0, AjmDataOperation::GetStatistics, nullptr, result);
}

int KYTY_SYSV_ABI AjmBatchJobControl(void* batch, uint32_t instance, uint64_t flags, const void* sideband_input, size_t sideband_input_size,
                                     void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (sideband_input == nullptr && sideband_input_size != 0) || sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	(void)flags;
	return QueueUnsupportedAjmControl(batch, instance, AjmDataOperation::Control, sideband_input, sideband_output, sideband_output_size,
	                                  sideband_input_size);
}

int KYTY_SYSV_ABI AjmBatchJobRun(void* batch, uint32_t instance, uint64_t flags, const void* data_input, size_t data_input_size,
                                 void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	(void)flags;
	(void)sideband_output;
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE || sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	AjmBatch2DataJob job {};
	job.operation = AjmDataOperation::Run;
	job.instance  = instance;
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchJobRunSplit(void* batch, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                      size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                      size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	(void)flags;
	(void)sideband_output;
	uint32_t input_size  = 0;
	uint32_t output_size = 0;
	if (batch == nullptr || instance == 0 || !ValidateAjmBuffers(data_input_buffers, data_input_buffer_count, &input_size) ||
	    !ValidateAjmBuffers(data_output_buffers, data_output_buffer_count, &output_size) || output_size == 0 ||
	    sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	AjmBatch2DataJob job {};
	job.operation = AjmDataOperation::RunSplit;
	job.instance  = instance;
	return QueueAjmDataJob(std::move(job), batch) ? OK : AJM_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI AjmBatchStart(uint32_t context, void* batch, int priority, AjmBatchError* error, uint32_t* batch_id)
{
	PRINT_NAME();
	if (batch == nullptr || batch_id == nullptr || priority < 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::unique_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	auto& state = context_it->second;
	if (state.completed_batches.size() >= AJM_MAX_COMPLETED_BATCHES)
	{
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	uint32_t next_batch_id = state.next_batch_id;
	do
	{
		++next_batch_id;
		if (next_batch_id == 0)
		{
			++next_batch_id;
		}
	} while (state.completed_batches.find(next_batch_id) != state.completed_batches.end());

	auto                  jobs_it      = g_ajm_batch2_jobs.find(reinterpret_cast<uintptr_t>(batch));
	const AjmQueuedBatch* queued_batch = jobs_it == g_ajm_batch2_jobs.end() ? nullptr : &jobs_it->second;
	if (queued_batch != nullptr)
	{
		for (const auto& job: queued_batch->jobs)
		{
			if (job.operation != AjmDataOperation::Initialize && job.operation != AjmDataOperation::Clear &&
			    job.operation != AjmDataOperation::Decode && job.operation != AjmDataOperation::DecodeSplit &&
			    job.operation != AjmDataOperation::SetGaplessDecode && job.operation != AjmDataOperation::SetResampleParametersEx &&
			    job.operation != AjmDataOperation::GetResampleInfo)
			{
				const auto operation = job.operation;
				lock.unlock();
				RejectUnsupportedAjmDataOperation(operation);
			}
		}
	}

	std::vector<AjmPreparedBatchJob> prepared_jobs;
	std::vector<AjmGuestWritePlan>   write_plans;
	prepared_jobs.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	write_plans.reserve((queued_batch == nullptr ? 0u : queued_batch->jobs.size() * (AJM_MAX_DIRECT_BUFFER_DESCRIPTORS + 2u)) + 2u);
	std::unordered_map<uint32_t, bool>                  active_decoders;
	std::unordered_map<uint32_t, AjmDecoderFormat>      decoder_formats;
	std::unordered_map<uint32_t, AjmGaplessDecodeState> gapless_states;
	std::unordered_map<uint32_t, uint32_t>              opus_pending_frames;
	std::unordered_map<uint32_t, AjmResampleState>      resample_states;
	active_decoders.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	decoder_formats.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	gapless_states.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	opus_pending_frames.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	resample_states.reserve(queued_batch == nullptr ? 0u : queued_batch->jobs.size());
	size_t active_decoder_count = g_ajm_live_decoder_count;
	size_t staged_decoder_count = 0;
	size_t superframes_admitted = 0;

	if (queued_batch != nullptr)
	{
		for (const auto& job: queued_batch->jobs)
		{
			auto* instance_state = FindAjmInstanceState(state, job.instance);
			if (instance_state == nullptr || (job.instance >> 14u) != instance_state->codec)
			{
				return AJM_ERROR_INVALID_INSTANCE;
			}
			if (active_decoders.find(job.instance) == active_decoders.end())
			{
				const bool active = instance_state->decoder != nullptr;
				active_decoders.emplace(job.instance, active);
				gapless_states.emplace(job.instance, instance_state->gapless);
				resample_states.emplace(job.instance, instance_state->resample);
				uint32_t pending_frames = 0;
				if (active && instance_state->codec == 24)
				{
					if (instance_state->channels == 0 || instance_state->channels > 2 ||
					    instance_state->pending_sample_offset > instance_state->pending_pcm.size() ||
					    instance_state->pending_pcm.size() % instance_state->channels != 0 ||
					    instance_state->pending_sample_offset % instance_state->channels != 0 ||
					    instance_state->pending_pcm.size() / instance_state->channels > AJM_MAX_OPUS_PENDING_FRAMES)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus pending PCM state is inconsistent");
					}
					const size_t pending_samples = instance_state->pending_pcm.size() - instance_state->pending_sample_offset;
					if (pending_samples % instance_state->channels != 0 ||
					    pending_samples / instance_state->channels > AJM_MAX_OPUS_PENDING_FRAMES)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus pending PCM state is inconsistent");
					}
					pending_frames = static_cast<uint32_t>(pending_samples / instance_state->channels);
				}
				opus_pending_frames.emplace(job.instance, pending_frames);
				if (active)
				{
					decoder_formats.emplace(job.instance, AjmDecoderFormat {instance_state->codec, instance_state->channels,
					                                                        instance_state->sample_rate, instance_state->block_align});
				}
			}
		}

		for (const auto& job: queued_batch->jobs)
		{
			AjmPreparedBatchJob prepared {};
			prepared.queued      = &job;
			auto* instance_state = FindAjmInstanceState(state, job.instance);
			if (instance_state == nullptr)
			{
				return AJM_ERROR_INVALID_INSTANCE;
			}
			bool& active = active_decoders[job.instance];

			if (job.operation == AjmDataOperation::Initialize)
			{
				// Prepared decoders remain live until publication replaces or installs them.
				if (g_ajm_live_decoder_count >= AJM_MAX_LIVE_DECODERS ||
				    staged_decoder_count >= AJM_MAX_LIVE_DECODERS - g_ajm_live_decoder_count)
				{
					return AJM_ERROR_OUT_OF_RESOURCES;
				}
				std::string decoder_error;
				if (instance_state->codec == 1)
				{
					// Only the four config bytes reach the decoder. A shipped title passes 0x20 in the following word, so it
					// carries no state the decoder must refuse.
					prepared.decoder = AjmDecoder::OpenAtrac9FromConfig(job.config.data(), 4, &decoder_error);
					if (prepared.decoder == nullptr || prepared.decoder->Channels() == 0 ||
					    prepared.decoder->Channels() > AjmDecoder::kAtrac9MaxChannels || prepared.decoder->SampleRate() == 0 ||
					    prepared.decoder->SampleRate() > AjmDecoder::kAtrac9MaxSampleRate || prepared.decoder->BlockAlign() == 0 ||
					    prepared.decoder->BlockAlign() > AjmDecoder::kAtrac9MaxSuperframeBytes)
					{
						lock.unlock();
						RejectMalformedAjmData("ATRAC9 configuration or decoder is unavailable");
					}
				} else if (instance_state->codec == 24)
				{
					if (instance_state->flags != 0x1002u)
					{
						lock.unlock();
						RejectUnsupportedAjmDataOperation(AjmDataOperation::Initialize);
					}
					const uint32_t channels    = AjmReadLittleEndianU32(job.config.data());
					const uint32_t sample_rate = AjmReadLittleEndianU32(job.config.data() + 4);
					const uint32_t reserved    = AjmReadLittleEndianU32(job.config.data() + 8);
					if ((channels != 1 && channels != 2) || sample_rate != 48000 || reserved != 0)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus configuration is outside the observed 48 kHz S16 profile");
					}
					prepared.decoder = AjmDecoder::OpenOpus(channels, sample_rate, &decoder_error);
					if (prepared.decoder == nullptr || prepared.decoder->Channels() != channels ||
					    prepared.decoder->SampleRate() != sample_rate || prepared.decoder->BlockAlign() != 0)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus decoder is unavailable for the observed profile");
					}
				} else
				{
					lock.unlock();
					RejectUnsupportedAjmDataOperation(AjmDataOperation::Initialize);
				}
				++staged_decoder_count;
				if (!active)
				{
					++active_decoder_count;
				}
				if (active_decoder_count > AJM_MAX_LIVE_DECODERS)
				{
					return AJM_ERROR_OUT_OF_RESOURCES;
				}
				active                        = true;
				decoder_formats[job.instance] = {instance_state->codec, prepared.decoder->Channels(), prepared.decoder->SampleRate(),
				                                 prepared.decoder->BlockAlign()};
				gapless_states[job.instance]  = {};
				opus_pending_frames[job.instance] = 0;
				resample_states[job.instance] = {};
				if (job.result_address != 0 &&
				    !AddAjmPreflightOutputRange(&write_plans, job.result_address, AJM_SIDEBAND_RESULT_SIZE, &prepared.result_plan))
				{
					return AJM_ERROR_INVALID_PARAMETER;
				}
			} else if (job.operation == AjmDataOperation::Clear)
			{
				gapless_states[job.instance]      = {};
				opus_pending_frames[job.instance] = 0;
				ClearAjmResampleState(&resample_states[job.instance]);
				if (job.result_address != 0 &&
				    !AddAjmPreflightOutputRange(&write_plans, job.result_address, AJM_SIDEBAND_RESULT_SIZE, &prepared.result_plan))
				{
					return AJM_ERROR_INVALID_PARAMETER;
				}
			} else if (job.operation == AjmDataOperation::SetGaplessDecode)
			{
				auto format_it = decoder_formats.find(job.instance);
				if (!active || format_it == decoder_formats.end())
				{
					lock.unlock();
					RejectMalformedAjmData("gapless configuration requires an initialized decoder");
				}
				const bool opus_profile = format_it->second.codec == 24 && instance_state->flags == 0x1002u;
				if ((format_it->second.codec != 1 && !opus_profile) ||
				    !ApplyAjmGaplessConfiguration(&gapless_states[job.instance], job.gapless_config, job.gapless_reset,
				                                  format_it->second.codec == 1))
				{
					lock.unlock();
					RejectUnsupportedAjmDataOperation(AjmDataOperation::SetGaplessDecode);
				}
				if (job.result_address != 0 &&
				    !AddAjmPreflightOutputRange(&write_plans, job.result_address, AJM_SIDEBAND_RESULT_SIZE, &prepared.result_plan))
				{
					return AJM_ERROR_INVALID_PARAMETER;
				}
			} else if (job.operation == AjmDataOperation::SetResampleParametersEx)
			{
				auto format_it = decoder_formats.find(job.instance);
				if (!active || format_it == decoder_formats.end())
				{
					lock.unlock();
					RejectMalformedAjmData("resample profile requires an initialized decoder");
				}
				auto&      resample = resample_states[job.instance];
				const auto& gapless  = gapless_states[job.instance];
				if (!AjmInstanceResamples(*instance_state) || !IsAjmResampleProfileAdmitted(resample, gapless, job))
				{
					lock.unlock();
					RejectUnsupportedAjmDataOperation(AjmDataOperation::SetResampleParametersEx);
				}
				ApplyAjmResampleProfile(&resample, job);
				if (job.result_address != 0 &&
				    !AddAjmPreflightOutputRange(&write_plans, job.result_address, AJM_SIDEBAND_RESULT_SIZE, &prepared.result_plan))
				{
					return AJM_ERROR_INVALID_PARAMETER;
				}
			} else if (job.operation == AjmDataOperation::GetResampleInfo)
			{
				auto format_it = decoder_formats.find(job.instance);
				auto state_it  = resample_states.find(job.instance);
				if (!active || format_it == decoder_formats.end() || state_it == resample_states.end() ||
				    !AjmInstanceResamples(*instance_state) || !state_it->second.configured || state_it->second.flags > 3u)
				{
					lock.unlock();
					RejectUnsupportedAjmDataOperation(AjmDataOperation::GetResampleInfo);
				}
				if (job.result_address != 0 && !AddAjmPreflightOutputRange(&write_plans, job.result_address,
				                                                           AJM_DIRECT_RESAMPLE_INFO_PREFIX_SIZE, &prepared.result_plan))
				{
					return AJM_ERROR_INVALID_PARAMETER;
				}
			} else
			{
				auto format_it = decoder_formats.find(job.instance);
				if (!active || format_it == decoder_formats.end())
				{
					lock.unlock();
					RejectMalformedAjmData("decode instance has not been initialized");
				}
				const AjmDecoderFormat format = format_it->second;
				if (format.codec != 1 && job.operation == AjmDataOperation::Decode)
				{
					lock.unlock();
					RejectUnsupportedAjmDataOperation(AjmDataOperation::Decode);
				}
				if (format.codec == 1)
				{
					if (resample_states[job.instance].profile_required)
					{
						lock.unlock();
						RejectUnsupportedAjmDataOperation(job.operation);
					}
					const uint64_t bytes_per_frame = static_cast<uint64_t>(format.channels) * sizeof(int16_t);
					if (format.channels == 0 || bytes_per_frame == 0 || format.block_align == 0 ||
					    job.input_size > AJM_MAX_DIRECT_JOB_INPUT_SIZE ||
					    job.output_size > Core::VirtualMemory::GuestMappingSnapshot::kMaxBytes ||
					    job.input_size % format.block_align != 0 || job.output_size % bytes_per_frame != 0)
					{
						lock.unlock();
						RejectMalformedAjmData("ATRAC9 job extent is not a whole number of samples or superframes");
					}
					const size_t job_superframes = static_cast<size_t>(job.input_size / format.block_align);
					if (job_superframes > AJM_MAX_DIRECT_SUPERFRAMES - superframes_admitted)
					{
						return AJM_ERROR_OUT_OF_RESOURCES;
					}
					superframes_admitted += job_superframes;
					// A split job is the same decode with its input gathered and its PCM scattered.
					const bool staged = job.operation == AjmDataOperation::Decode
					                        ? PrepareAjmContiguousDecode(job, &write_plans, &prepared)
					                        : job.output_buffers.size() <= AJM_MAX_DIRECT_BUFFER_DESCRIPTORS &&
					                              GatherAjmSplitInput(job, &prepared.input_data) &&
					                              PlanAjmSplitOutputs(job, &write_plans, &prepared.output_plans);
					if (!staged || (job.result_address != 0 && !AddAjmPreflightOutputRange(&write_plans, job.result_address,
					                                                                        sizeof(AjmDecodeResultPrefix), &prepared.result_plan)))
					{
						return AJM_ERROR_INVALID_PARAMETER;
					}
					// Before the first decode no PCM is pending, so any input with output room decodes a superframe.
					if (job.input_size != 0 && job.output_size != 0)
					{
						gapless_states[job.instance].has_decoded = true;
					}
					resample_states[job.instance].reset_window = false;
				} else
				{
					if (format.codec != 24 || instance_state->flags != 0x1002u || resample_states[job.instance].profile_required)
					{
						lock.unlock();
						RejectUnsupportedAjmDataOperation(AjmDataOperation::DecodeSplit);
					}
					const uint64_t bytes_per_frame = static_cast<uint64_t>(format.channels) * sizeof(int16_t);
					if (format.channels == 0 || format.channels > 2 || format.sample_rate != 48000 || format.block_align != 0 ||
					    job.input_size > AJM_MAX_DIRECT_JOB_INPUT_SIZE ||
					    job.output_size > Core::VirtualMemory::GuestMappingSnapshot::kMaxBytes || bytes_per_frame == 0 ||
					    job.output_size == 0 || job.output_size % bytes_per_frame != 0 ||
					    job.output_buffers.size() > AJM_MAX_DIRECT_BUFFER_DESCRIPTORS)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus split job extent is not a bounded whole number of S16 frames");
					}
					if (!GatherAjmSplitInput(job, &prepared.input_data))
					{
						return AJM_ERROR_INVALID_PARAMETER;
					}

					const AjmOpusPacketScanResult scan_result = ScanAjmOpusPacketRecords(prepared.input_data, &prepared.opus_packets);
					if (scan_result == AjmOpusPacketScanResult::TooManyPackets)
					{
						return AJM_ERROR_OUT_OF_RESOURCES;
					}
					if (scan_result != AjmOpusPacketScanResult::Valid)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus input is not a complete U16LE packet stream");
					}

					const uint64_t output_frame_capacity = job.output_size / bytes_per_frame;
					auto& gapless = gapless_states[job.instance];
					auto& resample = resample_states[job.instance];
					const AjmOpusPlanResult plan_result = PlanAjmOpusSplitDecode(prepared.opus_packets, gapless, resample, format.channels,
					                                                             opus_pending_frames[job.instance], output_frame_capacity,
					                                                             &prepared.opus_plan);
					if (plan_result == AjmOpusPlanResult::UnsupportedRatio)
					{
						lock.unlock();
						RejectUnsupportedAjmDataOperation(AjmDataOperation::DecodeSplit);
					}
					if (plan_result != AjmOpusPlanResult::Planned)
					{
						lock.unlock();
						RejectMalformedAjmData("Opus pending or gapless state is inconsistent");
					}
					gapless = prepared.opus_plan.gapless_after;
					opus_pending_frames[job.instance] = prepared.opus_plan.pending_frames_after;
					resample                          = prepared.opus_plan.resample_after;

					if (!PlanAjmSplitOutputs(job, &write_plans, &prepared.output_plans) ||
					    (job.result_address != 0 && !AddAjmPreflightOutputRange(&write_plans, job.result_address,
					                                                            sizeof(AjmDecodeResultPrefix), &prepared.result_plan)))
					{
						return AJM_ERROR_INVALID_PARAMETER;
					}
					resample_states[job.instance].reset_window = false;
				}
			}
			prepared_jobs.push_back(std::move(prepared));
		}
	}

	size_t error_plan = SIZE_MAX;
	if (error != nullptr && !AddAjmPreflightOutputRange(&write_plans, reinterpret_cast<uint64_t>(error), sizeof(*error), &error_plan))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	size_t batch_id_plan = SIZE_MAX;
	if (!AddAjmPreflightOutputRange(&write_plans, reinterpret_cast<uint64_t>(batch_id), sizeof(*batch_id), &batch_id_plan) ||
	    !VisitAjmWritablePlans(write_plans, &AjmNoopWritableVisitor, nullptr))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::vector<AjmStagedGuestWrite> staged_writes;
	staged_writes.reserve(write_plans.size());
	const std::array<uint8_t, AJM_SIDEBAND_RESULT_SIZE> control_result {};
	for (size_t index = 0; index < prepared_jobs.size(); ++index)
	{
		auto&                   prepared = prepared_jobs[index];
		const auto&             job      = *prepared.queued;
		AjmInstanceState* const instance = FindAjmInstanceState(state, job.instance);
		if (instance == nullptr)
		{
			lock.unlock();
			RejectMalformedAjmData("instance was destroyed during batch execution");
		}
		if (job.operation == AjmDataOperation::Initialize)
		{
			if (instance->decoder == nullptr)
			{
				++g_ajm_live_decoder_count;
			}
			instance->config      = job.config;
			instance->channels    = prepared.decoder->Channels();
			instance->sample_rate = prepared.decoder->SampleRate();
			instance->block_align = prepared.decoder->BlockAlign();
			instance->decoder     = std::move(prepared.decoder);
			instance->pending_pcm.clear();
			instance->pending_sample_offset      = 0;
			instance->delivered_samples          = 0;
			instance->gapless                    = {};
			instance->resample                   = {};
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, control_result.data(), AJM_SIDEBAND_RESULT_SIZE);
		} else if (job.operation == AjmDataOperation::Clear)
		{
			if (instance->decoder != nullptr)
			{
				instance->decoder->Reset();
			}
			instance->pending_pcm.clear();
			instance->pending_sample_offset = 0;
			instance->delivered_samples     = 0;
			instance->gapless               = {};
			ClearAjmResampleState(&instance->resample);
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, control_result.data(), AJM_SIDEBAND_RESULT_SIZE);
		} else if (job.operation == AjmDataOperation::SetGaplessDecode)
		{
			if (!ApplyAjmGaplessConfiguration(&instance->gapless, job.gapless_config, job.gapless_reset, instance->codec == 1))
			{
				lock.unlock();
				RejectMalformedAjmData("gapless state diverged from its batch preflight");
			}
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, control_result.data(), AJM_SIDEBAND_RESULT_SIZE);
		} else if (job.operation == AjmDataOperation::SetResampleParametersEx)
		{
			ApplyAjmResampleProfile(&instance->resample, job);
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, control_result.data(), AJM_SIDEBAND_RESULT_SIZE);
		} else if (job.operation == AjmDataOperation::GetResampleInfo)
		{
			const AjmResampleInfoPrefix info {0, 0, AjmReportedResampleRatio(instance->resample),
			                                  static_cast<int32_t>(instance->resample.held_frames)};
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, &info, sizeof(info));
		} else if (job.operation == AjmDataOperation::Decode || instance->codec == 1)
		{
			std::vector<uint8_t>  pcm;
			AjmDecodeResultPrefix result {};
			instance->resample.reset_window = false;
			if (!ProcessAjmAtrac9Decode(instance, job, prepared.input_data, &pcm, &result) ||
			    (prepared.output_plan != SIZE_MAX && pcm.size() > write_plans[prepared.output_plan].size))
			{
				lock.unlock();
				RejectMalformedAjmData("ATRAC9 compressed input could not be decoded");
			}
			if (job.operation == AjmDataOperation::Decode)
			{
				AddAjmStagedWrite(&staged_writes, prepared.output_plan, pcm.data(), pcm.size());
			} else if (!StageAjmSplitOutputs(job, prepared, write_plans, pcm, &staged_writes))
			{
				lock.unlock();
				RejectMalformedAjmData("ATRAC9 output exceeds its snapshotted descriptor list");
			}
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, &result, sizeof(result));
		} else
		{
			std::vector<uint8_t>  pcm;
			AjmDecodeResultPrefix result {};
			instance->resample.reset_window = false;
			if (!ProcessAjmOpusSplitDecode(instance, prepared, &pcm, &result) || pcm.size() > job.output_size)
			{
				lock.unlock();
				RejectMalformedAjmData("Opus split packet could not be decoded to its declared duration");
			}
			if (!StageAjmSplitOutputs(job, prepared, write_plans, pcm, &staged_writes))
			{
				lock.unlock();
				RejectMalformedAjmData("Opus output exceeds its snapshotted descriptor list");
			}
			AddAjmStagedWrite(&staged_writes, prepared.result_plan, &result, sizeof(result));
		}
	}
	const AjmBatchError clear_error {};
	AddAjmStagedWrite(&staged_writes, error_plan, &clear_error, sizeof(clear_error));
	AddAjmStagedWrite(&staged_writes, batch_id_plan, &next_batch_id, sizeof(next_batch_id));
	if (!CommitAjmGuestWrites(write_plans, staged_writes))
	{
		lock.unlock();
		RejectMalformedAjmData("guest output mapping changed before publication");
	}

	state.next_batch_id = next_batch_id;
	state.completed_batches.insert(next_batch_id);
	if (jobs_it != g_ajm_batch2_jobs.end())
	{
		g_ajm_batch2_jobs.erase(jobs_it);
	}
	return OK;
}

const char* KYTY_SYSV_ABI AjmStrError(int error)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t error = %d\n", error);
	return "AJM";
}

} // namespace Ajm
} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
