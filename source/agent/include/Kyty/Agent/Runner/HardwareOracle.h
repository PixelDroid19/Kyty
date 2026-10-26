#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_HARDWAREORACLE_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_HARDWAREORACLE_H_

#include "Kyty/Agent/Runner/RunnerHost.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

// Integer instruction oracle: native AMD gfx1030 execution and external comparison.
//
// The native backend compiles one HIP kernel whose five 32-bit integer operations
// are AMDGCN inline assembly (v_add_nc_u32, v_mul_lo_u32, v_mul_hi_u32,
// v_lshlrev_b32, v_lshrrev_b32). Before the binary is run, the compiler's device
// assembly is checked for each mnemonic, so a C++ fallback cannot pass as an ISA
// measurement. Only gfx1030 is supported.
//
// The Vulkan endpoint is an external executable that runs the production shader
// pipeline. The oracle only invokes it with a fixed argument list and validates
// its protocol; an absent executable reports the backend as unavailable.
//
// `compare` checks two measured result directories for the same input bytes. The
// host arithmetic reference is recorded alongside each measurement, but it is not
// the oracle and does not decide the comparison.
//
// Scope: five unsigned 32-bit operations on gfx1030 only. No floating-point,
// denormal, graphics or frame behavior is covered.

// Exact unsigned 32-bit results of `op` (add, mul, mulhi, shl, shr); shift counts
// use their low five bits. Ancillary only.
[[nodiscard]] std::vector<uint32_t> OracleHostReference(const std::string& op, const std::vector<uint32_t>& a,
                                                        const std::vector<uint32_t>& b);

// Reproducible operands for the generated modes "seeded" (64-bit LCG) and "edges".
void MakeOracleInputs(const std::string& mode, int64_t count, uint32_t seed, std::vector<uint32_t>* a, std::vector<uint32_t>* b);

// Expected mnemonics that do not start an instruction line; comments do not count.
[[nodiscard]] std::vector<std::string> MissingOracleMnemonics(const std::string& assembly);

struct OracleTools
{
	std::string kfd_path = "/dev/kfd";
	std::string compiler_path; // resolved HIP compiler; empty when it is not installed
	std::string repo_root;     // results must be written outside it
};

// `hardware-oracle probe|run|vulkan-run|compare ...` on `host`; the JSON report
// goes to `report`. Exit codes: 0 ok or match, 1 mismatch or failed measurement,
// 2 usage, 3 unavailable, 4 output error.
[[nodiscard]] int RunHardwareOracle(RunnerHost* host, const OracleTools& tools, const std::vector<std::string>& args,
                                    std::string* report);

// `kyty_agent hardware-oracle ...` with the system host, PATH and repository.
[[nodiscard]] int HardwareOracleMain(const std::vector<std::string>& args);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_HARDWAREORACLE_H_ */
