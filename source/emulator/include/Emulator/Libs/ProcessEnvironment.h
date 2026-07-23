#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_PROCESSENVIRONMENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_PROCESSENVIRONMENT_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstdint>

namespace Kyty::Libs::ProcessEnvironment {

constexpr int kArgumentCapacity = 4;

struct InitParameters
{
	int32_t     argc;
	uint32_t    reserved;
	const char* argv[kArgumentCapacity];
};

struct Arguments
{
	int32_t     argc;
	const char* argv[kArgumentCapacity];
};

[[nodiscard]] bool      Initialize(const InitParameters* parameters);
[[nodiscard]] Arguments GetArguments();
[[nodiscard]] const char* GetEnvironmentVariable(const char* name);

} // namespace Kyty::Libs::ProcessEnvironment

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_PROCESSENVIRONMENT_H_ */
