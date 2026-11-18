#include "Emulator/Libs/ProcessEnvironment.h"

#ifdef KYTY_EMU_ENABLED

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Kyty::Libs::ProcessEnvironment {

namespace {

std::mutex g_arguments_mutex;
Arguments  g_arguments {};
std::array<const char*, 16> g_environment {};
// Values the guest set, which shadow g_environment. Kept apart from the host environment.
std::unordered_map<std::string, std::string> g_assigned;

bool EnvironmentNameMatches(const char* assignment, const char* name, size_t name_length)
{
	return assignment != nullptr && std::strncmp(assignment, name, name_length) == 0 && assignment[name_length] == '=';
}

const char* FindLocked(const char* name)
{
	if (const auto assigned = g_assigned.find(name); assigned != g_assigned.end())
	{
		return assigned->second.c_str();
	}
	const size_t name_length = std::strlen(name);
	for (const char* entry: g_environment)
	{
		if (entry == nullptr)
		{
			break;
		}
		if (EnvironmentNameMatches(entry, name, name_length))
		{
			return entry + name_length + 1;
		}
	}
	return nullptr;
}

} // namespace

bool Initialize(const InitParameters* parameters)
{
	Arguments next {};

	if (parameters != nullptr)
	{
		if (parameters->argc < 0 || parameters->argc >= kArgumentCapacity ||
		    parameters->argv[parameters->argc] != nullptr)
		{
			return false;
		}

		next.argc = parameters->argc;
		for (int32_t index = 0; index < parameters->argc; index++)
		{
			next.argv[index] = parameters->argv[index];
		}
	}

	std::lock_guard lock(g_arguments_mutex);
	g_arguments = next;
	g_environment.fill(nullptr);
	g_assigned.clear();
	size_t destination = 0;
	if (parameters != nullptr)
	{
		const int environment_begin = parameters->argc + 1;
		for (int source = environment_begin;
		     source < kArgumentCapacity && destination < g_environment.size() - 1;
		     ++source)
		{
			if (parameters->argv[source] == nullptr)
			{
				break;
			}
			g_environment[destination++] = parameters->argv[source];
		}
	}
	return true;
}

Arguments GetArguments()
{
	std::lock_guard lock(g_arguments_mutex);
	return g_arguments;
}

const char* GetEnvironmentVariable(const char* name)
{
	if (name == nullptr || *name == '\0')
	{
		return nullptr;
	}
	std::lock_guard lock(g_arguments_mutex);
	return FindLocked(name);
}

bool SetEnvironmentVariable(const char* name, const char* value, bool overwrite)
{
	if (name == nullptr || *name == '\0' || std::strchr(name, '=') != nullptr || value == nullptr)
	{
		return false;
	}
	std::lock_guard lock(g_arguments_mutex);
	if (overwrite || FindLocked(name) == nullptr)
	{
		g_assigned[name] = value;
	}
	return true;
}

} // namespace Kyty::Libs::ProcessEnvironment

#endif // KYTY_EMU_ENABLED
