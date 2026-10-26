#ifndef INTEGRATION_TEST_SRC_ARCHITECTUREBOUNDARYCHECK_H_
#define INTEGRATION_TEST_SRC_ARCHITECTUREBOUNDARYCHECK_H_

#include <filesystem>
#include <string>
#include <vector>

namespace Kyty::ArchitectureBoundary {

struct CheckResult
{
	int                      exit_code = 0;
	std::vector<std::string> diagnostics;
};

// Checks the explicit emulator dependency-direction contract over a fixed set of
// source files under source_root. strict also requires every kernel
// implementation to live in the Kyty::Kernel namespace. Diagnostics are sorted
// and deterministic; exit_code is 1 when there is any.
[[nodiscard]] CheckResult CheckSourceRoot(const std::filesystem::path& source_root, bool strict);

} // namespace Kyty::ArchitectureBoundary

#endif /* INTEGRATION_TEST_SRC_ARCHITECTUREBOUNDARYCHECK_H_ */
