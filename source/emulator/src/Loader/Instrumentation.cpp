#include "Emulator/Loader/Instrumentation.h"

#include "Emulator/Profiling.h"

#include <easy/profiler.h>

#include <memory>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::Instrumentation {

struct FunctionScope::Impl
{
	const profiler::BaseBlockDescriptor* descriptor;
	profiler::Block                       block;

	static const profiler::BaseBlockDescriptor* RegisterDescriptor(const char* name, const char* file, int line)
	{
		const std::string unique_id = std::string(file != nullptr ? file : "Loader") + ":" + std::to_string(line);
		return profiler::registerDescription(profiler::ON, unique_id.c_str(), name != nullptr ? name : "Loader", file != nullptr ? file : "Loader",
		                                    line, profiler::BlockType::Block, profiler::colors::Default, true);
	}

	Impl(const char* name, const char* file, int line)
	    : descriptor(RegisterDescriptor(name, file, line)),
      block(descriptor, name)
	{
		profiler::beginBlock(block);
	}
};

FunctionScope::FunctionScope(const char* name, const char* file, int line)
	: m_impl(std::make_unique<Impl>(name != nullptr ? name : "Loader", file, line))
{
}

FunctionScope::~FunctionScope() = default;

void SetThreadName(const char* name)
{
	static thread_local const char* registered_name = nullptr;
	if (registered_name == nullptr)
	{
		registered_name = profiler::registerThread(name != nullptr ? name : "Loader");
	}
}

} // namespace Kyty::Loader::Instrumentation

#else

namespace Kyty::Loader::Instrumentation {

struct FunctionScope::Impl
{
};

FunctionScope::FunctionScope(const char* /*name*/, const char* /*file*/, int /*line*/) : m_impl(std::make_unique<Impl>()) {}

FunctionScope::~FunctionScope() = default;

void SetThreadName(const char* /*name*/) {}

} // namespace Kyty::Loader::Instrumentation

#endif // KYTY_EMU_ENABLED
