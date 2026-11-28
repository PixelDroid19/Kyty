#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

UT_BEGIN(EmulatorAcm);

namespace {

// Guest layouts of the audio compute library's batch and convolution reverb structures.
struct BatchInfo
{
	void*    buffer;
	uint64_t offset;
	uint64_t size;
};

struct Channel
{
	void* blocks;
	void* sizes;
};

struct ReverbIn
{
	uint32_t  block_size;
	uint32_t  block_count;
	uint32_t  channel_count;
	uint32_t  format;
	uint32_t  history_position;
	uint32_t  history_count;
	Channel** channels;
	float**   pcm;
};

struct ReverbIr
{
	uint32_t  block_size;
	uint32_t  block_count;
	uint32_t  channel_count;
	uint32_t  format;
	float*    block_gains;
	Channel** channels;
};

struct ReverbOut
{
	uint32_t block_size;
	uint32_t channel_count;
	float**  temps;
	float**  pcm;
};

using ContextCreate = int(KYTY_SYSV_ABI*)(uint32_t*);
using StartBuffers  = int(KYTY_SYSV_ABI*)(uint32_t, uint32_t, const BatchInfo* const*, void*, uint32_t*);
using BatchWait     = int(KYTY_SYSV_ABI*)(uint32_t, uint32_t, uint32_t);
using Fft           = int(KYTY_SYSV_ABI*)(BatchInfo*, int, int, uint32_t, const void* const*, uint32_t, void* const*, uint32_t);
using ConvReverb    = int(KYTY_SYSV_ABI*)(BatchInfo*, uint32_t, ReverbIn*, uint32_t, const ReverbIr* const*, const float*,
                                       const ReverbOut* const*);

constexpr uint32_t kFp32        = 0;
constexpr uint32_t kFp16        = 1;
constexpr uint32_t kZero2ndHalf = 1;

struct alignas(128) Samples
{
	float v[2048];
};

template <typename Function>
Function ResolveAcmExport(Kyty::Loader::SymbolDatabase* symbols, const char* name)
{
	Kyty::Loader::SymbolResolve query {};
	query.name                 = Kyty::Loader::EncodeNameAsNid(name);
	query.library              = U"Acm";
	query.library_version      = 1;
	query.module               = U"Acm";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Kyty::Loader::SymbolType::Func;
	const auto* record         = symbols->Find(query);
	return record != nullptr ? reinterpret_cast<Function>(record->vaddr) : nullptr;
}

struct Acm
{
	Kyty::Loader::SymbolDatabase symbols;
	StartBuffers                 start   = nullptr;
	BatchWait                    wait    = nullptr;
	Fft                          fft     = nullptr;
	ConvReverb                   reverb  = nullptr;
	uint32_t                     context = 0;

	bool Load()
	{
		if (!Kyty::Libs::Init(U"libAcm_1", &symbols))
		{
			return false;
		}
		auto create = ResolveAcmExport<ContextCreate>(&symbols, "sceAcmContextCreate");
		start       = ResolveAcmExport<StartBuffers>(&symbols, "sceAcmBatchStartBuffers");
		wait        = ResolveAcmExport<BatchWait>(&symbols, "sceAcmBatchWait");
		fft         = ResolveAcmExport<Fft>(&symbols, "sceAcm_FFT");
		reverb      = ResolveAcmExport<ConvReverb>(&symbols, "sceAcm_ConvReverb_SharedInput");
		return create != nullptr && start != nullptr && wait != nullptr && fft != nullptr && reverb != nullptr && create(&context) == 0;
	}

	// Starts one recorded batch the way the inline sceAcmBatchStart does and polls it once.
	bool Run(const BatchInfo& info) const
	{
		const BatchInfo* infos[] = {&info};
		uint32_t         batch   = 0;
		return start(context, 0, infos, nullptr, &batch) == 0 && wait(context, batch, 0) == 0;
	}
};

} // namespace

TEST(EmulatorAcm, FftPacksDcAndNyquistIntoTheFirstBinAndZeroPadsOnRequest)
{
	Acm acm;
	ASSERT_TRUE(acm.Load());

	constexpr uint32_t n          = 512;
	auto               input      = std::make_unique<Samples>();
	auto               spectrum   = std::make_unique<Samples>();
	auto               padded     = std::make_unique<Samples>();
	auto               half       = std::make_unique<Samples>();
	auto               half_spect = std::make_unique<Samples>();
	const double       pi         = 3.14159265358979323846;
	for (uint32_t i = 0; i < n; i++)
	{
		input->v[i]  = static_cast<float>(1.0 + 2.0 * ((i & 1u) != 0 ? -1.0 : 1.0) + 4.0 * std::cos(2.0 * pi * 8.0 * i / n));
		padded->v[i] = i < n / 2u ? 1.0f : 0.0f;
		// The second half must not be read under ZERO_2ND_HALF.
		half->v[i] = i < n / 2u ? 1.0f : 1000.0f;
	}

	uint64_t    words[64] {};
	BatchInfo   info {words, 0, sizeof(words)};
	const void* inputs[]  = {input->v, padded->v, half->v};
	void*       outputs[] = {spectrum->v, padded->v, half_spect->v};
	ASSERT_EQ(acm.fft(&info, n, 2, kFp32, inputs, kFp32, outputs, 0), 0);
	ASSERT_EQ(acm.fft(&info, n, 1, kFp32, inputs + 2, kFp32, outputs + 2, kZero2ndHalf), 0);
	ASSERT_TRUE(acm.Run(info));

	// DC at float 0, Nyquist at float 1, bin 8 at floats 16 and 17; ratios keep the assertion free of the scale.
	const float dc = spectrum->v[0];
	ASSERT_GT(dc, 0.0f);
	for (uint32_t i = 1; i < n; i++)
	{
		const float expected = i == 1u || i == 16u ? 2.0f : 0.0f;
		EXPECT_NEAR(spectrum->v[i] / dc, expected, 1e-4f) << "float " << i;
	}
	for (uint32_t i = 0; i < n; i++)
	{
		EXPECT_FLOAT_EQ(half_spect->v[i], padded->v[i]) << "float " << i;
	}
}

TEST(EmulatorAcm, ConvReverbReproducesTheImpulseResponseAcrossBlocks)
{
	Acm acm;
	ASSERT_TRUE(acm.Load());

	constexpr uint32_t block = 256;
	constexpr uint32_t n     = 2 * block;
	constexpr uint32_t delay = 5;
	constexpr float    gain  = 0.5f;
	std::vector<float> response(n);
	for (uint32_t j = 0; j < n; j++)
	{
		response[j] = 1.0f / static_cast<float>(1u + j);
	}

	for (const uint32_t format: {kFp32, kFp16})
	{
		const uint32_t block_bytes = n * (format == kFp16 ? 2u : 4u);
		auto           ir_time     = std::make_unique<Samples>();
		auto           ir_blocks   = std::make_unique<Samples>();
		auto           history     = std::make_unique<Samples>();
		auto           pcm         = std::make_unique<Samples>();
		auto           temps       = std::make_unique<Samples>();
		auto           out         = std::make_unique<Samples>();
		std::copy(response.begin(), response.end(), ir_time->v);
		std::fill(std::begin(temps->v), std::end(temps->v), 0.0f);

		// The IR is prepared as the sample code does: one zero-padded transform per block.
		uint64_t    ir_words[16] {};
		BatchInfo   ir_info {ir_words, 0, sizeof(ir_words)};
		const void* ir_inputs[]  = {ir_time->v, ir_time->v + block};
		void*       ir_outputs[] = {ir_blocks->v, reinterpret_cast<uint8_t*>(ir_blocks->v) + block_bytes};
		ASSERT_EQ(acm.fft(&ir_info, n, 2, kFp32, ir_inputs, format, ir_outputs, kZero2ndHalf), 0);
		ASSERT_TRUE(acm.Run(ir_info));

		Channel         input_channel {history->v, nullptr};
		Channel         ir_channel {ir_blocks->v, nullptr};
		Channel*        input_channels[] = {&input_channel};
		Channel*        ir_channels[]    = {&ir_channel};
		float*          input_pcm[]      = {pcm->v};
		float*          output_temps[]   = {temps->v};
		float*          output_pcm[]     = {out->v};
		ReverbIn        in {block, 2, 1, format, 0, 0, input_channels, input_pcm};
		const ReverbIr  ir {block, 2, 1, format, nullptr, ir_channels};
		const ReverbOut output {block, 1, output_temps, output_pcm};
		const ReverbIr* irs[]  = {&ir};
		const ReverbOut* outs[] = {&output};

		std::vector<float> result;
		for (uint32_t b = 0; b < 3; b++)
		{
			std::fill(std::begin(pcm->v), std::end(pcm->v), 0.0f);
			pcm->v[delay] = b == 0 ? 1.0f : 0.0f;
			uint64_t  words[64] {};
			BatchInfo info {words, 0, sizeof(words)};
			ASSERT_EQ(acm.reverb(&info, 1, &in, 1, irs, &gain, outs), 0);
			ASSERT_TRUE(acm.Run(info));
			result.insert(result.end(), out->v, out->v + block);
		}

		const float tolerance = format == kFp16 ? 2e-3f : 1e-5f;
		for (uint32_t t = 0; t < result.size(); t++)
		{
			const float expected = t >= delay && t < delay + n ? gain * response[t - delay] : 0.0f;
			EXPECT_NEAR(result[t], expected, tolerance) << "format " << format << " sample " << t;
		}
	}
}

UT_END();
