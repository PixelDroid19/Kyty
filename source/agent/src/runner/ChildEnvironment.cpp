#include "Kyty/Agent/Runner/RunnerHost.h"

#include <algorithm>

#if defined(_WIN32)
#include <stdlib.h>
#else
extern char** environ; // NOLINT(readability-redundant-declaration)
#endif

namespace Kyty::Agent::Runner {
namespace {

const std::vector<std::string>& HostEnvironmentKeys()
{
	static const std::vector<std::string> keys = {
	    "PATH",
	    "HOME",
	    "USER",
	    "LOGNAME",
	    "LANG",
	    "LC_ALL",
	    "LC_CTYPE",
	    "DISPLAY",
	    "WAYLAND_DISPLAY",
	    "XDG_RUNTIME_DIR",
	    "XAUTHORITY",
	    "XDG_SESSION_TYPE",
	    "DBUS_SESSION_BUS_ADDRESS",
	    "VK_ICD_FILENAMES",
	    "VK_DRIVER_FILES",
	    "LD_LIBRARY_PATH",
	    "LIBGL_DRIVERS_PATH",
	    "MESA_LOADER_DRIVER_OVERRIDE",
	    "AMD_VULKAN_ICD",
	    "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1",
	};
	return keys;
}

std::string ValueOf(const Environment& env, const std::string& key)
{
	const auto it = env.find(key);
	return it == env.end() ? std::string() : it->second;
}

} // namespace

const std::vector<std::string>& StrictForbiddenEnvironmentKeys()
{
	static const std::vector<std::string> keys = {
	    "KYTY_BRINGUP_MODE",
	    "KYTY_BRINGUP_FEATURES",
	    "KYTY_BRINGUP_SUBSYSTEMS",
	    "KYTY_BRINGUP_BURST_LIMIT",
	    "KYTY_BRINGUP_BURST_WINDOW_MS",
	    "KYTY_BRINGUP_ALLOW_DIAGNOSTIC",
	    "KYTY_STUB_MISSING",
	    "KYTY_GFX_PERMISSIVE",
	    "KYTY_AUTO_CROSS",
	    "KYTY_SKIP_UD2",
	};
	return keys;
}

Environment BuildChildEnvironment(const Environment& base, const ChildEnvironmentOptions& options)
{
	Environment env;
	for (const auto& key: HostEnvironmentKeys())
	{
		if (const std::string value = ValueOf(base, key); !value.empty())
		{
			env[key] = value;
		}
	}
	env["KYTY_GUEST_ROOT"]         = options.guest_root;
	env["KYTY_NATIVE_CAPTURE_DIR"] = options.capture_directory;
	env["KYTY_PRINTF_DIRECTION"]   = "Silent";
	env["KYTY_SCREEN_WIDTH"]       = base.count("KYTY_SCREEN_WIDTH") != 0 ? base.at("KYTY_SCREEN_WIDTH") : "1280";
	env["KYTY_SCREEN_HEIGHT"]      = base.count("KYTY_SCREEN_HEIGHT") != 0 ? base.at("KYTY_SCREEN_HEIGHT") : "720";
	if (!options.agent_endpoint.empty())
	{
		env["KYTY_AGENT_ENDPOINT"] = options.agent_endpoint;
	}
	for (const auto& key: options.optional_values)
	{
		if (const std::string value = ValueOf(base, key); !value.empty())
		{
			env[key] = value;
		}
	}
	for (const auto& [key, fallback]: options.default_values)
	{
		const std::string value = ValueOf(base, key);
		env[key]                = value.empty() ? fallback : value;
	}
	for (const auto& key: StrictForbiddenEnvironmentKeys())
	{
		env.erase(key);
	}
	return env;
}

std::vector<std::string> FindActiveEnvironmentKeys(const Environment& env, const std::vector<std::string>& keys,
                                                   const std::vector<std::string>& allowed)
{
	std::vector<std::string> active;
	for (const auto& key: keys)
	{
		if (!ValueOf(env, key).empty() && std::find(allowed.begin(), allowed.end(), key) == allowed.end())
		{
			active.push_back(key);
		}
	}
	return active;
}

Environment CurrentEnvironment()
{
	Environment env;
#if defined(_WIN32)
	char** entries = _environ;
#else
	char** entries = environ;
#endif
	for (char** entry = entries; entry != nullptr && *entry != nullptr; ++entry)
	{
		const std::string text(*entry);
		const size_t      equals = text.find('=');
		if (equals != std::string::npos && equals > 0)
		{
			env.emplace(text.substr(0, equals), text.substr(equals + 1));
		}
	}
	return env;
}

} // namespace Kyty::Agent::Runner
