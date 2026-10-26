#include "Kyty/Agent/Runner/HardwareOracle.h"

#include "Kyty/Agent/Runner/Md5.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Kyty::Agent::Runner {
namespace {

constexpr uint32_t kEdgeValues[] = {0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
constexpr const char* kMnemonics[] = {"v_add_nc_u32", "v_mul_lo_u32", "v_mul_hi_u32", "v_lshlrev_b32", "v_lshrrev_b32"};

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

} // namespace

std::vector<uint32_t> OracleHostReference(const std::string& op, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
	std::vector<uint32_t> out(std::min(a.size(), b.size()));
	for (size_t i = 0; i < out.size(); i++)
	{
		const uint64_t x = a[i];
		const uint64_t y = b[i];
		if (op == "add")
		{
			out[i] = static_cast<uint32_t>(x + y);
		} else if (op == "mul")
		{
			out[i] = static_cast<uint32_t>(x * y);
		} else if (op == "mulhi")
		{
			out[i] = static_cast<uint32_t>((x * y) >> 32u);
		} else if (op == "shl")
		{
			out[i] = static_cast<uint32_t>(x << (y & 31u));
		} else
		{
			out[i] = static_cast<uint32_t>(x >> (y & 31u));
		}
	}
	return out;
}

void MakeOracleInputs(const std::string& mode, int64_t count, uint32_t seed, std::vector<uint32_t>* a, std::vector<uint32_t>* b)
{
	a->assign(count, 0);
	b->assign(count, 0);
	if (mode == "edges")
	{
		for (int64_t i = 0; i < count; i++)
		{
			const int64_t pair = i % 25;
			(*a)[i]            = kEdgeValues[pair / 5];
			(*b)[i]            = kEdgeValues[pair % 5];
		}
		return;
	}
	uint64_t state = seed;
	for (int64_t i = 0; i < 2 * count; i++)
	{
		state                                  = state * 6364136223846793005ull + 1442695040888963407ull;
		(i < count ? (*a)[i] : (*b)[i - count]) = static_cast<uint32_t>(state >> 32u);
	}
}

std::vector<std::string> MissingOracleMnemonics(const std::string& assembly)
{
	std::vector<std::string> missing;
	for (const char* mnemonic: kMnemonics)
	{
		const size_t length = std::strlen(mnemonic);
		bool         found  = false;
		// A line start, any whitespace, the mnemonic, then one whitespace character.
		for (size_t line = 0; !found;)
		{
			size_t pos = line;
			while (pos < assembly.size() && IsSpace(assembly[pos]))
			{
				pos++;
			}
			found = assembly.compare(pos, length, mnemonic) == 0 && pos + length < assembly.size() && IsSpace(assembly[pos + length]);
			const size_t next = assembly.find('\n', line);
			if (next == std::string::npos)
			{
				break;
			}
			line = next + 1;
		}
		if (!found)
		{
			missing.emplace_back(mnemonic);
		}
	}
	return missing;
}

#if !defined(_WIN32)

namespace {

namespace fs = std::filesystem;

constexpr const char* kSchema         = "kyty_hardware_oracle_v2";
constexpr const char* kProbeSchema    = "kyty_hardware_oracle_probe_v1";
constexpr const char* kEndpointSchema = "kyty_shader_integer_endpoint_v1";
constexpr const char* kSupportedArch  = "gfx1030";
constexpr const char* kOutputsName    = "raw/outputs.bin";

constexpr int64_t kMaxCount               = int64_t {1} << 16;
constexpr int64_t kMaxSeed                = (int64_t {1} << 32) - 1;
constexpr size_t  kMaxMetaBytes           = 4096;
constexpr size_t  kMaxVersionBytes        = 8192;
constexpr int64_t kMaxAsmBytes            = int64_t {1} << 20;
constexpr int64_t kMaxEndpointDispatches  = 64;
constexpr int64_t kVulkanMaxPairs         = 1024; // 16 pairs per dispatch x 64 dispatches
constexpr int64_t kVulkanPairsPerDispatch = 16;
constexpr int64_t kVulkanLogicalWave      = 64; // the endpoint runs the paired logical wave64 only
constexpr int     kEndpointUnavailable    = 77; // pre-dispatch unavailability
constexpr int     kEndpointRefused        = 2;  // usage or bound violation
constexpr double  kCompileTimeoutS        = 120.0;
constexpr double  kRunTimeoutS            = 60.0;
constexpr double  kEndpointTimeoutS       = 120.0;

constexpr int kExitOk          = 0;
constexpr int kExitMismatch    = 1;
constexpr int kExitUsage       = 2;
constexpr int kExitUnavailable = 3;
constexpr int kExitError       = 4;

const std::vector<std::string>& Ops()
{
	static const std::vector<std::string> ops = {"add", "mul", "mulhi", "shl", "shr"};
	return ops;
}

const std::set<int64_t>& SubgroupSizes()
{
	static const std::set<int64_t> sizes = {1, 2, 4, 8, 16, 32, 64, 128};
	return sizes;
}

const char* const kHipSource = R"HIP(// Integer instruction oracle for gfx1030. Each arithmetic result is produced by one
// AMDGCN instruction written as inline assembly. There is no C++ arithmetic fallback.
// Usage: oracle <op-index> <count> <inputs.bin> <outputs.bin> <meta.json>
// inputs.bin holds count little-endian uint32 operands a, then count operands b.
#if !defined(__gfx1030__)
#error "this oracle supports gfx1030 only"
#endif
#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t BLOCK_SIZE = 256;
constexpr uint32_t MAX_COUNT = 1u << 16;
constexpr uint32_t OP_SHR = 4;

// Operand order is published per instruction: D = src0 op src1. For shifts, src0 is the
// shift count (y) and src1 is the value (x), so D = x << (y & 31) and D = x >> (y & 31).
__global__ void ArithmeticOracle(const uint32_t* a, const uint32_t* b, uint32_t* out, uint32_t count, uint32_t op)
{
	const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i >= count) {
		return;
	}
	const uint32_t x = a[i];
	const uint32_t y = b[i];
	uint32_t       r = 0;
	switch (op) {
		case 0: asm volatile("v_add_nc_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 1: asm volatile("v_mul_lo_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 2: asm volatile("v_mul_hi_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 3: asm volatile("v_lshlrev_b32 %0, %1, %2" : "=v"(r) : "v"(y), "v"(x)); break;
		default: asm volatile("v_lshrrev_b32 %0, %1, %2" : "=v"(r) : "v"(y), "v"(x)); break;
	}
	out[i] = r;
}

} // namespace

#define HIP_CHECK(expr)                                                        \
	do {                                                                       \
		const hipError_t status_ = (expr);                                     \
		if (status_ != hipSuccess) {                                           \
			std::fprintf(stderr, "hip error %d at line %d\n", static_cast<int>(status_), __LINE__); \
			return 4;                                                          \
		}                                                                      \
	} while (0)

int main(int argc, char** argv)
{
	if (argc != 6) {
		return 2;
	}
	const long op = std::strtol(argv[1], nullptr, 10);
	const long count_arg = std::strtol(argv[2], nullptr, 10);
	if (op < 0 || op > static_cast<long>(OP_SHR) || count_arg < 1 || count_arg > static_cast<long>(MAX_COUNT)) {
		return 2;
	}
	const uint32_t count = static_cast<uint32_t>(count_arg);

	std::vector<uint32_t> host(2u * count);
	std::FILE* input = std::fopen(argv[3], "rb");
	if (input == nullptr) {
		return 2;
	}
	const size_t read = std::fread(host.data(), sizeof(uint32_t), host.size(), input);
	std::fclose(input);
	if (read != host.size()) {
		return 2;
	}

	hipDeviceProp_t props {};
	HIP_CHECK(hipGetDeviceProperties(&props, 0));
	int runtime_version = 0;
	int driver_version = 0;
	HIP_CHECK(hipRuntimeGetVersion(&runtime_version));
	HIP_CHECK(hipDriverGetVersion(&driver_version));
	const size_t bytes = sizeof(uint32_t) * count;
	uint32_t*    a     = nullptr;
	uint32_t*    b     = nullptr;
	uint32_t*    out   = nullptr;
	HIP_CHECK(hipMalloc(&a, bytes));
	HIP_CHECK(hipMalloc(&b, bytes));
	HIP_CHECK(hipMalloc(&out, bytes));
	HIP_CHECK(hipMemcpy(a, host.data(), bytes, hipMemcpyHostToDevice));
	HIP_CHECK(hipMemcpy(b, host.data() + count, bytes, hipMemcpyHostToDevice));

	const uint32_t blocks = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;
	hipLaunchKernelGGL(ArithmeticOracle, dim3(blocks), dim3(BLOCK_SIZE), 0, 0, a, b, out, count, static_cast<uint32_t>(op));
	HIP_CHECK(hipGetLastError());
	HIP_CHECK(hipDeviceSynchronize());

	std::vector<uint32_t> result(count);
	HIP_CHECK(hipMemcpy(result.data(), out, bytes, hipMemcpyDeviceToHost));
	HIP_CHECK(hipFree(a));
	HIP_CHECK(hipFree(b));
	HIP_CHECK(hipFree(out));

	std::FILE* output = std::fopen(argv[4], "wb");
	if (output == nullptr || std::fwrite(result.data(), sizeof(uint32_t), result.size(), output) != result.size()) {
		return 3;
	}
	std::fclose(output);
	std::FILE* meta = std::fopen(argv[5], "wb");
	if (meta == nullptr) {
		return 3;
	}
	std::fprintf(meta,
	             "{\"warp_size\": %d, \"device_name\": \"%s\", \"gcn_arch_name\": \"%s\", "
	             "\"runtime_version\": %d, \"driver_version\": %d}\n",
	             props.warpSize, props.name, props.gcnArchName, runtime_version, driver_version);
	std::fclose(meta);
	return 0;
}
)HIP";

// A refusal (exit 2) or an output error (exit 4) that ends the command.
struct Failure
{
	int         exit_code = kExitUsage;
	std::string message;
};

bool Refuse(Failure* failure, std::string message)
{
	failure->exit_code = kExitUsage;
	failure->message   = std::move(message);
	return false;
}

bool OutputError(Failure* failure, const std::string& detail)
{
	failure->exit_code = kExitError;
	failure->message   = "cannot prepare or read output: " + detail;
	return false;
}

std::string LeafName(const std::string& path)
{
	return fs::path(path).filename().string();
}

bool AllOf(const std::string& text, size_t min, size_t max, bool (*allowed)(char))
{
	return text.size() >= min && text.size() <= max && std::all_of(text.begin(), text.end(), allowed);
}

bool IsDeviceNameChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::strchr(" ._()/+-", c) != nullptr;
}

bool IsGcnFeatureChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-';
}

bool IsLowerHex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool IsPrintableAscii(char c)
{
	return c >= ' ' && c <= '~';
}

bool IsDeviceName(const Json& value)
{
	return value.IsString() && value.AsString().find('\0') == std::string::npos && AllOf(value.AsString(), 1, 128, IsDeviceNameChar);
}

// gfx + 3..6 lowercase hex digits, then any number of ":feature" suffixes.
bool IsGcnName(const Json& value)
{
	if (!value.IsString() || value.AsString().rfind("gfx", 0) != 0)
	{
		return false;
	}
	const std::string rest  = value.AsString().substr(3);
	const size_t      colon = rest.find(':');
	if (!AllOf(rest.substr(0, colon), 3, 6, IsLowerHex))
	{
		return false;
	}
	for (size_t start = colon; start != std::string::npos;)
	{
		const size_t next = rest.find(':', start + 1);
		if (!AllOf(rest.substr(start + 1, next == std::string::npos ? std::string::npos : next - start - 1), 1, 16, IsGcnFeatureChar))
		{
			return false;
		}
		start = next;
	}
	return true;
}

bool IsInputMd5(const Json& value)
{
	return value.IsString() && AllOf(value.AsString(), 32, 32, IsLowerHex);
}

bool IsIntegerIn(const Json& value, int64_t min, int64_t max)
{
	return value.IsInteger() && value.AsNumber() >= static_cast<double>(min) && value.AsNumber() <= static_cast<double>(max);
}

bool IsKnownOp(const Json& value)
{
	return value.IsString() && std::find(Ops().begin(), Ops().end(), value.AsString()) != Ops().end();
}

bool HasExactKeys(const Json& object, const std::set<std::string>& keys)
{
	if (!object.IsObject() || object.Size() != keys.size())
	{
		return false;
	}
	return std::all_of(keys.begin(), keys.end(), [&](const std::string& key) { return object.Has(key); });
}

bool IsUtf8(const std::string& text)
{
	for (size_t i = 0; i < text.size();)
	{
		const auto lead = static_cast<unsigned char>(text[i]);
		size_t     extra = 0;
		uint32_t   code  = 0;
		if (lead < 0x80u)
		{
			i++;
			continue;
		}
		if (lead >= 0xC2u && lead <= 0xDFu)
		{
			extra = 1;
			code  = lead & 0x1Fu;
		} else if (lead >= 0xE0u && lead <= 0xEFu)
		{
			extra = 2;
			code  = lead & 0x0Fu;
		} else if (lead >= 0xF0u && lead <= 0xF4u)
		{
			extra = 3;
			code  = lead & 0x07u;
		} else
		{
			return false;
		}
		if (i + extra >= text.size())
		{
			return false;
		}
		for (size_t k = 1; k <= extra; k++)
		{
			const auto next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xC0u) != 0x80u)
			{
				return false;
			}
			code = (code << 6u) | (next & 0x3Fu);
		}
		const uint32_t min_code = extra == 1 ? 0x80u : (extra == 2 ? 0x800u : 0x10000u);
		if (code < min_code || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
		{
			return false;
		}
		i += extra + 1;
	}
	return true;
}

// Opens without following a final symbolic link and without blocking on a FIFO.
// The type is checked on the open descriptor, so a path swapped after a check
// still cannot be a FIFO, device or directory.
bool OpenRegular(const std::string& path, int* fd, int64_t* size, Failure* failure)
{
	*fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (*fd < 0)
	{
		return Refuse(failure, "cannot open " + LeafName(path) + ": " + std::strerror(errno));
	}
	struct stat status {};
	if (::fstat(*fd, &status) != 0 || !S_ISREG(status.st_mode))
	{
		::close(*fd);
		return Refuse(failure, LeafName(path) + " must be a regular file");
	}
	*size = status.st_size;
	return true;
}

// At most limit + 1 bytes, so the caller can compare the exact size.
bool ReadAtMost(int fd, size_t limit, std::string* data, const std::string& path, Failure* failure)
{
	data->clear();
	char buffer[65536];
	while (data->size() <= limit)
	{
		const ssize_t got = ::read(fd, buffer, std::min(sizeof(buffer), limit + 1 - data->size()));
		if (got < 0)
		{
			::close(fd);
			return OutputError(failure, LeafName(path) + ": " + std::strerror(errno));
		}
		if (got == 0)
		{
			break;
		}
		data->append(buffer, static_cast<size_t>(got));
	}
	::close(fd);
	return true;
}

// A regular file of at most `limit` bytes; a larger one is refused before it is read.
bool ReadRegularFile(const std::string& path, size_t limit, std::string* data, Failure* failure)
{
	int     fd   = -1;
	int64_t size = 0;
	if (!OpenRegular(path, &fd, &size, failure))
	{
		return false;
	}
	if (size > static_cast<int64_t>(limit))
	{
		::close(fd);
		return Refuse(failure, LeafName(path) + " exceeds the " + std::to_string(limit) + "-byte limit");
	}
	return ReadAtMost(fd, limit, data, path, failure);
}

bool ReadOutputBytes(const std::string& path, size_t limit, std::string* data, Failure* failure)
{
	int     fd   = -1;
	int64_t size = 0;
	return OpenRegular(path, &fd, &size, failure) && ReadAtMost(fd, limit, data, path, failure);
}

bool ReadBoundedJson(const std::string& path, Json* document, Failure* failure)
{
	std::string text;
	if (!ReadRegularFile(path, kMaxMetaBytes, &text, failure))
	{
		return false;
	}
	if (!IsUtf8(text))
	{
		return Refuse(failure, "metadata is not UTF-8 text");
	}
	std::string parse_error;
	if (!ParseJson(text, document, &parse_error))
	{
		return Refuse(failure, LeafName(path) + " is not valid JSON: " + parse_error);
	}
	return true;
}

std::vector<uint32_t> UnpackWords(const std::string& bytes, size_t first, size_t count)
{
	std::vector<uint32_t> words(count);
	for (size_t i = 0; i < count; i++)
	{
		const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + 4 * (first + i);
		words[i]      = p[0] | (uint32_t {p[1]} << 8u) | (uint32_t {p[2]} << 16u) | (uint32_t {p[3]} << 24u);
	}
	return words;
}

void PackWords(const std::vector<uint32_t>& words, std::string* bytes)
{
	for (const uint32_t word: words)
	{
		for (uint32_t shift = 0; shift < 32; shift += 8)
		{
			bytes->push_back(static_cast<char>((word >> shift) & 0xFFu));
		}
	}
}

struct Request
{
	std::string                op;
	std::string                mode; // "external" for an input file
	int64_t                    wave_width = 0;
	std::optional<int64_t>     count;    // absent for an input file
	int64_t                    seed = 1;
	std::string                arch;
	std::string                out_dir; // resolved
	std::optional<std::string> input_file;
};

struct Inputs
{
	int64_t               count = 0;
	std::vector<uint32_t> a;
	std::vector<uint32_t> b;
	std::string           data;
	std::string           md5;
};

// External files are used verbatim; their size bound is checked before reading.
bool BuildInputs(const Request& request, int64_t max_pairs, Inputs* inputs, Failure* failure)
{
	if (request.input_file.has_value())
	{
		if (!ReadRegularFile(*request.input_file, static_cast<size_t>(8 * max_pairs), &inputs->data, failure))
		{
			return false;
		}
		if (inputs->data.empty() || inputs->data.size() % 8 != 0)
		{
			return Refuse(failure, "external input must hold a non-zero multiple of 8 bytes (uint32 pairs)");
		}
		inputs->count = static_cast<int64_t>(inputs->data.size() / 8);
		inputs->a     = UnpackWords(inputs->data, 0, inputs->count);
		inputs->b     = UnpackWords(inputs->data, inputs->count, inputs->count);
	} else
	{
		inputs->count = *request.count;
		MakeOracleInputs(request.mode, inputs->count, static_cast<uint32_t>(request.seed), &inputs->a, &inputs->b);
		inputs->data.clear();
		PackWords(inputs->a, &inputs->data);
		PackWords(inputs->b, &inputs->data);
	}
	inputs->md5 = Md5Hex(inputs->data.data(), inputs->data.size());
	return true;
}

Json CompareOutputs(const std::vector<uint32_t>& expected, const std::vector<uint32_t>& actual)
{
	Json result = Json::MakeObject().Set("mismatch_count", 0).Set("first_index", Json()).Set("expected", Json()).Set("actual", Json());
	if (expected.size() != actual.size())
	{
		return result.Set("status", "length_mismatch");
	}
	int64_t                mismatches = 0;
	std::optional<size_t> first;
	for (size_t i = 0; i < expected.size(); i++)
	{
		if (expected[i] != actual[i])
		{
			mismatches++;
			first = first.has_value() ? first : std::optional<size_t>(i);
		}
	}
	if (!first.has_value())
	{
		return result.Set("status", "match");
	}
	return result.Set("status", "mismatch")
	    .Set("mismatch_count", mismatches)
	    .Set("first_index", static_cast<int64_t>(*first))
	    .Set("expected", static_cast<int64_t>(expected[*first]))
	    .Set("actual", static_cast<int64_t>(actual[*first]));
}

// Strict schema for the device metadata written by the kernel.
bool ValidDeviceMetadata(const Json& meta)
{
	return HasExactKeys(meta, {"warp_size", "device_name", "gcn_arch_name", "runtime_version", "driver_version"}) &&
	       meta["warp_size"].IsInteger() && (meta["warp_size"].AsInt() == 32 || meta["warp_size"].AsInt() == 64) &&
	       IsDeviceName(meta["device_name"]) && IsGcnName(meta["gcn_arch_name"]) &&
	       IsIntegerIn(meta["runtime_version"], 0, (int64_t {1} << 31) - 1) && IsIntegerIn(meta["driver_version"], 0, (int64_t {1} << 31) - 1);
}

bool IsSubgroupSize(const Json& value)
{
	return value.IsInteger() && SubgroupSizes().count(value.AsInt()) != 0;
}

// Strict schema for the Vulkan endpoint's own report. Measured only when it says so.
bool ValidEndpointMeta(const Json& meta, const std::string& op, int64_t count)
{
	static const std::set<std::string> required = {"schema", "backend", "op", "pairs", "dispatches", "physical_device", "subgroup_size", "measured"};
	if (!meta.IsObject())
	{
		return false;
	}
	for (const auto& [key, value]: meta.Members())
	{
		if (required.count(key) == 0 && key != "default_subgroup_size")
		{
			return false;
		}
	}
	const int64_t dispatches = (count + kVulkanPairsPerDispatch - 1) / kVulkanPairsPerDispatch;
	return std::all_of(required.begin(), required.end(), [&](const std::string& key) { return meta.Has(key); }) &&
	       meta["schema"] == Json(kEndpointSchema) && meta["backend"] == Json("vulkan") && meta["op"] == Json(op) &&
	       meta["pairs"] == Json(count) && meta["dispatches"].IsInteger() && meta["dispatches"].AsInt() == dispatches &&
	       dispatches <= kMaxEndpointDispatches && IsSubgroupSize(meta["subgroup_size"]) && IsDeviceName(meta["physical_device"]) &&
	       meta["measured"].IsBool() && (!meta.Has("default_subgroup_size") || IsSubgroupSize(meta["default_subgroup_size"]));
}

// First non-empty line of `hipcc --version`, restricted to printable ASCII.
bool ReadCompilerVersion(const std::string& path, std::string* version)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		return false;
	}
	std::string text(kMaxVersionBytes, '\0');
	text.resize(std::fread(text.data(), 1, text.size(), file));
	std::fclose(file);
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t      end  = text.find_first_of("\n\r\v\f\x1c\x1d\x1e", start);
		const std::string line = Strip(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (!line.empty())
		{
			*version = line;
			return AllOf(line, 1, 200, IsPrintableAscii);
		}
		if (end == std::string::npos)
		{
			break;
		}
		start = end + 1;
	}
	return false;
}

bool IsExecutableFile(const std::string& path)
{
	struct stat status {};
	return ::stat(path.c_str(), &status) == 0 && !S_ISDIR(status.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

// The executable PATH would run for a bare `name`; empty when there is none.
std::string FindOnPath(const std::string& name)
{
	const char*       env  = std::getenv("PATH");
	const std::string path = env != nullptr ? env : "/bin:/usr/bin";
	for (size_t start = 0; start <= path.size();)
	{
		const size_t      end       = path.find(':', start);
		const std::string directory = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const std::string candidate = directory.empty() ? name : directory + "/" + name;
		if (IsExecutableFile(candidate))
		{
			return candidate;
		}
		if (end == std::string::npos)
		{
			break;
		}
		start = end + 1;
	}
	return {};
}

// Presence only: the device node is accessible and the HIP compiler resolves.
Json Probe(const OracleTools& tools)
{
	const bool kfd_ok      = ::access(tools.kfd_path.c_str(), R_OK | W_OK) == 0;
	const bool compiler_ok = !tools.compiler_path.empty();
	return Json::MakeObject()
	    .Set("schema", kProbeSchema)
	    .Set("status", kfd_ok && compiler_ok ? "present" : "unavailable")
	    .Set("checks", Json::MakeObject().Set("kfd_device", kfd_ok).Set("hip_compiler", compiler_ok))
	    .Set("measurement", "not_run");
}

bool IsInside(const fs::path& path, const fs::path& root)
{
	auto root_it = root.begin();
	auto path_it = path.begin();
	for (; root_it != root.end(); ++root_it, ++path_it)
	{
		if (path_it == path.end() || *path_it != *root_it)
		{
			return false;
		}
	}
	return true;
}

struct RequestArgs
{
	std::string                op;
	std::optional<std::string> mode;
	int64_t                    wave_width = 0;
	std::optional<int64_t>     count;
	int64_t                    seed = 1;
	std::string                arch = kSupportedArch;
	std::string                out_dir;
	std::optional<std::string> input_file;
};

bool ValidateRequest(const RequestArgs& args, const std::string& repo_root, Request* request, Failure* failure)
{
	if (std::find(Ops().begin(), Ops().end(), args.op) == Ops().end())
	{
		return Refuse(failure, "operation must be one of add, mul, mulhi, shl, shr");
	}
	if (!args.input_file.has_value() && args.mode != "seeded" && args.mode != "edges")
	{
		return Refuse(failure, "mode must be one of seeded, edges");
	}
	if (args.input_file.has_value() && args.mode.has_value())
	{
		return Refuse(failure, "choose either a generated mode or an external input file, not both");
	}
	if (args.wave_width != 32 && args.wave_width != 64)
	{
		return Refuse(failure, "wave width must be 32 or 64");
	}
	if (args.input_file.has_value() && args.count.has_value())
	{
		return Refuse(failure, "--count cannot be combined with --input-file: the file defines the pair count");
	}
	if (!args.input_file.has_value() && (!args.count.has_value() || *args.count < 1 || *args.count > kMaxCount))
	{
		return Refuse(failure, "count must be in [1, " + std::to_string(kMaxCount) + "]");
	}
	if (args.seed < 0 || args.seed > kMaxSeed)
	{
		return Refuse(failure, "seed must be in [0, " + std::to_string(kMaxSeed) + "]");
	}
	if (args.arch != kSupportedArch)
	{
		return Refuse(failure, "unsupported architecture '" + args.arch + "': this oracle supports gfx1030 only");
	}
	if (!fs::path(args.out_dir).is_absolute())
	{
		return Refuse(failure, "output directory must be absolute");
	}
	std::error_code ec;
	const fs::path  resolved = fs::weakly_canonical(args.out_dir, ec);
	const fs::path  repo     = fs::weakly_canonical(repo_root, ec);
	if (IsInside(resolved, repo))
	{
		return Refuse(failure, "output directory must be outside the repository");
	}
	if (args.input_file.has_value() && !fs::path(*args.input_file).is_absolute())
	{
		return Refuse(failure, "input file must be an absolute path");
	}
	request->op         = args.op;
	request->mode       = args.input_file.has_value() ? "external" : *args.mode;
	request->wave_width = args.wave_width;
	request->count      = args.input_file.has_value() ? std::nullopt : args.count;
	request->seed       = args.seed;
	request->arch       = args.arch;
	request->out_dir    = resolved.string();
	request->input_file = args.input_file;
	return true;
}

// The output directory must be new or empty, so no earlier result is overwritten.
bool PrepareFreshDir(const std::string& out_dir, Failure* failure)
{
	struct stat status {};
	if (::stat(fs::path(out_dir).parent_path().c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
	{
		return Refuse(failure, "output directory parent does not exist");
	}
	if (::lstat(out_dir.c_str(), &status) == 0 && S_ISLNK(status.st_mode))
	{
		return Refuse(failure, "output directory must not be a symbolic link");
	}
	if (::stat(out_dir.c_str(), &status) == 0)
	{
		std::error_code ec;
		if (!S_ISDIR(status.st_mode) || !fs::is_empty(out_dir, ec) || ec)
		{
			return Refuse(failure, "output directory must be new or empty");
		}
		return true;
	}
	if (::mkdir(out_dir.c_str(), 0700) != 0)
	{
		return OutputError(failure, std::strerror(errno));
	}
	return true;
}

bool MakeDir(const std::string& path, Failure* failure)
{
	return ::mkdir(path.c_str(), 0700) == 0 || OutputError(failure, LeafName(path) + ": " + std::strerror(errno));
}

bool WriteFile(const std::string& path, const std::string& bytes, Failure* failure)
{
	std::string error;
	return WriteTextFile(path, bytes, &error) || OutputError(failure, error);
}

bool WriteExclusive(const std::string& path, const std::string& text, Failure* failure)
{
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
	{
		return OutputError(failure, LeafName(path) + ": " + std::strerror(errno));
	}
	const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
	return (::close(fd) == 0 && written) || OutputError(failure, "write failed: " + LeafName(path));
}

enum class CommandStatus
{
	Exited,
	TimedOut,
};

// Runs a fixed argument list without a shell; stdout and stderr go to `log_path`.
bool RunCommand(RunnerHost* host, const std::vector<std::string>& argv, double timeout_s, const std::string& log_path,
                CommandStatus* status, int* exit_code, Failure* failure)
{
	std::error_code ec;
	LaunchSpec      spec {argv, fs::current_path(ec).string(), CurrentEnvironment(), log_path};
	std::string     error;
	if (!host->Launch(spec, &error))
	{
		return OutputError(failure, error);
	}
	const double deadline = host->Now() + timeout_s;
	while (true)
	{
		if (const auto code = host->PollChild(); code.has_value())
		{
			*status    = CommandStatus::Exited;
			*exit_code = *code;
			return true;
		}
		if (host->Now() >= deadline)
		{
			std::vector<std::string> notes;
			host->TerminateChild(&notes);
			*status = CommandStatus::TimedOut;
			return true;
		}
		host->Sleep(0.05);
	}
}

struct Outcome
{
	Json summary;
	int  exit_code = kExitOk;
};

Outcome Summary(const Request& request, const char* backend, const char* status, int exit_code)
{
	Json summary = Json::MakeObject()
	                   .Set("schema", kSchema)
	                   .Set("backend", backend)
	                   .Set("status", status)
	                   .Set("measured", false)
	                   .Set("op", request.op)
	                   .Set("mode", request.mode)
	                   .Set("wave_width", request.wave_width)
	                   .Set("count", request.count.has_value() ? Json(*request.count) : Json())
	                   .Set("seed", request.seed)
	                   .Set("arch", request.arch);
	return {std::move(summary), exit_code};
}

// `status` is "measured" only for a regular file of exactly 4 * count bytes. A
// read error after a successful open ends the command.
bool LoadOutputs(const std::string& path, int64_t count, std::vector<uint32_t>* values, std::string* status, Failure* failure)
{
	std::string bytes;
	Failure     open_failure;
	if (!ReadOutputBytes(path, static_cast<size_t>(4 * count), &bytes, &open_failure))
	{
		if (open_failure.exit_code != kExitUsage)
		{
			*failure = open_failure;
			return false;
		}
		*status = "output_invalid";
		return true;
	}
	if (bytes.size() != static_cast<size_t>(4 * count))
	{
		*status = "output_size_mismatch";
		return true;
	}
	*status = "measured";
	*values = UnpackWords(bytes, 0, count);
	return true;
}

// The host reference is ancillary evidence; it does not decide any comparison.
Outcome MeasuredOutcome(const Request& request, const char* backend, const Inputs& inputs, const std::vector<uint32_t>& actual,
                        Json artifact)
{
	Outcome outcome = Summary(request, backend, "measured", kExitOk);
	outcome.summary.Set("measured", true)
	    .Set("artifact", std::move(artifact))
	    .Set("input_md5", inputs.md5)
	    .Set("outputs", kOutputsName)
	    .Set("host_reference", CompareOutputs(OracleHostReference(request.op, inputs.a, inputs.b), actual))
	    .Set("count", inputs.count);
	return outcome;
}

std::string Join(const std::string& directory, const char* leaf)
{
	return (fs::path(directory) / leaf).string();
}

std::vector<std::string> CompilerArgv(const OracleTools& tools, const Request& request)
{
	std::vector<std::string> argv = {tools.compiler_path, "-O2", "-std=c++17", "--offload-arch=" + request.arch};
	if (request.wave_width == 64)
	{
		argv.emplace_back("-mwavefrontsize64");
	}
	return argv;
}

// Compiles, checks the device assembly and runs the native kernel. Returns
// false only for a refusal or an output error; every measurement state is an outcome.
bool RunHip(RunnerHost* host, const OracleTools& tools, const Request& request, Outcome* outcome, Failure* failure)
{
	const Json capability = Probe(tools);
	if (capability["status"] != Json("present"))
	{
		*outcome = Summary(request, "hip", "unavailable", kExitUnavailable);
		outcome->summary.Set("probe", capability);
		return true;
	}
	const std::string build_dir = Join(request.out_dir, "build");
	const std::string raw_dir   = Join(request.out_dir, "raw");
	Inputs            inputs;
	if (!PrepareFreshDir(request.out_dir, failure) || !MakeDir(build_dir, failure) || !MakeDir(raw_dir, failure) ||
	    !BuildInputs(request, kMaxCount, &inputs, failure))
	{
		return false;
	}
	const std::string inputs_path = Join(raw_dir, "inputs.bin");
	const std::string source      = Join(build_dir, "oracle.hip");
	const std::string binary      = Join(build_dir, "oracle");
	const std::string asm_out     = Join(build_dir, "oracle.s");
	const std::string output_path = Join(raw_dir, "outputs.bin");
	const std::string meta_path   = Join(raw_dir, "meta.json");
	if (!WriteFile(inputs_path, inputs.data, failure) || !WriteFile(source, kHipSource, failure))
	{
		return false;
	}

	std::vector<std::string> asm_argv = CompilerArgv(tools, request);
	asm_argv.insert(asm_argv.end(), {"-S", "--cuda-device-only", "-o", asm_out, source});
	std::vector<std::string> compile_argv = CompilerArgv(tools, request);
	compile_argv.insert(compile_argv.end(), {"-o", binary, source});
	const std::vector<std::string> run_argv = {binary,
	                                           std::to_string(std::find(Ops().begin(), Ops().end(), request.op) - Ops().begin()),
	                                           std::to_string(inputs.count),
	                                           inputs_path,
	                                           output_path,
	                                           meta_path};

	CommandStatus status    = CommandStatus::Exited;
	int           exit_code = 0;
	const auto    timed_out = [&]() { return status == CommandStatus::TimedOut; };
	if (!RunCommand(host, asm_argv, kCompileTimeoutS, Join(build_dir, "asm.log"), &status, &exit_code, failure))
	{
		return false;
	}
	if (timed_out() || exit_code != 0)
	{
		*outcome = Summary(request, "hip", timed_out() ? "timeout" : "compile_failed", kExitMismatch);
		return true;
	}
	struct stat asm_status {};
	if (::stat(asm_out.c_str(), &asm_status) != 0)
	{
		return OutputError(failure, "oracle.s: " + std::string(std::strerror(errno)));
	}
	if (asm_status.st_size > kMaxAsmBytes)
	{
		*outcome = Summary(request, "hip", "asm_check_failed", kExitMismatch);
		outcome->summary.Set("reason", "assembly too large");
		return true;
	}
	std::string assembly;
	std::string read_error;
	if (!ReadTextFile(asm_out, static_cast<size_t>(kMaxAsmBytes), &assembly, &read_error))
	{
		return OutputError(failure, read_error);
	}
	if (const auto missing = MissingOracleMnemonics(assembly); !missing.empty())
	{
		Json names = Json::MakeArray();
		for (const auto& name: missing)
		{
			names.Push(name);
		}
		*outcome = Summary(request, "hip", "asm_check_failed", kExitMismatch);
		outcome->summary.Set("missing_mnemonics", std::move(names));
		return true;
	}
	if (!RunCommand(host, compile_argv, kCompileTimeoutS, Join(build_dir, "compile.log"), &status, &exit_code, failure))
	{
		return false;
	}
	if (timed_out() || exit_code != 0)
	{
		*outcome = Summary(request, "hip", timed_out() ? "timeout" : "compile_failed", kExitMismatch);
		return true;
	}
	const std::string version_log = Join(build_dir, "version.log");
	if (!RunCommand(host, {tools.compiler_path, "--version"}, kCompileTimeoutS, version_log, &status, &exit_code, failure))
	{
		return false;
	}
	if (timed_out())
	{
		*outcome = Summary(request, "hip", "timeout", kExitMismatch);
		return true;
	}
	std::string compiler_version;
	if (exit_code != 0 || !ReadCompilerVersion(version_log, &compiler_version))
	{
		*outcome = Summary(request, "hip", "compiler_version_unreadable", kExitMismatch);
		return true;
	}
	if (!RunCommand(host, run_argv, kRunTimeoutS, Join(raw_dir, "run.log"), &status, &exit_code, failure))
	{
		return false;
	}
	if (timed_out() || exit_code != 0)
	{
		*outcome = Summary(request, "hip", timed_out() ? "timeout" : "device_error", kExitMismatch);
		return true;
	}

	Json    device;
	Failure meta_failure;
	if (!ReadBoundedJson(meta_path, &device, &meta_failure) || !ValidDeviceMetadata(device))
	{
		*outcome = Summary(request, "hip", "device_metadata_invalid", kExitMismatch);
		return true;
	}
	Json artifact = device;
	artifact.Set("compiler_version", compiler_version).Set("supported_arch", kSupportedArch);
	const std::string& gcn_name = device["gcn_arch_name"].AsString();
	const char*        mismatch = device["warp_size"].AsInt() != request.wave_width     ? "wave_width_mismatch"
	                              : gcn_name.substr(0, gcn_name.find(':')) != request.arch ? "arch_mismatch"
	                                                                                       : nullptr;
	if (mismatch != nullptr)
	{
		*outcome = Summary(request, "hip", mismatch, kExitMismatch);
		outcome->summary.Set("artifact", std::move(artifact));
		return true;
	}
	std::vector<uint32_t> actual;
	std::string           output_status;
	if (!LoadOutputs(output_path, inputs.count, &actual, &output_status, failure))
	{
		return false;
	}
	if (output_status != "measured")
	{
		*outcome = Summary(request, "hip", output_status.c_str(), kExitMismatch);
		outcome->summary.Set("artifact", std::move(artifact));
		return true;
	}
	*outcome = MeasuredOutcome(request, "hip", inputs, actual, std::move(artifact));
	return WriteExclusive(Join(request.out_dir, "result.json"), outcome->summary.Dump(2) + "\n", failure);
}

// Admission (logical wave64, at most 1024 pairs) is checked before any file is
// read or created. A missing or non-executable endpoint is unavailable.
bool RunVulkan(RunnerHost* host, const Request& request, const std::string& endpoint, Outcome* outcome, Failure* failure)
{
	if (request.wave_width != kVulkanLogicalWave)
	{
		*outcome = Summary(request, "vulkan", "unavailable", kExitUnavailable);
		outcome->summary.Set("reason", "logical_wave32_unsupported");
		return true;
	}
	const std::string too_many = "the Vulkan endpoint accepts at most " + std::to_string(kVulkanMaxPairs) + " pairs per run";
	if (request.count.has_value() && *request.count > kVulkanMaxPairs)
	{
		return Refuse(failure, too_many);
	}
	Inputs inputs;
	if (!BuildInputs(request, kVulkanMaxPairs, &inputs, failure))
	{
		return false;
	}
	struct stat endpoint_status {};
	if (::stat(endpoint.c_str(), &endpoint_status) != 0 || !S_ISREG(endpoint_status.st_mode))
	{
		*outcome = Summary(request, "vulkan", "unavailable", kExitUnavailable);
		outcome->summary.Set("reason", "endpoint_not_found");
		return true;
	}
	if (::access(endpoint.c_str(), X_OK) != 0)
	{
		*outcome = Summary(request, "vulkan", "unavailable", kExitUnavailable);
		outcome->summary.Set("reason", "endpoint_not_executable");
		return true;
	}
	const std::string raw_dir     = Join(request.out_dir, "raw");
	const std::string inputs_path = Join(raw_dir, "inputs.bin");
	const std::string output_path = Join(raw_dir, "outputs.bin");
	const std::string meta_path   = Join(raw_dir, "endpoint-meta.json");
	if (!PrepareFreshDir(request.out_dir, failure) || !MakeDir(raw_dir, failure) || !WriteFile(inputs_path, inputs.data, failure))
	{
		return false;
	}
	const std::vector<std::string> argv = {endpoint,
	                                       "--op",
	                                       request.op,
	                                       "--count",
	                                       std::to_string(inputs.count),
	                                       "--inputs",
	                                       inputs_path,
	                                       "--output",
	                                       output_path,
	                                       "--meta",
	                                       meta_path,
	                                       "--wave-width",
	                                       std::to_string(request.wave_width),
	                                       "--max-dispatches",
	                                       std::to_string(kMaxEndpointDispatches)};
	CommandStatus status    = CommandStatus::Exited;
	int           exit_code = 0;
	if (!RunCommand(host, argv, kEndpointTimeoutS, Join(raw_dir, "endpoint.log"), &status, &exit_code, failure))
	{
		return false;
	}
	if (status == CommandStatus::TimedOut)
	{
		*outcome = Summary(request, "vulkan", "timeout", kExitMismatch);
		return true;
	}
	if (exit_code == kEndpointUnavailable)
	{
		*outcome = Summary(request, "vulkan", "endpoint_unavailable", kExitUnavailable);
		outcome->summary.Set("reason", "compute_capability_unavailable_before_dispatch");
		return true;
	}
	if (exit_code != 0)
	{
		*outcome = Summary(request, "vulkan", exit_code == kEndpointRefused ? "endpoint_refused" : "endpoint_error", kExitMismatch);
		return true;
	}
	Json    meta;
	Failure meta_failure;
	if (!ReadBoundedJson(meta_path, &meta, &meta_failure) || !ValidEndpointMeta(meta, request.op, inputs.count))
	{
		*outcome = Summary(request, "vulkan", "endpoint_metadata_invalid", kExitMismatch);
		return true;
	}
	if (!meta["measured"].AsBool())
	{
		*outcome = Summary(request, "vulkan", "endpoint_not_measured", kExitUnavailable);
		return true;
	}
	std::vector<uint32_t> actual;
	std::string           output_status;
	if (!LoadOutputs(output_path, inputs.count, &actual, &output_status, failure))
	{
		return false;
	}
	if (output_status != "measured")
	{
		*outcome = Summary(request, "vulkan", output_status.c_str(), kExitMismatch);
		return true;
	}
	Json artifact = Json::MakeObject()
	                    .Set("physical_device", meta["physical_device"])
	                    .Set("physical_subgroup_size", meta["subgroup_size"])
	                    .Set("logical_wave_width", request.wave_width)
	                    .Set("subgroup_mode", meta["subgroup_size"].AsInt() == request.wave_width ? "matched" : "differs_qualified")
	                    .Set("dispatches", meta["dispatches"]);
	if (meta.Has("default_subgroup_size"))
	{
		artifact.Set("default_subgroup_size", meta["default_subgroup_size"]);
	}
	*outcome = MeasuredOutcome(request, "vulkan", inputs, actual, std::move(artifact));
	return WriteExclusive(Join(request.out_dir, "result.json"), outcome->summary.Dump(2) + "\n", failure);
}

struct LoadedResult
{
	Json                  result;
	std::vector<uint32_t> values;
	std::string           unavailable_status; // set when the side reports no measurement
};

// Resolves the result directory once and admits only a real raw/ child, so no
// link redirects the output outside it. The leaf is opened without following links.
bool AdmittedOutputPath(const std::string& directory, std::string* path, Failure* failure)
{
	std::error_code ec;
	const fs::path  base = fs::canonical(directory, ec);
	struct stat     raw_status {};
	if (ec || ::lstat((base / "raw").c_str(), &raw_status) != 0)
	{
		const std::string reason = ec ? ec.message() : std::string(std::strerror(errno));
		return Refuse(failure, "result directory has no readable raw child: " + reason);
	}
	if (!S_ISDIR(raw_status.st_mode))
	{
		return Refuse(failure, "raw must be a real directory inside the result directory");
	}
	*path = (base / "raw" / "outputs.bin").string();
	return true;
}

bool LoadResult(const std::string& directory, const std::string& label, LoadedResult* loaded, Failure* failure)
{
	Json& result = loaded->result;
	if (!ReadBoundedJson(Join(directory, "result.json"), &result, failure))
	{
		return false;
	}
	if (!result.IsObject() || result["schema"] != Json(kSchema) || result["backend"] != Json(label))
	{
		return Refuse(failure, label + " directory does not hold a " + label + " result of this schema");
	}
	if (result["measured"] == Json(false))
	{
		if (!result["status"].IsString() || result["status"].AsString().empty())
		{
			return Refuse(failure, label + " result has no status");
		}
		loaded->unavailable_status = result["status"].AsString();
		return true;
	}
	static const char* const measured_keys[] = {"schema", "backend", "status", "measured", "op", "count", "input_md5", "outputs",
	                                            "artifact", "wave_width", "arch"};
	if (result["measured"] != Json(true) ||
	    !std::all_of(std::begin(measured_keys), std::end(measured_keys), [&](const char* key) { return result.Has(key); }))
	{
		return Refuse(failure, label + " result is missing measured fields");
	}
	if (result["status"] != Json("measured") || !IsKnownOp(result["op"]))
	{
		return Refuse(failure, label + " result has an unknown status or operation");
	}
	const int64_t limit = label == "vulkan" ? kVulkanMaxPairs : kMaxCount;
	if (!IsIntegerIn(result["count"], 1, limit))
	{
		return Refuse(failure, label + " result has an invalid pair count");
	}
	if (!IsInputMd5(result["input_md5"]))
	{
		return Refuse(failure, label + " result has an invalid input digest");
	}
	if (result["outputs"] != Json(kOutputsName))
	{
		return Refuse(failure, label + " result names an output location other than the fixed one");
	}
	if (!result["wave_width"].IsInteger() || (result["wave_width"].AsInt() != 32 && result["wave_width"].AsInt() != 64))
	{
		return Refuse(failure, label + " result has an invalid wave width");
	}
	if (result["arch"] != Json(kSupportedArch) || !result["artifact"].IsObject())
	{
		return Refuse(failure, label + " result has an unsupported architecture or artifact");
	}
	const int64_t count = result["count"].AsInt();
	std::string   outputs_path;
	std::string   bytes;
	if (!AdmittedOutputPath(directory, &outputs_path, failure) || !ReadOutputBytes(outputs_path, static_cast<size_t>(4 * count), &bytes, failure))
	{
		return false;
	}
	if (bytes.size() != static_cast<size_t>(4 * count))
	{
		return Refuse(failure, label + " outputs do not match their recorded count");
	}
	loaded->values = UnpackWords(bytes, 0, count);
	return true;
}

// Compares two measured result directories for the same operation and input bytes.
bool CompareResults(const std::string& hip_dir, const std::string& vulkan_dir, Outcome* outcome, Failure* failure)
{
	LoadedResult hip;
	LoadedResult vulkan;
	if (!LoadResult(hip_dir, "hip", &hip, failure) || !LoadResult(vulkan_dir, "vulkan", &vulkan, failure))
	{
		return false;
	}
	for (const auto* side: {&hip, &vulkan})
	{
		if (!side->unavailable_status.empty())
		{
			outcome->summary = Json::MakeObject()
			                       .Set("schema", kSchema)
			                       .Set("status", "unavailable")
			                       .Set("unavailable_side", side == &hip ? "hip" : "vulkan")
			                       .Set("reported_status", side->unavailable_status);
			outcome->exit_code = kExitUnavailable;
			return true;
		}
	}
	for (const char* key: {"op", "count", "input_md5"})
	{
		if (hip.result[key] != vulkan.result[key])
		{
			return Refuse(failure, std::string("results differ in ") + key + "; they were not produced from the same input");
		}
	}
	const Json comparison = CompareOutputs(hip.values, vulkan.values);
	const bool match      = comparison["status"] == Json("match");
	outcome->summary      = Json::MakeObject()
	                       .Set("schema", kSchema)
	                       .Set("status", match ? "match" : "mismatch")
	                       .Set("op", hip.result["op"])
	                       .Set("count", hip.result["count"])
	                       .Set("input_md5", hip.result["input_md5"])
	                       .Set("comparison", comparison)
	                       .Set("hip_artifact", hip.result["artifact"])
	                       .Set("vulkan_artifact", vulkan.result["artifact"]);
	outcome->exit_code = match ? kExitOk : kExitMismatch;
	return true;
}

bool ParseIntegerOption(const ParsedOptions& options, const char* name, int64_t* value, Failure* failure)
{
	return ParseInt64(OptionValue(options, name), value) || Refuse(failure, std::string("argument ") + name + ": invalid int value");
}

bool ParseRequestArgs(const ParsedOptions& options, RequestArgs* args, Failure* failure)
{
	for (const char* required: {"--op", "--wave-width", "--out-dir"})
	{
		if (!HasOption(options, required))
		{
			return Refuse(failure, std::string("the following arguments are required: ") + required);
		}
	}
	const bool has_mode = HasOption(options, "--mode");
	const bool has_file = HasOption(options, "--input-file");
	if (has_mode == has_file)
	{
		return Refuse(failure, has_mode ? "argument --input-file: not allowed with argument --mode"
		                                : "one of the arguments --mode --input-file is required");
	}
	args->op      = OptionValue(options, "--op");
	args->out_dir = OptionValue(options, "--out-dir");
	args->arch    = OptionValue(options, "--arch", kSupportedArch);
	if (has_mode)
	{
		args->mode = OptionValue(options, "--mode");
	} else
	{
		args->input_file = OptionValue(options, "--input-file");
	}
	if (!ParseIntegerOption(options, "--wave-width", &args->wave_width, failure) ||
	    (HasOption(options, "--seed") && !ParseIntegerOption(options, "--seed", &args->seed, failure)))
	{
		return false;
	}
	if (HasOption(options, "--count"))
	{
		int64_t count = 0;
		if (!ParseIntegerOption(options, "--count", &count, failure))
		{
			return false;
		}
		args->count = count;
	}
	if (!args->input_file.has_value() && !args->count.has_value())
	{
		return Refuse(failure, "generated modes need --count");
	}
	return true;
}

bool RunSubcommand(RunnerHost* host, const OracleTools& tools, const std::vector<std::string>& args, Outcome* outcome, Failure* failure)
{
	const std::string&             command = args.empty() ? std::string() : args[0];
	const std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
	std::set<std::string>          values;
	if (command == "run" || command == "vulkan-run")
	{
		values = {"--op", "--mode", "--input-file", "--wave-width", "--count", "--seed", "--arch", "--out-dir"};
		if (command == "vulkan-run")
		{
			values.insert("--endpoint");
		}
	} else if (command == "compare")
	{
		values = {"--hip", "--vulkan"};
	} else if (command != "probe")
	{
		return Refuse(failure, "expected one of probe, run, vulkan-run, compare");
	}
	ParsedOptions options;
	std::string   parse_error;
	if (!ParseOptions(rest, {}, values, &options, &parse_error) || !options.positionals.empty())
	{
		return Refuse(failure, parse_error.empty() ? "unrecognized arguments" : parse_error);
	}
	if (command == "probe")
	{
		outcome->summary   = Probe(tools);
		outcome->exit_code = outcome->summary["status"] == Json("present") ? kExitOk : kExitUnavailable;
		return true;
	}
	if (command == "compare")
	{
		if (!HasOption(options, "--hip") || !HasOption(options, "--vulkan"))
		{
			return Refuse(failure, "the following arguments are required: --hip, --vulkan");
		}
		return CompareResults(OptionValue(options, "--hip"), OptionValue(options, "--vulkan"), outcome, failure);
	}
	if (command == "vulkan-run" && !HasOption(options, "--endpoint"))
	{
		return Refuse(failure, "the following arguments are required: --endpoint");
	}
	RequestArgs request_args;
	Request     request;
	if (!ParseRequestArgs(options, &request_args, failure) || !ValidateRequest(request_args, tools.repo_root, &request, failure))
	{
		return false;
	}
	if (command == "run")
	{
		return RunHip(host, tools, request, outcome, failure);
	}
	const std::string endpoint = OptionValue(options, "--endpoint");
	if (!fs::path(endpoint).is_absolute())
	{
		return Refuse(failure, "endpoint must be an absolute path");
	}
	return RunVulkan(host, request, endpoint, outcome, failure);
}

} // namespace

int RunHardwareOracle(RunnerHost* host, const OracleTools& tools, const std::vector<std::string>& args, std::string* report)
{
	Outcome outcome;
	Failure failure;
	report->clear();
	if (!RunSubcommand(host, tools, args, &outcome, &failure))
	{
		std::fprintf(stderr, "error: %s\n", failure.message.c_str());
		return failure.exit_code;
	}
	*report = outcome.summary.Dump(2) + "\n";
	return outcome.exit_code;
}

int HardwareOracleMain(const std::vector<std::string>& args)
{
	const auto  host = CreateSystemRunnerHost();
	OracleTools tools;
	tools.compiler_path = FindOnPath("hipcc");
	tools.repo_root     = KYTY_REPO_ROOT;
	std::string report;
	const int   rc = RunHardwareOracle(host.get(), tools, args, &report);
	std::fputs(report.c_str(), stdout);
	return rc;
}

#else

int RunHardwareOracle(RunnerHost* /*host*/, const OracleTools& /*tools*/, const std::vector<std::string>& /*args*/, std::string* report)
{
	report->clear();
	std::fprintf(stderr, "error: hardware-oracle is not supported on this host\n");
	return 3;
}

int HardwareOracleMain(const std::vector<std::string>& args)
{
	std::string report;
	return RunHardwareOracle(nullptr, {}, args, &report);
}

#endif

} // namespace Kyty::Agent::Runner
