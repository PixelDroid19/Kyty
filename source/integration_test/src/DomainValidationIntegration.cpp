// Process-isolated scenarios that drive SHIPPED entry points:
//   BringUp::ValidateConfig / InitializeFromEnvironment
//   Agent::ParseAndValidateRequestLine / ParseRequestLine
//   RuntimeLinker::Resolve (import validation)
//   RuntimeLinker::LoadProgram path validation
//   GameRun via ValidateGameRunRequest as called from kyty_mount (pure + mount gate)
// No proprietary game binaries.

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/DomainResult.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/Protocol.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Validation/DomainValidators.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Kyty;
using Kyty::Core::Domain::ValidationResult;
using Kyty::Emulator::Agent::ErrorInfo;
using Kyty::Emulator::Agent::ParseAndValidateRequestLine;
using Kyty::Emulator::Agent::ParseRequestLine;
using Kyty::Emulator::Agent::Request;

namespace {

[[noreturn]] void Die(const char* msg)
{
	std::fprintf(stderr, "domain_validation FAIL: %s\n", msg);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		Die(msg);
	}
}

void ExpectCode(const ValidationResult& r, const char* code)
{
	Expect(!r.Ok(), "expected failure result");
	Expect(std::strcmp(r.error.code, code) == 0, "unexpected error code");
	Expect(r.error.subsystem[0] != '\0', "subsystem required");
	Expect(r.error.operation[0] != '\0', "operation required");
	Expect(r.error.reason[0] != '\0', "reason required");
}

int InitHost(int argc, char** argv)
{
	auto* subsystems = Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(argc, argv);
	auto* core    = Core::CoreSubsystem::Instance();
	auto* log     = Log::LogSubsystem::Instance();
	auto* threads = Core::ThreadsSubsystem::Instance();
	auto* config  = Config::ConfigSubsystem::Instance();
	subsystems->Add(core, {});
	subsystems->Add(config, {core});
	subsystems->Add(threads, {core});
	subsystems->Add(log, {core, config, threads});
	if (!subsystems->InitAll(false))
	{
		return 125;
	}
	Libs::LibKernel::PthreadInitSelfForMainThread();
	return 0;
}

// --- Config: real ValidateConfig + InitializeFromEnvironment path ---

int ConfigValid()
{
	Core::BringUp::EnvView env {};
	env.mode     = "unsafe";
	const auto v = Core::BringUp::ValidateConfig(env);
	Expect(v.result.Ok(), "ValidateConfig unsafe");
	Expect(v.config.mode == Core::BringUp::Mode::Unsafe, "mode");
	return 0;
}

int ConfigMalformed()
{
	Core::BringUp::EnvView env {};
	env.mode     = "not_a_mode";
	const auto v = Core::BringUp::ValidateConfig(env);
	ExpectCode(v.result, "malformed");
	return 0;
}

int ConfigEmptyMode()
{
	// Empty string is present-but-invalid (not unset → strict).
	Core::BringUp::EnvView env {};
	env.mode     = "";
	const auto v = Core::BringUp::ValidateConfig(env);
	ExpectCode(v.result, "malformed");
	Expect(std::strstr(v.result.error.context, "KYTY_BRINGUP_MODE") != nullptr || std::strstr(v.result.error.reason, "empty") != nullptr,
	       "empty mode diagnostic");
	return 0;
}

int ConfigEmptyFeatures()
{
	Core::BringUp::EnvView env {};
	env.mode     = "unsafe";
	env.features = "";
	const auto v = Core::BringUp::ValidateConfig(env);
	ExpectCode(v.result, "malformed");
	return 0;
}

int ConfigUnsupported()
{
	Core::BringUp::EnvView env {};
	env.mode     = "unsafe";
	env.features = "not_implemented,unknown_feature";
	const auto v = Core::BringUp::ValidateConfig(env);
	ExpectCode(v.result, "unsupported");
	return 0;
}

int ConfigPolicyDenied()
{
	Core::BringUp::EnvView env {};
	env.mode     = "strict";
	env.features = "not_implemented";
	const auto v = Core::BringUp::ValidateConfig(env);
	ExpectCode(v.result, "policy_denied");
	return 0;
}

// --- Agent: real ParseAndValidateRequestLine / ParseRequestLine ---

int AgentValid()
{
	Request    req {};
	const auto r = ParseAndValidateRequestLine(R"({"id":1,"tool":"ping"})", &req);
	Expect(r.Ok(), "ping must parse+validate");
	Expect(req.tool == "ping", "tool");
	Expect(req.id == 1, "id");
	return 0;
}

int AgentMalformed()
{
	Request    req {};
	const auto r = ParseAndValidateRequestLine(R"({"id":1})", &req);
	ExpectCode(r, "malformed");
	ErrorInfo err {};
	Expect(!ParseRequestLine(R"({"id":1})", &req, &err), "adapter false");
	Expect(err.code == "malformed", "adapter code");
	return 0;
}

int AgentUnsupported()
{
	// Unknown tool is rejected by ValidateAgentRequest (same allowlist as Dispatch).
	Request    req {};
	const auto r = ParseAndValidateRequestLine(R"({"id":1,"tool":"not_a_registered_tool"})", &req);
	ExpectCode(r, "unsupported");
	return 0;
}

int AgentPolicyDenied()
{
	// Real Dispatch contract: pad_down requires button.
	Request    req {};
	const auto r = ParseAndValidateRequestLine(R"({"id":2,"tool":"pad_down","args":{}})", &req);
	ExpectCode(r, "policy_denied");
	return 0;
}

// --- Import: real RuntimeLinker::Resolve ---

void SetupLinker(Loader::RuntimeLinker& linker, Loader::Program& program, Loader::DynamicInfo& dynamic_info)
{
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;
}

int ImportValid()
{
	// Feature on via process env already applied by main's InitializeBringUp path;
	// for pure valid export win we add a real symbol and resolve under any policy.
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	Loader::Program       program {};
	SetupLinker(linker, program, dynamic_info);

	Loader::SymbolResolve sr {};
	sr.name                 = U"sanitizedNid";
	sr.library              = U"libIntegration";
	sr.library_version      = 1;
	sr.module               = U"moduleIntegration";
	sr.module_version_major = 1;
	sr.module_version_minor = 0;
	sr.type                 = Loader::SymbolType::Func;
	linker.Symbols()->Add(sr, 0x1000);

	Loader::SymbolRecord out {};
	linker.Resolve(U"sanitizedNid#lib-id#module-id", Loader::SymbolType::Func, &program, &out, nullptr);
	Expect(out.vaddr == 0x1000, "export wins");
	return 0;
}

int ImportMalformed()
{
	// The pure decision and the production Resolve path must agree: malformed
	// import identities are terminal at the owning boundary.
	Emulator::Validation::ImportResolveRequest req {};
	req.type            = Loader::SymbolType::Func;
	req.identity        = "";
	const auto decision = Emulator::Validation::ClassifyImportResolution(req);
	Expect(decision.outcome == Emulator::Validation::ImportResolutionOutcome::Malformed, "empty import identity is malformed");
	ExpectCode(decision.validation, "malformed");

	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	Loader::Program       program {};
	SetupLinker(linker, program, dynamic_info);
	Loader::SymbolRecord out {};
	linker.Resolve(U"not-three-parts", Loader::SymbolType::Func, &program, &out, nullptr);
	Die("malformed import must terminate explicitly");
}

int ImportUnsupported()
{
	// Diagnostic Func stubs never extend to data/TLS/non-Func ABI classes.
	// Resolve leaves the object unresolved so relocation can apply the binding
	// policy: weak objects receive the invalid-memory sentinel, strong objects
	// terminate with complete relocation context.
	Expect(Core::BringUp::IsEnabled(Core::BringUp::Feature::MissingFunctionImport, Core::BringUp::Subsystem::Loader), "feature must be on");
	Emulator::Validation::ImportResolveRequest req {};
	req.type                            = Loader::SymbolType::Object;
	req.identity                        = "dataObj";
	req.missing_function_import_enabled = true;
	const auto decision                 = Emulator::Validation::ClassifyImportResolution(req);
	Expect(decision.outcome == Emulator::Validation::ImportResolutionOutcome::UnresolvedNonFunction,
	       "missing Object import remains explicitly unresolved");
	ExpectCode(decision.validation, "policy_denied");

	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	Loader::Program       program {};
	SetupLinker(linker, program, dynamic_info);
	Loader::SymbolRecord out {};
	linker.Resolve(U"dataObj#lib-id#module-id", Loader::SymbolType::Object, &program, &out, nullptr);
	Expect(out.vaddr == 0, "Resolve must not fabricate a non-function address");
	return 0;
}

int ImportPolicyDenied()
{
	// Strict missing Func remains unresolved for the existing PLT failure path,
	// but this is now an explicit typed outcome rather than a string comparison.
	Expect(!Core::BringUp::IsEnabled(Core::BringUp::Feature::MissingFunctionImport, Core::BringUp::Subsystem::Loader),
	       "feature must be off");
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	Loader::Program       program {};
	SetupLinker(linker, program, dynamic_info);
	Loader::SymbolRecord out {};
	linker.Resolve(U"missingFunc#lib-id#module-id", Loader::SymbolType::Func, &program, &out, nullptr);
	Expect(out.vaddr == 0, "policy denied leaves zero");
	// Confirm pure validator agrees for the same request shape.
	Emulator::Validation::ImportResolveRequest req {};
	req.type                            = Loader::SymbolType::Func;
	req.has_export                      = false;
	req.missing_function_import_enabled = false;
	req.identity                        = "missingFunc";
	auto decision                       = Emulator::Validation::ClassifyImportResolution(req);
	Expect(decision.outcome == Emulator::Validation::ImportResolutionOutcome::StrictUnresolvedFunction, "strict missing Func outcome");
	ExpectCode(decision.validation, "policy_denied");

	req.type = Loader::SymbolType::Object;
	decision = Emulator::Validation::ClassifyImportResolution(req);
	Expect(decision.outcome == Emulator::Validation::ImportResolutionOutcome::UnresolvedNonFunction, "missing Object outcome");
	ExpectCode(decision.validation, "policy_denied");
	return 0;
}

// --- Guest: real LoadProgram path validation ---

int GuestValid()
{
	// Valid path shape only (no open of real guest binary).
	Emulator::Validation::GuestExecutableRequest req {};
	req.root_path = "/sanitized/path/to/module.prx";
	// LoadProgram also validates path then opens ELF — empty open would EXIT later.
	// Prove the shipped LoadProgram validation entry by empty-path fail below;
	// valid shape is accepted by ValidateGuestExecutable which LoadProgram calls first.
	Expect(Emulator::Validation::ValidateGuestExecutable(req).Ok(), "path shape ok");
	return 0;
}

int GuestMalformed()
{
	// LoadProgram must fail validation on empty path (process halt).
	Loader::RuntimeLinker linker;
	(void)linker.LoadProgram(U"");
	Die("empty path must EXIT via guest validation");
}

int GuestUnsupported()
{
	Loader::RuntimeLinker linker;
	(void)linker.LoadProgram(U"/tmp/../escape/module.prx");
	Die("dotdot path must EXIT via guest validation");
}

// --- Module: real LoadProgram basename validation for .prx ---
// LoadProgram uses FilenameWithoutDirectory().FilenameWithoutExtension() only
// (no fabricated versions). Failures EXIT with module metadata validation.

int ModuleValid()
{
	// Same function LoadProgram calls, with a basename it would extract from libc.prx.
	Emulator::Validation::ModuleMetadataRequest req {};
	req.name = "libc";
	Expect(Emulator::Validation::ValidateModuleMetadata(req).Ok(), "module ok");
	return 0;
}

int ModuleMalformed()
{
	// Path ".prx" / "sanitized/.prx" → empty basename after strip → malformed → EXIT 65.
	Loader::RuntimeLinker linker;
	(void)linker.LoadProgram(U"/sanitized/.prx");
	Die("empty module basename must EXIT via LoadProgram module validation");
}

int ModuleUnsupported()
{
	// Basename longer than 64 → unsupported → EXIT 65.
	std::string           long_name(70, 'm');
	const auto            path = String::FromUtf8(("/sanitized/" + long_name + ".prx").c_str());
	Loader::RuntimeLinker linker;
	(void)linker.LoadProgram(path);
	Die("long module basename must EXIT via LoadProgram module validation");
}

// --- Game-run: same ValidateGameRunRequest used by kyty_mount ---

int GameRunValid()
{
	Emulator::Validation::GameRunRequest req {};
	req.guest_root                = "/sanitized/guest";
	req.bringup_env_present       = false;
	req.allow_diagnostic_override = false;
	Expect(Emulator::Validation::ValidateGameRunRequest(req).Ok(), "game run ok");
	return 0;
}

int GameRunMalformed()
{
	Emulator::Validation::GameRunRequest req {};
	req.guest_root = "";
	ExpectCode(Emulator::Validation::ValidateGameRunRequest(req), "malformed");
	return 0;
}

int GameRunUnsupported()
{
	Emulator::Validation::GameRunRequest req {};
	req.guest_root                     = "/sanitized/guest";
	req.removed_permissive_env_present = true;
	ExpectCode(Emulator::Validation::ValidateGameRunRequest(req), "unsupported");
	return 0;
}

int GameRunPolicyDenied()
{
	Emulator::Validation::GameRunRequest req {};
	req.guest_root                = "/sanitized/guest";
	req.bringup_env_present       = true;
	req.allow_diagnostic_override = false;
	ExpectCode(Emulator::Validation::ValidateGameRunRequest(req), "policy_denied");
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: kyty_domain_validation_integration <scenario>\n");
		return 125;
	}

	// Optional BringUp init from env (fail-closed when invalid) — process isolation sets env.
	Core::BringUp::ConfigError berr {};
	const bool                 bringup_ok = Core::BringUp::InitializeFromEnvironment(&berr);
	(void)bringup_ok;

	const std::string scenario(argv[1]);
	const bool needs_host = (scenario == "import_valid" || scenario == "import_malformed" || scenario == "import_unsupported" ||
	                         scenario == "import_policy_denied" || scenario == "guest_malformed" || scenario == "guest_unsupported" ||
	                         scenario == "module_malformed" || scenario == "module_unsupported");

	if (needs_host)
	{
		if (InitHost(argc, argv) != 0)
		{
			return 125;
		}
	}

	if (scenario == "config_valid") return ConfigValid();
	if (scenario == "config_malformed") return ConfigMalformed();
	if (scenario == "config_empty_mode") return ConfigEmptyMode();
	if (scenario == "config_empty_features") return ConfigEmptyFeatures();
	if (scenario == "config_unsupported") return ConfigUnsupported();
	if (scenario == "config_policy_denied") return ConfigPolicyDenied();
	if (scenario == "agent_valid") return AgentValid();
	if (scenario == "agent_malformed") return AgentMalformed();
	if (scenario == "agent_unsupported") return AgentUnsupported();
	if (scenario == "agent_policy_denied") return AgentPolicyDenied();
	if (scenario == "import_valid") return ImportValid();
	if (scenario == "import_malformed") return ImportMalformed();
	if (scenario == "import_unsupported") return ImportUnsupported();
	if (scenario == "import_policy_denied") return ImportPolicyDenied();
	if (scenario == "guest_valid") return GuestValid();
	if (scenario == "guest_malformed") return GuestMalformed();
	if (scenario == "guest_unsupported") return GuestUnsupported();
	if (scenario == "module_valid") return ModuleValid();
	if (scenario == "module_malformed") return ModuleMalformed();
	if (scenario == "module_unsupported") return ModuleUnsupported();
	if (scenario == "gamerun_valid") return GameRunValid();
	if (scenario == "gamerun_malformed") return GameRunMalformed();
	if (scenario == "gamerun_unsupported") return GameRunUnsupported();
	if (scenario == "gamerun_policy_denied") return GameRunPolicyDenied();

	std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
	return 125;
}
