#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_INSTRUMENTATION_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_INSTRUMENTATION_H_

#include <memory>

namespace Kyty::Loader::Instrumentation {

// Loader code reports lifecycle/relocation scopes through this narrow seam.
// The profiler implementation stays in Instrumentation.cpp so RuntimeLinker
// does not include the development profiler headers directly.
class FunctionScope final
{
public:
	FunctionScope(const char* name, const char* file, int line);
	~FunctionScope();

	FunctionScope(const FunctionScope&)            = delete;
	FunctionScope& operator=(const FunctionScope&) = delete;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

void SetThreadName(const char* name);

} // namespace Kyty::Loader::Instrumentation

#define KYTY_LOADER_PROFILE_FUNCTION() \
	::Kyty::Loader::Instrumentation::FunctionScope kyty_loader_profile_scope(__func__, __FILE__, __LINE__)
#define KYTY_LOADER_PROFILE_THREAD(name) ::Kyty::Loader::Instrumentation::SetThreadName(name)

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_INSTRUMENTATION_H_ */
