#include "Kyty/Agent/Runner/HardwareOracle.h"
#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/RunnerHost.h"
#include "Kyty/UnitTest.h"

#include "ScopedTestDirectory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

UT_BEGIN(AgentHardwareOracle);

using namespace Kyty::Agent::Runner; // NOLINT(google-build-using-namespace)
namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kOps = {"add", "mul", "mulhi", "shl", "shr"};

void WriteBytes(const fs::path& path, const std::string& bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << bytes;
}

std::string ReadBytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<uint32_t> Words(const std::string& bytes)
{
	std::vector<uint32_t> words(bytes.size() / 4);
	for (size_t i = 0; i < words.size(); i++)
	{
		const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + 4 * i;
		words[i]      = p[0] | (uint32_t {p[1]} << 8u) | (uint32_t {p[2]} << 16u) | (uint32_t {p[3]} << 24u);
	}
	return words;
}

std::string Bytes(const std::vector<uint32_t>& words)
{
	std::string bytes;
	for (const uint32_t word: words)
	{
		for (uint32_t shift = 0; shift < 32; shift += 8)
		{
			bytes.push_back(static_cast<char>((word >> shift) & 0xFFu));
		}
	}
	return bytes;
}

const char* const kAllMnemonics = "\tv_add_nc_u32 v0, v1, v2\n\tv_mul_lo_u32 v0, v1, v2\n\tv_mul_hi_u32 v0, v1, v2\n"
                                  "\tv_lshlrev_b32 v0, v1, v2\n\tv_lshrrev_b32 v0, v1, v2\n";

// Each launch is answered by `respond`, which may write the child's files and
// returns its exit code; no value keeps the child running until it is terminated.
class OracleHost: public RunnerHost
{
public:
	double Now() override { return now; }
	void   Sleep(double seconds) override { now += seconds; }

	bool Launch(const LaunchSpec& spec, std::string* /*error*/) override
	{
		launches.push_back(spec.argv);
		exit_code = respond(spec);
		return true;
	}
	[[nodiscard]] int64_t ChildPid() const override { return 1; }
	std::optional<int>    PollChild() override { return exit_code; }
	void                  TerminateChild(std::vector<std::string>* notes) override
	{
		++terminations;
		exit_code = -9;
		notes->emplace_back("killed_sigkill");
	}
	AgentReply CallAgent(const std::string& /*endpoint*/, const std::string& /*tool*/, const Json& /*args*/, double /*timeout_s*/) override
	{
		return {};
	}

	std::function<std::optional<int>(const LaunchSpec&)> respond;
	std::vector<std::vector<std::string>>                launches;
	std::optional<int>                                   exit_code;
	double                                               now          = 0.0;
	int                                                  terminations = 0;
};

// A gfx1030 toolchain that emits `assembly`, reports `meta` and computes exact results.
std::optional<int> FakeHip(const LaunchSpec& spec, const std::string& assembly, const std::string& meta)
{
	const auto& argv = spec.argv;
	if (std::find(argv.begin(), argv.end(), "-S") != argv.end())
	{
		WriteBytes(argv[std::find(argv.begin(), argv.end(), "-o") - argv.begin() + 1], assembly);
		return 0;
	}
	if (argv.size() == 2 && argv[1] == "--version")
	{
		WriteBytes(spec.log_path, "\nHIP version: 6.2.41133\n");
		return 0;
	}
	if (argv.size() == 6)
	{
		const std::string           inputs = ReadBytes(argv[3]);
		const std::vector<uint32_t> words  = Words(inputs);
		const size_t                count  = words.size() / 2;
		const std::vector<uint32_t> a(words.begin(), words.begin() + static_cast<int64_t>(count));
		const std::vector<uint32_t> b(words.begin() + static_cast<int64_t>(count), words.end());
		WriteBytes(argv[4], Bytes(OracleHostReference(kOps[std::stoul(argv[1])], a, b)));
		WriteBytes(argv[5], meta);
	}
	return 0;
}

const char* const kGfx1030Meta = R"({"warp_size": 32, "device_name": "AMD Radeon RX 6800", "gcn_arch_name": "gfx1030:sramecc-:xnack-", )"
                                 R"("runtime_version": 60241133, "driver_version": 60241133})";

struct OracleRun
{
	int  exit_code = -1;
	Json report;
};

OracleRun RunOracle(OracleHost* host, const OracleTools& tools, const std::vector<std::string>& args)
{
	OracleRun   run;
	std::string text;
	run.exit_code = RunHardwareOracle(host, tools, args, &text);
	if (!text.empty())
	{
		std::string error;
		EXPECT_TRUE(ParseJson(text, &run.report, &error)) << error;
	}
	return run;
}

class HardwareOracleHarness
{
public:
	HardwareOracleHarness(): m_dir("agent-hardware-oracle")
	{
		WriteBytes(m_dir.Path() / "kfd", "");
		tools.kfd_path      = (m_dir.Path() / "kfd").string();
		tools.compiler_path = "/opt/rocm/bin/hipcc";
		tools.repo_root     = (m_dir.Path() / "repo").string();
	}

	[[nodiscard]] std::string Out(const char* name) const { return (m_dir.Path() / name).string(); }

	OracleTools tools;

private:
	ScopedTestDirectory m_dir;
};

void WriteResult(const fs::path& directory, const char* backend, const std::vector<uint32_t>& values, bool measured = true)
{
	Json result = Json::MakeObject().Set("schema", "kyty_hardware_oracle_v2").Set("backend", backend).Set("measured", measured);
	if (measured)
	{
		result.Set("status", "measured")
		    .Set("op", "add")
		    .Set("count", static_cast<int64_t>(values.size()))
		    .Set("input_md5", std::string(32, 'a'))
		    .Set("outputs", "raw/outputs.bin")
		    .Set("artifact", Json::MakeObject().Set("side", backend))
		    .Set("wave_width", 64)
		    .Set("arch", "gfx1030");
	} else
	{
		result.Set("status", "device_error");
	}
	WriteBytes(directory / "result.json", result.Dump(2));
	WriteBytes(directory / "raw" / "outputs.bin", Bytes(values));
}

} // namespace

TEST(AgentHardwareOracle, HostReferenceWrapsAtThirtyTwoBitsAndShiftsUseTheLowFiveBits)
{
	EXPECT_EQ(OracleHostReference("add", {0xFFFFFFFFu, 2}, {1, 3}), (std::vector<uint32_t> {0, 5}));
	EXPECT_EQ(OracleHostReference("mul", {0x10000u}, {0x10000u}), (std::vector<uint32_t> {0}));
	EXPECT_EQ(OracleHostReference("mulhi", {0xFFFFFFFFu}, {0xFFFFFFFFu}), (std::vector<uint32_t> {0xFFFFFFFEu}));
	EXPECT_EQ(OracleHostReference("shl", {1}, {33}), (std::vector<uint32_t> {2}));
	EXPECT_EQ(OracleHostReference("shr", {0x80000000u}, {63}), (std::vector<uint32_t> {1}));
}

TEST(AgentHardwareOracle, GeneratedInputsAreReproducible)
{
	std::vector<uint32_t> a;
	std::vector<uint32_t> b;
	MakeOracleInputs("seeded", 3, 1, &a, &b);
	EXPECT_EQ(a, (std::vector<uint32_t> {0x6c576facu, 0x826886b3u, 0xa5fae199u}));
	EXPECT_EQ(b, (std::vector<uint32_t> {0x620355cdu, 0xcba276b4u, 0x802181e6u}));
	MakeOracleInputs("edges", 7, 1, &a, &b);
	EXPECT_EQ(a, (std::vector<uint32_t> {0, 0, 0, 0, 0, 1, 1}));
	EXPECT_EQ(b, (std::vector<uint32_t> {0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0, 1}));
}

TEST(AgentHardwareOracle, AssemblyGateCountsOnlyInstructionLines)
{
	EXPECT_TRUE(MissingOracleMnemonics(kAllMnemonics).empty());
	std::string commented = kAllMnemonics;
	commented.replace(commented.find("\tv_mul_hi"), 1, "; ");
	EXPECT_EQ(MissingOracleMnemonics(commented), (std::vector<std::string> {"v_mul_hi_u32"}));
	EXPECT_EQ(MissingOracleMnemonics("v_add_nc_u32").size(), 5u);
}

// The oracle runs commands and reads device files only on POSIX hosts.
#if !defined(_WIN32)
TEST(AgentHardwareOracle, MeasuredNativeRunChecksTheAssemblyAndRecordsTheArtifact)
{
	HardwareOracleHarness harness;
	OracleHost            host;
	host.respond = [](const LaunchSpec& spec) { return FakeHip(spec, kAllMnemonics, kGfx1030Meta); };
	const std::string out = harness.Out("native");
	const OracleRun   run = RunOracle(&host, harness.tools, {"run", "--op", "mulhi", "--mode", "edges", "--count", "25", "--wave-width", "32", "--out-dir", out});

	EXPECT_EQ(run.exit_code, 0);
	EXPECT_EQ(run.report["status"], Json("measured"));
	EXPECT_EQ(run.report["host_reference"]["status"], Json("match"));
	EXPECT_EQ(run.report["artifact"]["compiler_version"], Json("HIP version: 6.2.41133"));
	EXPECT_EQ(run.report["artifact"]["gcn_arch_name"], Json("gfx1030:sramecc-:xnack-"));
	ASSERT_EQ(host.launches.size(), 4u);
	const auto& assemble = host.launches[0];
	EXPECT_NE(std::find(assemble.begin(), assemble.end(), "--offload-arch=gfx1030"), assemble.end());
	EXPECT_NE(std::find(assemble.begin(), assemble.end(), "--cuda-device-only"), assemble.end());
	EXPECT_EQ(std::find(assemble.begin(), assemble.end(), "-mwavefrontsize64"), assemble.end());
	EXPECT_EQ(host.launches[3][1], "2");
	Json saved;
	std::string error;
	ASSERT_TRUE(ParseJson(ReadBytes(fs::path(out) / "result.json"), &saved, &error)) << error;
	EXPECT_EQ(saved, run.report);
}

TEST(AgentHardwareOracle, NativeRunRefusesUncheckedOrForeignMeasurements)
{
	HardwareOracleHarness harness;
	const std::vector<std::string> args = {"run", "--op", "add", "--mode", "seeded", "--count", "8", "--wave-width", "32", "--out-dir"};

	OracleHost no_mulhi;
	no_mulhi.respond = [](const LaunchSpec& spec) { return FakeHip(spec, "\tv_add_nc_u32 v0, v1, v2\n", kGfx1030Meta); };
	std::vector<std::string> first = args;
	first.push_back(harness.Out("asm"));
	const OracleRun asm_run = RunOracle(&no_mulhi, harness.tools, first);
	EXPECT_EQ(asm_run.exit_code, 1);
	EXPECT_EQ(asm_run.report["status"], Json("asm_check_failed"));
	EXPECT_EQ(no_mulhi.launches.size(), 1u);

	OracleHost other_arch;
	other_arch.respond = [](const LaunchSpec& spec)
	{
		std::string meta = kGfx1030Meta;
		meta.replace(meta.find("gfx1030"), 7, "gfx1100");
		return FakeHip(spec, kAllMnemonics, meta);
	};
	std::vector<std::string> second = args;
	second.push_back(harness.Out("arch"));
	const OracleRun arch_run = RunOracle(&other_arch, harness.tools, second);
	EXPECT_EQ(arch_run.exit_code, 1);
	EXPECT_EQ(arch_run.report["status"], Json("arch_mismatch"));
	EXPECT_FALSE(arch_run.report["measured"].AsBool());
	EXPECT_FALSE(fs::exists(fs::path(harness.Out("arch")) / "result.json"));

	OracleHost hung;
	hung.respond = [](const LaunchSpec& /*spec*/) { return std::optional<int>(); };
	std::vector<std::string> third = args;
	third.push_back(harness.Out("hung"));
	const OracleRun hung_run = RunOracle(&hung, harness.tools, third);
	EXPECT_EQ(hung_run.report["status"], Json("timeout"));
	EXPECT_EQ(hung.terminations, 1);
}

TEST(AgentHardwareOracle, MissingDeviceIsUnavailableAndRunsNothing)
{
	HardwareOracleHarness harness;
	harness.tools.kfd_path = harness.Out("no-kfd");
	OracleHost      host;
	const OracleRun run = RunOracle(&host, harness.tools, {"run", "--op", "add", "--mode", "seeded", "--count", "8", "--wave-width", "32", "--out-dir", harness.Out("none")});
	EXPECT_EQ(run.exit_code, 3);
	EXPECT_EQ(run.report["status"], Json("unavailable"));
	EXPECT_FALSE(run.report["probe"]["checks"]["kfd_device"].AsBool());
	EXPECT_TRUE(host.launches.empty());
	EXPECT_FALSE(fs::exists(harness.Out("none")));
}

TEST(AgentHardwareOracle, EndpointExitCodesKeepUnavailabilityApartFromErrors)
{
	HardwareOracleHarness harness;
	const std::string     endpoint = harness.Out("endpoint");
	WriteBytes(endpoint, "#!/bin/sh\n");
	fs::permissions(endpoint, fs::perms::owner_all);
	const auto run_with = [&](int code, const char* out)
	{
		OracleHost host;
		host.respond = [code](const LaunchSpec& /*spec*/) { return std::optional<int>(code); };
		return RunOracle(&host, harness.tools,
		           {"vulkan-run", "--op", "add", "--mode", "seeded", "--count", "8", "--wave-width", "64", "--out-dir", harness.Out(out), "--endpoint", endpoint});
	};
	const OracleRun unavailable = run_with(77, "e77");
	EXPECT_EQ(unavailable.exit_code, 3);
	EXPECT_EQ(unavailable.report["status"], Json("endpoint_unavailable"));
	EXPECT_EQ(run_with(2, "e2").report["status"], Json("endpoint_refused"));
	EXPECT_EQ(run_with(5, "e5").report["status"], Json("endpoint_error"));
}

TEST(AgentHardwareOracle, CompareReportsTheFirstDisagreeingLaneOrTheUnmeasuredSide)
{
	HardwareOracleHarness harness;
	OracleHost            host;
	WriteResult(harness.Out("hip"), "hip", {1, 2, 3});
	WriteResult(harness.Out("vulkan"), "vulkan", {1, 9, 3});
	const OracleRun mismatch = RunOracle(&host, harness.tools, {"compare", "--hip", harness.Out("hip"), "--vulkan", harness.Out("vulkan")});
	EXPECT_EQ(mismatch.exit_code, 1);
	EXPECT_EQ(mismatch.report["comparison"]["first_index"], Json(1));
	EXPECT_EQ(mismatch.report["comparison"]["actual"], Json(9));

	WriteResult(harness.Out("hip-down"), "hip", {}, false);
	const OracleRun unavailable = RunOracle(&host, harness.tools, {"compare", "--hip", harness.Out("hip-down"), "--vulkan", harness.Out("vulkan")});
	EXPECT_EQ(unavailable.exit_code, 3);
	EXPECT_EQ(unavailable.report["unavailable_side"], Json("hip"));

	const OracleRun swapped = RunOracle(&host, harness.tools, {"compare", "--hip", harness.Out("vulkan"), "--vulkan", harness.Out("hip")});
	EXPECT_EQ(swapped.exit_code, 2);
}

#endif
UT_END();
