#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/AudioAcm.h"
#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Acm", 1, "Acm", 1, 1);

namespace Acm {

namespace Dsp = Audio::Acm;

constexpr int ERROR_INVALID_CONTEXT   = static_cast<int>(0x81940002u);
constexpr int ERROR_INVALID_BATCH     = static_cast<int>(0x81940003u);
constexpr int ERROR_INVALID_PARAMETER = static_cast<int>(0x81940006u);
constexpr int ERROR_BATCHBUFFER_FULL  = static_cast<int>(0x81940009u);
constexpr int ERROR_ALIGNMENT         = static_cast<int>(0x8194000Au);

constexpr uint64_t kDataAlignment   = 128;
constexpr uint32_t kFormatFp32      = 0;
constexpr uint32_t kFormatFp16      = 1;
constexpr uint32_t kFftZero2ndHalf  = 1u;
constexpr uint32_t kFftSplitInput   = 2u;
constexpr uint32_t kFftSplitOutput  = 4u;
constexpr uint32_t kMaxChannels     = 64;
constexpr size_t   kMaxPendingBatch = 65536;

// A batch is a sequence of 8-byte command words. The guest itself writes only the statistics job, inlined from the
// library header (opcode 11, then the statistics pointer). The exported job builders are emulated here, so they
// write Kyty's own words: the library's encoding is not published.
constexpr uint8_t kCommandStatistics     = 11;
constexpr uint8_t kCommandFft            = 0xf0;
constexpr uint8_t kCommandConvReverb     = 0xf1;

struct BatchInfo
{
	uint8_t* buffer;
	uint64_t offset;
	uint64_t size;
};
static_assert(sizeof(BatchInfo) == 24);

struct CommandHeader
{
	uint8_t  command;
	uint8_t  flags;
	uint8_t  input_format;
	uint8_t  output_format;
	uint16_t count;
	uint16_t size;
};
static_assert(sizeof(CommandHeader) == 8);

struct BlockSize
{
	uint16_t size_memory;
	uint16_t size;
};

struct ConvReverbChannel
{
	void*      blocks;
	BlockSize* sizes;
};
static_assert(sizeof(ConvReverbChannel) == 16);

struct ConvReverbIn
{
	uint32_t            block_size;
	uint32_t            block_count;
	uint32_t            channel_count;
	uint32_t            format;
	uint32_t            history_position;
	uint32_t            history_count;
	ConvReverbChannel** channels;
	float**             pcm;
};
static_assert(sizeof(ConvReverbIn) == 40);

struct ConvReverbIr
{
	uint32_t            block_size;
	uint32_t            block_count;
	uint32_t            channel_count;
	uint32_t            format;
	float*              block_gains;
	ConvReverbChannel** channels;
};
static_assert(sizeof(ConvReverbIr) == 32);

struct ConvReverbOut
{
	uint32_t block_size;
	uint32_t channel_count;
	float**  temps;
	float**  pcm;
};
static_assert(sizeof(ConvReverbOut) == 24);

struct Contexts
{
	std::mutex                                                   mutex;
	std::unordered_map<uint32_t, std::unordered_set<uint32_t>>   pending;
	uint32_t                                                     next_context = 1;
	uint32_t                                                     next_batch   = 1;
};

static Contexts* GetContexts()
{
	static auto* contexts = new Contexts;
	return contexts;
}

static bool IsAligned(const void* p)
{
	return p != nullptr && (reinterpret_cast<uintptr_t>(p) % kDataAlignment) == 0;
}

static uint32_t SampleBytes(uint32_t format)
{
	return format == kFormatFp16 ? 2u : 4u;
}

static void LoadSamples(const void* src, uint32_t format, uint32_t count, float* dst)
{
	if (format == kFormatFp32)
	{
		std::memcpy(dst, src, count * sizeof(float));
		return;
	}
	const auto* half = static_cast<const uint16_t*>(src);
	for (uint32_t i = 0; i < count; i++)
	{
		dst[i] = Dsp::HalfToFloat(half[i]);
	}
}

static void StoreSamples(const float* src, uint32_t format, uint32_t count, void* dst)
{
	if (format == kFormatFp32)
	{
		std::memcpy(dst, src, count * sizeof(float));
		return;
	}
	auto* half = static_cast<uint16_t*>(dst);
	for (uint32_t i = 0; i < count; i++)
	{
		half[i] = Dsp::FloatToHalf(src[i]);
	}
}

// Reserves words in the batch buffer; null when they do not fit.
static uint64_t* AppendWords(BatchInfo* info, uint64_t words)
{
	const uint64_t bytes = words * sizeof(uint64_t);
	if (info->offset > info->size || bytes > info->size - info->offset)
	{
		return nullptr;
	}
	auto* out = reinterpret_cast<uint64_t*>(info->buffer + info->offset);
	info->offset += bytes;
	return out;
}

static uint64_t HeaderWord(const CommandHeader& header)
{
	uint64_t word = 0;
	std::memcpy(&word, &header, sizeof(word));
	return word;
}

// sceAcm_FFT — NID KovqaFbmtsM. Each of iCount transforms reads iSize real samples, or iSize / 2 followed by zeros
// with ZERO_2ND_HALF, and writes the packed iSize-float spectrum.
static int KYTY_SYSV_ABI AcmFft(BatchInfo* info, int size, int count, uint32_t input_format, const void* const* inputs,
                                uint32_t output_format, void* const* outputs, uint32_t flags)
{
	PRINT_NAME();

	if (info == nullptr || inputs == nullptr || outputs == nullptr || count <= 0 || count > 0xffff ||
	    input_format > kFormatFp16 || output_format > kFormatFp16 || (flags & ~(kFftZero2ndHalf | kFftSplitInput | kFftSplitOutput)) != 0)
	{
		return ERROR_INVALID_PARAMETER;
	}
	if (size != 512 && size != 1024 && size != 2048)
	{
		return ERROR_INVALID_PARAMETER;
	}
	// Which halves the split pointer pairs address is not published.
	EXIT_IF((flags & (kFftSplitInput | kFftSplitOutput)) != 0);
	for (int i = 0; i < count; i++)
	{
		if (!IsAligned(inputs[i]) || !IsAligned(outputs[i]))
		{
			return inputs[i] == nullptr || outputs[i] == nullptr ? ERROR_INVALID_PARAMETER : ERROR_ALIGNMENT;
		}
	}

	uint64_t* words = AppendWords(info, 1u + 2u * static_cast<uint64_t>(count));
	if (words == nullptr)
	{
		return ERROR_BATCHBUFFER_FULL;
	}
	words[0] = HeaderWord({kCommandFft, static_cast<uint8_t>(flags), static_cast<uint8_t>(input_format), static_cast<uint8_t>(output_format),
	                       static_cast<uint16_t>(count), static_cast<uint16_t>(size)});
	for (int i = 0; i < count; i++)
	{
		words[1 + 2 * i] = reinterpret_cast<uint64_t>(inputs[i]);
		words[2 + 2 * i] = reinterpret_cast<uint64_t>(outputs[i]);
	}
	return OK;
}

static const uint64_t* RunFft(const uint64_t* words, const CommandHeader& header)
{
	const uint32_t     n    = header.size;
	const uint32_t     read = (header.flags & kFftZero2ndHalf) != 0 ? n / 2u : n;
	std::vector<float> samples(n);
	std::vector<float> spectrum(n);
	for (uint32_t i = 0; i < header.count; i++)
	{
		std::fill(samples.begin(), samples.end(), 0.0f);
		LoadSamples(reinterpret_cast<const void*>(words[2u * i]), header.input_format, read, samples.data());
		Dsp::ForwardTransform(samples.data(), n, spectrum.data());
		StoreSamples(spectrum.data(), header.output_format, n, reinterpret_cast<void*>(words[2u * i + 1u]));
	}
	return words + 2u * header.count;
}

static bool ValidChannels(ConvReverbChannel* const* channels, uint32_t count)
{
	if (channels == nullptr)
	{
		return false;
	}
	return std::all_of(channels, channels + count, [](const ConvReverbChannel* c) { return c != nullptr && IsAligned(c->blocks); });
}

static bool ValidBuffers(float* const* buffers, uint32_t count)
{
	return buffers != nullptr && std::all_of(buffers, buffers + count, [](const float* p) { return IsAligned(p); });
}

static bool ValidIn(const ConvReverbIn& in)
{
	const bool sizes_unused =
	    in.channels != nullptr && std::all_of(in.channels, in.channels + in.channel_count, [](const ConvReverbChannel* c) { return c == nullptr || c->sizes == nullptr; });
	return (in.block_size == 256 || in.block_size == 512 || in.block_size == 1024) && in.block_count != 0 && in.channel_count != 0 &&
	       in.format <= kFormatFp16 && ValidChannels(in.channels, in.channel_count) && sizes_unused && ValidBuffers(in.pcm, in.channel_count);
}

static bool ValidIr(const ConvReverbIr* ir, uint32_t block_size)
{
	return ir != nullptr && ir->block_size == block_size && ir->block_count != 0 && ir->channel_count != 0 && ir->format <= kFormatFp16 &&
	       ValidChannels(ir->channels, ir->channel_count);
}

// 1:N, N:1 and N:N input-to-IR channel layouts are supported; the output has the larger count.
static bool ValidOut(const ConvReverbOut* out, uint32_t block_size, uint32_t input_channels, uint32_t ir_channels)
{
	const bool compatible = input_channels == ir_channels || input_channels == 1u || ir_channels == 1u;
	return out != nullptr && compatible && out->block_size == block_size && out->channel_count == std::max(input_channels, ir_channels) &&
	       ValidBuffers(out->temps, out->channel_count) && ValidBuffers(out->pcm, out->channel_count);
}

// sceAcm_ConvReverb_SharedInput — NID u70oWo92SYQ. Uniformly partitioned convolution of one input against iCount
// impulse responses. Each call transforms the input's latest block (iBlockSize samples, zero-padded to twice that)
// into its history ring, then each output block is the inverse transform of the sum over IR blocks k of
// H_k * X_(n-k), scaled by the IR's block gain and pGain[i], overlap-added with the tail carried in pTemps.
static int KYTY_SYSV_ABI AcmConvReverbSharedInput(BatchInfo* info, uint32_t block_count, ConvReverbIn* in, uint32_t count,
                                                  const ConvReverbIr* const* irs, const float* gains, const ConvReverbOut* const* outs)
{
	PRINT_NAME();

	if (info == nullptr || in == nullptr || irs == nullptr || outs == nullptr || count == 0 || count > 0xffff || block_count != 1u)
	{
		return ERROR_INVALID_PARAMETER;
	}
	EXIT_IF(in->channel_count > kMaxChannels);
	if (!ValidIn(*in))
	{
		return ERROR_INVALID_PARAMETER;
	}
	for (uint32_t i = 0; i < count; i++)
	{
		EXIT_IF(irs[i] != nullptr && irs[i]->channel_count > kMaxChannels);
		if (!ValidIr(irs[i], in->block_size) || !ValidOut(outs[i], in->block_size, in->channel_count, irs[i]->channel_count))
		{
			return ERROR_INVALID_PARAMETER;
		}
		// Truncated IR blocks are stored back to back at their memory size; how the rest of a block reads is not
		// published.
		EXIT_IF(std::any_of(irs[i]->channels, irs[i]->channels + irs[i]->channel_count, [](const ConvReverbChannel* c) { return c->sizes != nullptr; }));
	}

	uint64_t* words = AppendWords(info, 2u + in->channel_count + 3u * static_cast<uint64_t>(count));
	if (words == nullptr)
	{
		return ERROR_BATCHBUFFER_FULL;
	}
	// The history position names the slot of the most recent block; the count saturates at the ring size.
	in->history_position = in->history_count == 0 ? in->history_position % in->block_count : (in->history_position + 1u) % in->block_count;
	in->history_count    = std::min(in->history_count + 1u, in->block_count);

	words[0] = HeaderWord({kCommandConvReverb, 0, 0, 0, static_cast<uint16_t>(count), 0});
	words[1] = reinterpret_cast<uint64_t>(in);
	words[2] = in->history_position | (static_cast<uint64_t>(in->history_count) << 32u);
	for (uint32_t c = 0; c < in->channel_count; c++)
	{
		words[3 + c] = reinterpret_cast<uint64_t>(in->pcm[c]);
	}
	uint64_t* jobs = words + 3 + in->channel_count;
	for (uint32_t i = 0; i < count; i++)
	{
		const float gain = gains != nullptr ? gains[i] : 1.0f;
		uint32_t    bits = 0;
		std::memcpy(&bits, &gain, sizeof(bits));
		jobs[3 * i]     = reinterpret_cast<uint64_t>(irs[i]);
		jobs[3 * i + 1] = reinterpret_cast<uint64_t>(outs[i]);
		jobs[3 * i + 2] = bits;
	}
	return OK;
}

// A block holds the packed spectrum of 2 * block_size samples.
static const uint8_t* SpectrumBlock(const ConvReverbChannel* channel, uint32_t format, uint32_t block_size, uint32_t index)
{
	return static_cast<const uint8_t*>(channel->blocks) + static_cast<uint64_t>(index) * 2u * block_size * SampleBytes(format);
}

struct HistoryView
{
	const ConvReverbIn* in;
	uint32_t            position;
	uint32_t            count;
};

static void ConvolveChannel(const HistoryView& history, const ConvReverbChannel* input, const ConvReverbIr& ir, const ConvReverbChannel* response,
                            float gain, float* temps, float* pcm)
{
	const uint32_t     block = history.in->block_size;
	const uint32_t     n     = 2u * block;
	const uint32_t     terms = std::min({history.count, history.in->block_count, ir.block_count});
	std::vector<float> sum(n, 0.0f);
	std::vector<float> x(n);
	std::vector<float> h(n);
	for (uint32_t k = 0; k < terms; k++)
	{
		const uint32_t slot = (history.position + history.in->block_count - k) % history.in->block_count;
		LoadSamples(SpectrumBlock(input, history.in->format, block, slot), history.in->format, n, x.data());
		LoadSamples(SpectrumBlock(response, ir.format, block, k), ir.format, n, h.data());
		const float block_gain = ir.block_gains != nullptr ? ir.block_gains[k] : 1.0f;
		Dsp::MultiplyAccumulate(x.data(), h.data(), gain * block_gain, n, sum.data());
	}
	std::vector<float> y(n);
	Dsp::InverseTransform(sum.data(), n, y.data());
	for (uint32_t j = 0; j < block; j++)
	{
		pcm[j]   = y[j] + temps[j];
		temps[j] = y[block + j];
	}
}

static const uint64_t* RunConvReverb(const uint64_t* words, const CommandHeader& header)
{
	const auto*    in      = reinterpret_cast<const ConvReverbIn*>(words[0]);
	const auto     history = HistoryView {in, static_cast<uint32_t>(words[1]), static_cast<uint32_t>(words[1] >> 32u)};
	const uint32_t block   = in->block_size;

	std::vector<float> samples(2u * block, 0.0f);
	std::vector<float> spectrum(2u * block);
	for (uint32_t c = 0; c < in->channel_count; c++)
	{
		LoadSamples(reinterpret_cast<const float*>(words[2 + c]), kFormatFp32, block, samples.data());
		Dsp::ForwardTransform(samples.data(), 2u * block, spectrum.data());
		StoreSamples(spectrum.data(), in->format, 2u * block,
		             const_cast<uint8_t*>(SpectrumBlock(in->channels[c], in->format, block, history.position)));
	}

	const uint64_t* jobs = words + 2 + in->channel_count;
	for (uint32_t i = 0; i < header.count; i++)
	{
		const auto* ir   = reinterpret_cast<const ConvReverbIr*>(jobs[3 * i]);
		const auto* out  = reinterpret_cast<const ConvReverbOut*>(jobs[3 * i + 1]);
		const auto  bits = static_cast<uint32_t>(jobs[3 * i + 2]);
		float       gain = 0.0f;
		std::memcpy(&gain, &bits, sizeof(gain));
		for (uint32_t c = 0; c < out->channel_count; c++)
		{
			const ConvReverbChannel* input    = in->channels[in->channel_count == 1u ? 0u : c];
			const ConvReverbChannel* response = ir->channels[ir->channel_count == 1u ? 0u : c];
			ConvolveChannel(history, input, *ir, response, gain, out->temps[c], out->pcm[c]);
		}
	}
	return jobs + 3u * header.count;
}

static void RunBatch(const BatchInfo& info)
{
	const auto*     words = reinterpret_cast<const uint64_t*>(info.buffer);
	const uint64_t* end   = words + info.offset / sizeof(uint64_t);
	while (words < end)
	{
		CommandHeader header {};
		std::memcpy(&header, words, sizeof(header));
		words++;
		switch (header.command)
		{
			case kCommandFft: words = RunFft(words, header); break;
			case kCommandConvReverb: words = RunConvReverb(words, header); break;
			// The statistics job reports the audio compute unit's usage, which the host does not model.
			case kCommandStatistics: EXIT("Acm statistics job is not modeled\n"); break;
			default: EXIT("Acm batch command 0x%02x is not modeled\n", header.command);
		}
	}
}

// sceAcmContextCreate — NID ZIXln2K3XMk. Its only argument is the context it
// creates, a 32-bit handle: a title tests it with a 32-bit compare and creates
// it while it is zero, tail-calling this function with the other argument
// registers holding whatever the caller left there. Reading one of them as a
// buffer size cleared that many bytes past the handle and destroyed the audio
// engine globals that follow it in the title's data.
static int KYTY_SYSV_ABI AcmContextCreate(uint32_t* ctx)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t ctx = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ctx));

	if (ctx == nullptr)
	{
		return ERROR_INVALID_PARAMETER;
	}
	auto*                 contexts = GetContexts();
	std::lock_guard<std::mutex> lock(contexts->mutex);
	const uint32_t        id = contexts->next_context++;
	contexts->pending[id];
	*ctx = id;
	return OK;
}

// sceAcmContextDestroy — NID jBgBjAj02R8.
static int KYTY_SYSV_ABI AcmContextDestroy(uint32_t ctx)
{
	PRINT_NAME();

	auto*                       contexts = GetContexts();
	std::lock_guard<std::mutex> lock(contexts->mutex);
	return contexts->pending.erase(ctx) != 0 ? OK : ERROR_INVALID_CONTEXT;
}

// sceAcmBatchStartBuffers — NID 8fe55ktlNVo. A count of 0 starts the single batch the inline sceAcmBatchStart passes.
// The batch runs to completion here, so its first wait succeeds.
static int KYTY_SYSV_ABI AcmBatchStartBuffers(uint32_t ctx, uint32_t count, const BatchInfo* const* infos, void* batch_error, uint32_t* batch)
{
	PRINT_NAME();

	auto* contexts = GetContexts();
	{
		std::lock_guard<std::mutex> lock(contexts->mutex);
		if (contexts->pending.count(ctx) == 0)
		{
			return ERROR_INVALID_CONTEXT;
		}
	}
	if (infos == nullptr || batch == nullptr)
	{
		return ERROR_INVALID_PARAMETER;
	}
	// The batch error report is documented as not implemented; only null is accepted.
	EXIT_IF(batch_error != nullptr);
	const uint32_t infos_count = count == 0 ? 1u : count;
	for (uint32_t i = 0; i < infos_count; i++)
	{
		const BatchInfo* info = infos[i];
		if (info == nullptr || info->buffer == nullptr || info->offset > info->size || (info->offset % sizeof(uint64_t)) != 0)
		{
			return ERROR_INVALID_PARAMETER;
		}
	}
	for (uint32_t i = 0; i < infos_count; i++)
	{
		RunBatch(*infos[i]);
	}

	std::lock_guard<std::mutex> lock(contexts->mutex);
	auto                        pending = contexts->pending.find(ctx);
	if (pending == contexts->pending.end())
	{
		return ERROR_INVALID_CONTEXT;
	}
	EXIT_IF(pending->second.size() >= kMaxPendingBatch);
	uint32_t id = contexts->next_batch++;
	if (id == UINT32_MAX)
	{
		id = contexts->next_batch++;
	}
	pending->second.insert(id);
	*batch = id;
	return OK;
}

// sceAcmBatchWait — NID RLN3gRlXJLE. Every started batch has already completed; a successful wait releases it.
static int KYTY_SYSV_ABI AcmBatchWait(uint32_t ctx, uint32_t batch, uint32_t timeout)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t batch = %u, timeout = %u\n", batch, timeout);

	auto*                       contexts = GetContexts();
	std::lock_guard<std::mutex> lock(contexts->mutex);
	auto                        pending = contexts->pending.find(ctx);
	if (pending == contexts->pending.end())
	{
		return ERROR_INVALID_CONTEXT;
	}
	return pending->second.erase(batch) != 0 ? OK : ERROR_INVALID_BATCH;
}

} // namespace Acm

LIB_DEFINE(InitAcm_1)
{
	LIB_FUNC("ZIXln2K3XMk", Acm::AcmContextCreate);
	LIB_FUNC("jBgBjAj02R8", Acm::AcmContextDestroy);
	LIB_FUNC("8fe55ktlNVo", Acm::AcmBatchStartBuffers);
	LIB_FUNC("RLN3gRlXJLE", Acm::AcmBatchWait);
	LIB_FUNC("KovqaFbmtsM", Acm::AcmFft);
	LIB_FUNC("u70oWo92SYQ", Acm::AcmConvReverbSharedInput);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
