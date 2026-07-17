#include "Emulator/Validation/DomainValidators.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Validation {
namespace {

using Kyty::Core::Domain::Fail;
using Kyty::Core::Domain::Ok;
using Kyty::Core::Domain::RunRules;
using Kyty::Core::Domain::ValidationResult;

bool IsEmpty(const char* s) noexcept
{
	return s == nullptr || s[0] == '\0';
}

bool HasControlChar(const char* s) noexcept
{
	if (s == nullptr)
	{
		return false;
	}
	for (const char* p = s; *p != '\0'; ++p)
	{
		if (static_cast<unsigned char>(*p) < 0x20)
		{
			return true;
		}
	}
	return false;
}

bool EndsWithEbootName(const char* path) noexcept
{
	if (path == nullptr)
	{
		return false;
	}
	const char* base = path;
	for (const char* p = path; *p != '\0'; ++p)
	{
		if (*p == '/' || *p == '\\')
		{
			base = p + 1;
		}
	}
	return std::strcmp(base, "eboot.bin") == 0;
}

bool ContainsDotDot(const char* path) noexcept
{
	if (path == nullptr)
	{
		return false;
	}
	for (const char* p = path; *p != '\0'; ++p)
	{
		if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/' || p[-1] == '\\') && (p[2] == '\0' || p[2] == '/' || p[2] == '\\'))
		{
			return true;
		}
	}
	return false;
}

} // namespace

ValidationResult ValidateGuestExecutable(const GuestExecutableRequest& req) noexcept
{
	return RunRules(
	    [&]
	    {
		    if (IsEmpty(req.root_path))
		    {
			    return Fail("malformed", "loader", "validate_guest_executable", "guest path is empty");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (HasControlChar(req.root_path))
		    {
			    return Fail("malformed", "loader", "validate_guest_executable", "guest path has control characters");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (ContainsDotDot(req.root_path))
		    {
			    return Fail("unsupported", "loader", "validate_guest_executable", "guest path must not contain '..'");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (req.require_eboot_name && !EndsWithEbootName(req.root_path))
		    {
			    return Fail("malformed", "loader", "validate_guest_executable", "path must name eboot.bin");
		    }
		    return Ok();
	    });
}

ValidationResult ValidateModuleMetadata(const ModuleMetadataRequest& req) noexcept
{
	// Name-shape only — matches what LoadProgram can supply from the file path.
	return RunRules(
	    [&]
	    {
		    if (IsEmpty(req.name))
		    {
			    return Fail("malformed", "loader", "validate_module_metadata", "module name is empty");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (HasControlChar(req.name))
		    {
			    return Fail("malformed", "loader", "validate_module_metadata", "module name has control characters");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    // Production limit: basename from guest module path (not version fabrication).
		    constexpr std::size_t kMaxModuleName = 64;
		    if (std::strlen(req.name) > kMaxModuleName)
		    {
			    return Fail("unsupported", "loader", "validate_module_metadata", "module name exceeds supported length");
		    }
		    return Ok();
	    });
}

ImportResolutionDecision ClassifyImportResolution(const ImportResolveRequest& req) noexcept
{
	if (IsEmpty(req.identity))
	{
		return {ImportResolutionOutcome::Malformed, Fail("malformed", "loader", "classify_import_resolution", "import identity is empty")};
	}
	if (HasControlChar(req.identity))
	{
		return {ImportResolutionOutcome::Malformed,
		        Fail("malformed", "loader", "classify_import_resolution", "import identity has control characters", req.identity)};
	}
	if (req.type == Loader::SymbolType::Unknown)
	{
		return {ImportResolutionOutcome::Malformed,
		        Fail("malformed", "loader", "classify_import_resolution", "import symbol type is unknown", req.identity)};
	}
	if (req.has_export)
	{
		return {ImportResolutionOutcome::Resolved, Ok()};
	}
	if (req.type != Loader::SymbolType::Func)
	{
		return {ImportResolutionOutcome::UnresolvedNonFunction,
		        Fail("policy_denied", "loader", "classify_import_resolution",
		             "missing non-Func import remains unresolved and must be handled by relocation policy", req.identity)};
	}
	if (req.missing_function_import_enabled)
	{
		return {ImportResolutionOutcome::DiagnosticFunctionStub, Ok()};
	}
	return {ImportResolutionOutcome::StrictUnresolvedFunction,
	        Fail("policy_denied", "loader", "classify_import_resolution", "missing Func import not allowed without missing_function_import",
	             req.identity)};
}

ValidationResult ValidateAgentRequest(const AgentRequestDraft& req) noexcept
{
	return RunRules(
	    [&]
	    {
		    if (IsEmpty(req.tool))
		    {
			    return Fail("malformed", "agent", "validate_agent_request", "tool is missing");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    for (const char* p = req.tool; *p != '\0'; ++p)
		    {
			    const unsigned char c = static_cast<unsigned char>(*p);
			    if (!(std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_'))
			    {
				    return Fail("malformed", "agent", "validate_agent_request", "tool has invalid characters", req.tool);
			    }
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (!req.known_tool)
		    {
			    // Same class as AgentServer::Dispatch unknown_tool.
			    return Fail("unsupported", "agent", "validate_agent_request", "unknown tool", req.tool);
		    }
		    return Ok();
	    },
	    [&]
	    {
		    // Mirror Dispatch: pad_down/up/tap require a button argument.
		    if (req.requires_button && !req.has_string_button)
		    {
			    return Fail("policy_denied", "agent", "validate_agent_request", "button is required", req.tool);
		    }
		    return Ok();
	    });
}

ValidationResult ValidateGameRunRequest(const GameRunRequest& req) noexcept
{
	return RunRules(
	    [&]
	    {
		    if (IsEmpty(req.guest_root))
		    {
			    return Fail("malformed", "core", "validate_game_run", "guest root is empty");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (HasControlChar(req.guest_root) || ContainsDotDot(req.guest_root))
		    {
			    return Fail("malformed", "core", "validate_game_run", "guest root path is invalid");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (req.removed_permissive_env_present)
		    {
			    return Fail("unsupported", "core", "validate_game_run", "legacy stub/permissive env is not supported");
		    }
		    return Ok();
	    },
	    [&]
	    {
		    if (req.bringup_env_present && !req.allow_diagnostic_override)
		    {
			    return Fail("policy_denied", "core", "validate_game_run", "KYTY_BRINGUP_* requires KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1");
		    }
		    return Ok();
	    });
}

void FormatErrorLine(const Core::Domain::Error& err, char* buf, std::size_t cap) noexcept
{
	if (buf == nullptr || cap == 0)
	{
		return;
	}
	std::snprintf(buf, cap, "code=%s subsystem=%s operation=%s reason=%s context=%s", err.code, err.subsystem, err.operation, err.reason,
	              err.context);
}

} // namespace Kyty::Emulator::Validation

#endif // KYTY_EMU_ENABLED
