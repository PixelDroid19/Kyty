// External operand endpoint for the integer instruction oracle.
//
// For each batch of up to 16 uint32 pairs, the guest program moves operand a[k] into
// v(2k) and b[k] into v(2k+1) with v_mov_b32 literals, then executes the requested
// operation as one guest instruction per pair, writing v(32+k). The literals come from
// the external input file. No arithmetic result is computed on the host and inserted.
// The production parser and SPIR-V emitter lower the guest words, the production
// toolchain assembles them, and VulkanComputeProbe runs them. The probe only observes
// VGPR results; it never inserts arithmetic.
//
// Exit status: 0 measured output written; 1 execution or validation failure, nothing
// written; 2 usage or refused input (the request violates a bound); 77 the compute
// capability is unavailable before any dispatch (no output written).

#include "VulkanComputeProbe.h"
#include "ShaderWaveProbeSource.h"

#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Kyty::Libs::Graphics::BuildWaveProbeSource;
using Kyty::Libs::Graphics::ShaderComputeInputInfo;
using Kyty::Libs::Graphics::ShaderComputeWaveLayoutStatus;
using Kyty::Libs::Graphics::ShaderComputeWaveRequest;
using Kyty::Libs::Graphics::ShaderGuestLaneOrder;
using Kyty::Libs::Graphics::ShaderToolchain::Run;
using Kyty::Libs::Graphics::VulkanComputeProbe;
using Kyty::Libs::Graphics::WaveProbeObservation;

constexpr uint32_t kLogicalLanes     = 64;     // paired logical wave64
constexpr uint32_t kBatchPairs       = 16;     // pairs per dispatch (one observed result each)
constexpr uint32_t kMaxPairs         = 1024;   // 16 pairs x 64 dispatches
constexpr uint32_t kMaxDispatches    = 64;
constexpr uint32_t kFirstResultVgpr  = 32;     // operation results are written to v32..v47
constexpr uint32_t kSentinel         = 0xa5c37e19u;
constexpr uint32_t kEndProgram       = 0xbf810000u; // s_endpgm
constexpr uint32_t kSubgroupSize     = 32;     // physical lanes the paired64 layout runs on

constexpr int kExitFailure     = 1;
constexpr int kExitUsage       = 2;
constexpr int kExitUnavailable = 77;

enum class Op
{
	Add,
	Mul,
	MulHi,
	Shl,
	Shr,
};

struct Arguments
{
	Op       op = Op::Add;
	uint32_t count = 0;
	uint32_t max_dispatches = 0;
	uint32_t wave_width = 0;
	std::string inputs;
	std::string output;
	std::string meta;
};

const char* OpName(Op op)
{
	switch (op)
	{
		case Op::Add: return "add";
		case Op::Mul: return "mul";
		case Op::MulHi: return "mulhi";
		case Op::Shl: return "shl";
		case Op::Shr: return "shr";
	}
	return "unknown";
}

bool ParseOp(const std::string& text, Op* op)
{
	for (const Op candidate: {Op::Add, Op::Mul, Op::MulHi, Op::Shl, Op::Shr})
	{
		if (text == OpName(candidate))
		{
			*op = candidate;
			return true;
		}
	}
	return false;
}

bool ParseUnsigned(const char* text, uint32_t* value)
{
	if (text == nullptr || *text == '\0' || std::strlen(text) > 9)
	{
		return false;
	}
	uint32_t result = 0;
	for (const char* p = text; *p != '\0'; ++p)
	{
		if (*p < '0' || *p > '9')
		{
			return false;
		}
		result = result * 10u + static_cast<uint32_t>(*p - '0');
	}
	*value = result;
	return true;
}

bool IsAbsolute(const std::string& path)
{
	return !path.empty() && std::filesystem::path(path).is_absolute();
}

// Accepts the fixed endpoint flags only. Each flag appears once and every flag is required.
bool ParseArguments(int argc, char** argv, Arguments* args)
{
	bool seen_op = false, seen_count = false, seen_inputs = false, seen_output = false, seen_meta = false,
	     seen_wave = false, seen_max = false;
	if (argc != 15)
	{
		return false;
	}
	for (int i = 1; i + 1 < argc; i += 2)
	{
		const std::string flag = argv[i];
		const char*       value = argv[i + 1];
		if (flag == "--op" && !seen_op)
		{
			seen_op = ParseOp(value, &args->op);
			if (!seen_op) { return false; }
		} else if (flag == "--count" && !seen_count)
		{
			seen_count = ParseUnsigned(value, &args->count);
			if (!seen_count) { return false; }
		} else if (flag == "--inputs" && !seen_inputs)
		{
			args->inputs = value;
			seen_inputs  = true;
		} else if (flag == "--output" && !seen_output)
		{
			args->output = value;
			seen_output  = true;
		} else if (flag == "--meta" && !seen_meta)
		{
			args->meta = value;
			seen_meta  = true;
		} else if (flag == "--wave-width" && !seen_wave)
		{
			seen_wave = ParseUnsigned(value, &args->wave_width);
			if (!seen_wave) { return false; }
		} else if (flag == "--max-dispatches" && !seen_max)
		{
			seen_max = ParseUnsigned(value, &args->max_dispatches);
			if (!seen_max) { return false; }
		} else
		{
			return false;
		}
	}
	return seen_op && seen_count && seen_inputs && seen_output && seen_meta && seen_wave && seen_max;
}

// Reads the operand planes after checking the exact size. The caller has already bounded count.
bool ReadInputPairs(const std::string& path, uint32_t count, std::vector<uint32_t>* a, std::vector<uint32_t>* b, int* exit_code)
{
	std::error_code ec;
	const auto      status = std::filesystem::status(path, ec);
	if (ec || !std::filesystem::is_regular_file(status))
	{
		std::fprintf(stderr, "input must be an existing regular file\n");
		*exit_code = kExitUsage;
		return false;
	}
	const auto size = std::filesystem::file_size(path, ec);
	if (ec || size != static_cast<uintmax_t>(count) * 8u)
	{
		std::fprintf(stderr, "input size must be exactly 8 bytes per pair\n");
		*exit_code = kExitUsage;
		return false;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	std::FILE*           file = std::fopen(path.c_str(), "rb");
	if (file == nullptr || std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
	{
		if (file != nullptr) { std::fclose(file); }
		std::fprintf(stderr, "input could not be read\n");
		*exit_code = kExitFailure;
		return false;
	}
	std::fclose(file);
	a->assign(count, 0);
	b->assign(count, 0);
	for (uint32_t k = 0; k < count; ++k)
	{
		for (uint32_t byte = 0; byte < 4; ++byte)
		{
			(*a)[k] |= static_cast<uint32_t>(bytes[k * 4u + byte]) << (8u * byte);
			(*b)[k] |= static_cast<uint32_t>(bytes[(count + k) * 4u + byte]) << (8u * byte);
		}
	}
	return true;
}

// v_mov_b32 vdst, literal (VOP1, src0 = literal constant 0xff).
void AppendMoveLiteral(std::vector<uint32_t>* words, uint32_t vdst, uint32_t literal)
{
	words->push_back(0x7e000000u | (vdst << 17u) | (0x01u << 9u) | 0xffu);
	words->push_back(literal);
}

// VOP3A two-source form: word0 = prefix | opcode << 16 | vdst; word1 = src0 | src1 << 9 | src2 << 18.
// src2 is not a live operand for these opcodes, so its selector is zero padding. The strict decoder
// rejects any nonzero unused selector, so it must not carry an inline constant.
void AppendVop3(std::vector<uint32_t>* words, uint32_t opcode, uint32_t vdst, uint32_t src0, uint32_t src1)
{
	words->push_back(0xd5000000u | (opcode << 16u) | vdst);
	words->push_back(src0 | (src1 << 9u));
}

// Operation for pair k. Operands are a = v(2k) and b = v(2k+1). VGPR sources use 256 + register.
// Shifts take the count in src0 (low five bits) and the value in vsrc1: D = value << (count & 31).
void AppendOperation(std::vector<uint32_t>* words, Op op, uint32_t k)
{
	const uint32_t dst = kFirstResultVgpr + k;
	const uint32_t a   = 2u * k;
	const uint32_t b   = 2u * k + 1u;
	switch (op)
	{
		case Op::Add: words->push_back((0x25u << 25u) | (dst << 17u) | (b << 9u) | (256u + a)); break; // v_add_nc_u32
		case Op::Shr: words->push_back((0x16u << 25u) | (dst << 17u) | (a << 9u) | (256u + b)); break; // v_lshrrev_b32
		case Op::Shl: words->push_back((0x1au << 25u) | (dst << 17u) | (a << 9u) | (256u + b)); break; // v_lshlrev_b32
		case Op::Mul: AppendVop3(words, 0x169u, dst, 256u + a, 256u + b); break;                       // v_mul_lo_u32
		case Op::MulHi: AppendVop3(words, 0x16au, dst, 256u + a, 256u + b); break;                     // v_mul_hi_u32
	}
}

// Runs one batch of n pairs through the production path and returns one verified result per pair.
bool RunBatch(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, Op op, const uint32_t* a, const uint32_t* b,
              uint32_t n, uint32_t* results, std::string* error)
{
	std::vector<uint32_t> words;
	for (uint32_t k = 0; k < n; ++k)
	{
		AppendMoveLiteral(&words, 2u * k, a[k]);
		AppendMoveLiteral(&words, 2u * k + 1u, b[k]);
	}
	for (uint32_t k = 0; k < n; ++k)
	{
		AppendOperation(&words, op, k);
	}
	words.push_back(kEndProgram);

	std::vector<WaveProbeObservation> observations;
	for (uint32_t k = 0; k < n; ++k)
	{
		const std::string name = "v" + std::to_string(kFirstResultVgpr + k);
		// VGPR results are float-declared slots; the probe reloads them as float and bitcasts to uint.
		observations.push_back({name + "_low", name + "_high", true});
	}

	Kyty::String8 source;
	Kyty::String8 build_error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, {}, &source, &build_error))
	{
		*error = build_error.c_str();
		return false;
	}
	Kyty::Vector<uint32_t> binary;
	if (!Run(source, &binary, &build_error))
	{
		*error = build_error.c_str();
		return false;
	}

	const size_t           stride = observations.size();
	std::vector<uint32_t>  initial(static_cast<size_t>(kLogicalLanes) * stride * 2u, kSentinel);
	std::vector<uint32_t>  result(initial.size());
	std::string            message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		*error = "dispatch failed: " + message;
		return false;
	}

	// Every logical lane runs the same program with uniform literals, so each pair's result must be
	// identical across all 64 lanes (both banks). Any difference is a failure, not a value to pick.
	for (uint32_t k = 0; k < n; ++k)
	{
		const uint32_t value = result[k];
		for (uint32_t lane = 0; lane < kLogicalLanes; ++lane)
		{
			if (result[static_cast<size_t>(lane) * stride + k] != value)
			{
				*error = "lane replicas disagree for a pair";
				return false;
			}
		}
		results[k] = value;
	}
	for (size_t word = static_cast<size_t>(kLogicalLanes) * stride; word < result.size(); ++word)
	{
		if (result[word] != kSentinel)
		{
			*error = "output canary was overwritten";
			return false;
		}
	}
	return true;
}

// The device name is restricted to the printable set the protocol validator accepts.
std::string SanitizeDeviceName(const std::string& name)
{
	std::string clean;
	for (const char c: name)
	{
		const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
		                     c == '.' || c == '_' || c == '(' || c == ')' || c == '/' || c == '+' || c == '-';
		clean += allowed ? c : '_';
		if (clean.size() == 128)
		{
			break;
		}
	}
	return clean.empty() ? std::string("unknown") : clean;
}

// Exclusive creation: an existing file, a symbolic link or a FIFO at the path makes the open fail.
bool WriteExclusive(const std::string& path, const void* data, size_t size)
{
	std::FILE* file = std::fopen(path.c_str(), "wbx");
	if (file == nullptr)
	{
		return false;
	}
	const bool written = std::fwrite(data, 1, size, file) == size;
	return std::fclose(file) == 0 && written;
}

bool InitializeConfig()
{
	char program[] = "kyty_shader_arithmetic_oracle";
	char* argv[] = {program, nullptr};
	Kyty::Core::SubsystemsList* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(1, argv);
	using Kyty::Config::ConfigSubsystem;
	using Kyty::Core::CoreSubsystem;
	using Kyty::Core::ThreadsSubsystem;
	using Kyty::Log::LogSubsystem;
	using Kyty::Math::MathSubsystem;
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(MathSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(ThreadsSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(LogSubsystem::Instance(),
	                {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
	if (!subsystems->InitAll(false))
	{
		return false;
	}
	Kyty::Config::SetNextGen(true);

	class ValidationConfig final: public Kyty::Config::ConfigSource
	{
	public:
		bool Has(const Kyty::Core::String& key) const override
		{
			return key == U"ShaderValidationEnabled" || key == U"ShaderOptimizationType";
		}
		int64_t GetInteger(const Kyty::Core::String&) const override { return 0; }
		bool GetBool(const Kyty::Core::String&) const override { return true; }
		Kyty::Core::String GetString(const Kyty::Core::String&) const override { return {}; }
	} validation;
	Kyty::Config::Load(validation);
	return true;
}

int ExecuteRequest(const Arguments& args)
{
	if (args.wave_width != 64)
	{
		std::fprintf(stderr, "logical wave width %u is not supported by this endpoint; only 64 is\n", args.wave_width);
		return kExitUnavailable;
	}
	if (args.count == 0 || args.count > kMaxPairs || args.max_dispatches == 0 || args.max_dispatches > kMaxDispatches)
	{
		std::fprintf(stderr, "pair count must be 1..%u and dispatch cap 1..%u\n", kMaxPairs, kMaxDispatches);
		return kExitUsage;
	}
	const uint32_t dispatches = (args.count + kBatchPairs - 1u) / kBatchPairs;
	if (dispatches > args.max_dispatches)
	{
		std::fprintf(stderr, "%u pairs need %u dispatches, above the cap of %u\n", args.count, dispatches, args.max_dispatches);
		return kExitUsage;
	}
	if (!IsAbsolute(args.inputs) || !IsAbsolute(args.output) || !IsAbsolute(args.meta))
	{
		std::fprintf(stderr, "input, output and meta paths must be absolute\n");
		return kExitUsage;
	}
	std::error_code ec;
	if (std::filesystem::exists(args.output, ec) || std::filesystem::exists(args.meta, ec) || ec)
	{
		std::fprintf(stderr, "output and meta files must not already exist\n");
		return kExitUsage;
	}

	std::vector<uint32_t> a;
	std::vector<uint32_t> b;
	int                   read_exit = kExitFailure;
	if (!ReadInputPairs(args.inputs, args.count, &a, &b, &read_exit))
	{
		return read_exit;
	}

	if (!InitializeConfig())
	{
		std::fprintf(stderr, "configuration subsystems failed to initialize\n");
		return kExitFailure;
	}
	VulkanComputeProbe probe;
	std::string        message;
	const auto         initialized = probe.InitializeWave(&message);
	if (initialized == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "paired-wave compute capability unavailable: %s\n", message.c_str());
		return kExitUnavailable;
	}
	if (initialized != VulkanComputeProbe::Result::Success)
	{
		std::fprintf(stderr, "paired-wave compute initialization failed: %s\n", message.c_str());
		return kExitFailure;
	}

	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	ShaderComputeInputInfo         input {};
	input.threads_num[0]  = 64u;
	input.threads_num[1]  = input.threads_num[2] = 1u;
	input.thread_ids_num  = 1;
	if (Kyty::Libs::Graphics::ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		std::fprintf(stderr, "paired64 layout is not supported by the selected device\n");
		return kExitUnavailable;
	}

	std::vector<uint32_t> results(args.count);
	for (uint32_t batch = 0; batch < dispatches; ++batch)
	{
		const uint32_t first = batch * kBatchPairs;
		const uint32_t n     = std::min(kBatchPairs, args.count - first);
		std::string    batch_error;
		if (!RunBatch(probe, input, args.op, a.data() + first, b.data() + first, n, results.data() + first, &batch_error))
		{
			std::fprintf(stderr, "batch %u failed: %s\n", batch, batch_error.c_str());
			return kExitFailure;
		}
	}

	std::vector<uint8_t> bytes(results.size() * 4u);
	for (size_t k = 0; k < results.size(); ++k)
	{
		for (uint32_t byte = 0; byte < 4; ++byte)
		{
			bytes[k * 4u + byte] = static_cast<uint8_t>((results[k] >> (8u * byte)) & 0xffu);
		}
	}
	if (!WriteExclusive(args.output, bytes.data(), bytes.size()))
	{
		std::fprintf(stderr, "output could not be created exclusively\n");
		return kExitFailure;
	}
	const std::string meta = std::string("{\"schema\":\"kyty_shader_integer_endpoint_v1\",\"backend\":\"vulkan\",\"op\":\"") +
	                         OpName(args.op) + "\",\"pairs\":" + std::to_string(args.count) +
	                         ",\"dispatches\":" + std::to_string(dispatches) + ",\"physical_device\":\"" +
	                         SanitizeDeviceName(probe.PhysicalDeviceName()) + "\",\"subgroup_size\":" +
	                         std::to_string(kSubgroupSize) + ",\"measured\":true,\"default_subgroup_size\":" +
	                         std::to_string(probe.DefaultSubgroupSize()) + "}\n";
	if (!WriteExclusive(args.meta, meta.data(), meta.size()))
	{
		std::remove(args.output.c_str());
		std::fprintf(stderr, "meta could not be created exclusively; output removed\n");
		return kExitFailure;
	}
	std::printf("measured pairs=%u dispatches=%u\n", args.count, dispatches);
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	Arguments args;
	if (!ParseArguments(argc, argv, &args))
	{
		std::fprintf(stderr,
		             "usage: kyty_shader_arithmetic_oracle --op {add|mul|mulhi|shl|shr} --count N --inputs FILE "
		             "--output FILE --meta FILE --wave-width 64 --max-dispatches N\n");
		return kExitUsage;
	}
	return ExecuteRequest(args);
}
