#include "ArchitectureBoundaryCheck.h"

#include <cstdio>
#include <cstring>

// Usage: kyty_architecture_boundary_integration <source_root> [--strict]
// Prints one diagnostic per line and exits 1 when the contract is broken.
int main(int argc, char** argv)
{
	const bool strict = argc == 3 && std::strcmp(argv[2], "--strict") == 0;
	if (argc != 2 && !strict)
	{
		std::fprintf(stderr, "usage: %s <source_root> [--strict]\n", argv[0]);
		return 2;
	}
	const auto result = Kyty::ArchitectureBoundary::CheckSourceRoot(argv[1], strict);
	for (const auto& diagnostic: result.diagnostics)
	{
		std::printf("%s\n", diagnostic.c_str());
	}
	return result.exit_code;
}
