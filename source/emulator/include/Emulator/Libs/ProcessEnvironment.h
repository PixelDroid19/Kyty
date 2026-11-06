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
// Guest setenv. Returns false for a name POSIX rejects (null, empty or holding '=') or a null value.
// A new value invalidates the pointer an earlier lookup of the same name returned.
[[nodiscard]] bool SetEnvironmentVariable(const char* name, const char* value, bool overwrite);

} // namespace Kyty::Libs::ProcessEnvironment

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_PROCESSENVIRONMENT_H_ */
