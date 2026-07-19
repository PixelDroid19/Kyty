#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Kyty;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

int InitializeBringUp()
{
	Core::BringUp::ConfigError error {};
	if (!Core::BringUp::InitializeFromEnvironment(&error))
	{
		std::fprintf(stderr, "bring-up configuration error: %s\n", error.message);
		return 125;
	}
	return 0;
}

int InitializeHostThread(int argc, char** argv)
{
	// Minimal host stack used by the emulator path: Core (fail-closed BringUp
	// already loaded above), Log (printf/GetDirection), Threads (mutexes).
	auto* subsystems = Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(argc, argv);
	auto* core    = Core::CoreSubsystem::Instance();
	auto* log     = Log::LogSubsystem::Instance();
	auto* threads = Core::ThreadsSubsystem::Instance();
	// Config is required by Log::Init (PrintfDirection).
	auto* config = Config::ConfigSubsystem::Instance();
	subsystems->Add(core, {});
	subsystems->Add(config, {core});
	subsystems->Add(threads, {core});
	subsystems->Add(log, {core, config, threads});
	if (!subsystems->InitAll(false))
	{
		std::fprintf(stderr, "host subsystem init failed: %s\n", subsystems->GetFailMsg());
		return 125;
	}

	Libs::LibKernel::PthreadInitSelfForMainThread();
	return 0;
}

void AddSyntheticSymbol(Loader::RuntimeLinker& linker, const Loader::SymbolResolve& sr, uint64_t vaddr)
{
	if (auto* symbols = linker.Symbols())
	{
		symbols->Add(sr, vaddr);
	}
}

int ScenarioStrictNotImplemented()
{
	EXIT_NOT_IMPLEMENTED(true);
	return 1;
}

int ScenarioUnsafeNotImplemented()
{
	// Macro path (one site).
	EXIT_NOT_IMPLEMENTED(true);
	const auto diagnostics = Core::BringUp::GetDiagnostics();
	Expect(diagnostics.total_continuations == 1, "continuation was not counted");
	Expect(diagnostics.continues_by_feature[static_cast<uint32_t>(Core::BringUp::Feature::NotImplemented)] >= 1,
	       "per-feature continue counter");

	// Quiet continue + dedupe: same identity/file/line twice via Report (shipped API).
	const char* id = "quiet-continue-site";
	const char* file = "BringUpIntegration.cpp";
	const int   line = 1;
	const auto  d1 =
	    Core::BringUp::Report(Core::BringUp::Feature::NotImplemented, Core::BringUp::Subsystem::Other, id, file, line);
	const auto d2 =
	    Core::BringUp::Report(Core::BringUp::Feature::NotImplemented, Core::BringUp::Subsystem::Other, id, file, line);
	Expect(d1 == Core::BringUp::Decision::Continue && d2 == Core::BringUp::Decision::Continue, "both continue");
	const auto again = Core::BringUp::GetDiagnostics();
	Expect(again.total_continuations == 3, "macro + two reports counted");
	// At least the quiet-continue site is a single unique entry (macro is a second site).
	Expect(again.unique_sites == 2, "macro site + one quiet-continue site");
	return 0;
}

int ScenarioDisabledSubsystem()
{
	const auto decision = Core::BringUp::Report(Core::BringUp::Feature::NotImplemented,
	                                            Core::BringUp::Subsystem::Graphics, "disabled-subsystem", __FILE__,
	                                            __LINE__);
	Expect(decision == Core::BringUp::Decision::Halt, "disabled subsystem must halt");
	const auto diagnostics = Core::BringUp::GetDiagnostics();
	Expect(diagnostics.total_continuations == 0, "continuation happened in disabled subsystem");
	Expect(diagnostics.total_halts >= 1, "halt must be counted");
	Expect(diagnostics.halts_by_feature[static_cast<uint32_t>(Core::BringUp::Feature::NotImplemented)] >= 1,
	       "per-feature halt counter");
	return 0;
}

int ScenarioBurstBreaker()
{
	for (int i = 0; i != 4; ++i)
	{
		const auto decision = Core::BringUp::Report(Core::BringUp::Feature::NotImplemented,
		                                            Core::BringUp::Subsystem::Core, "burst-breaker", __FILE__, __LINE__);
		if (decision == Core::BringUp::Decision::Halt)
		{
			// Match production halt path for process-isolated CTest.
			Core::dbg_exit(321);
		}
	}
	Die("burst breaker did not halt");
}

int ScenarioSlowRepeats()
{
	for (int i = 0; i != 3; ++i)
	{
		const auto decision = Core::BringUp::Report(Core::BringUp::Feature::NotImplemented,
		                                            Core::BringUp::Subsystem::Core, "slow-repeats", __FILE__, __LINE__);
		Expect(decision == Core::BringUp::Decision::Continue, "unexpected breaker in slow repeats");
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
	const auto diagnostics = Core::BringUp::GetDiagnostics();
	Expect(diagnostics.total_continuations == 3, "unexpected continuation count");
	return 0;
}

int ScenarioMissingFunctionImport()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	Loader::SymbolRecord first {};
	Loader::SymbolRecord second {};
	linker.Resolve(U"sanitizedNid#lib-id#module-id", Loader::SymbolType::Func, &program, &first, nullptr);
	linker.Resolve(U"sanitizedNid#lib-id#module-id", Loader::SymbolType::Func, &program, &second, nullptr);

	const auto diagnostics = linker.GetMissingImportDiagnostics();
	Expect(first.vaddr != 0, "first import was not stubbed");
	Expect(second.vaddr != 0, "second import was not stubbed");
	Expect(first.vaddr == second.vaddr, "stub address changed for same identity");
	Expect(diagnostics.unique_symbols == 1, "unexpected unique symbol count");
	Expect(diagnostics.used == 1, "unexpected used slot count");
	// Duplicate resolve must not consume capacity; attempts stay separate from used.
	Expect(diagnostics.resolution_attempts == 2, "resolution_attempts must count both resolves");
	Expect(diagnostics.resolution_attempts > diagnostics.used, "attempts must exceed used on duplicates");
	Expect(diagnostics.table_capacity == 2048, "expected static stub capacity");
	return 0;
}

// BURST_LIMIT=1 (see RunBringUpCase): first NEW slot may Report once; a second
// Resolve of the same identity must reuse the registry address without re-gating
// through BringUp (which would Halt and EXIT under burst=1).
int ScenarioMissingImportBurstReuse()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	Loader::SymbolRecord first {};
	Loader::SymbolRecord second {};
	Loader::SymbolRecord third {};
	linker.Resolve(U"sanitizedBurst#lib-id#module-id", Loader::SymbolType::Func, &program, &first, nullptr);
	linker.Resolve(U"sanitizedBurst#lib-id#module-id", Loader::SymbolType::Func, &program, &second, nullptr);
	linker.Resolve(U"sanitizedBurst#lib-id#module-id", Loader::SymbolType::Func, &program, &third, nullptr);

	const auto diagnostics = linker.GetMissingImportDiagnostics();
	Expect(first.vaddr != 0, "first burst-reuse import was not stubbed");
	Expect(second.vaddr != 0, "duplicate under BURST_LIMIT=1 must not EXIT");
	Expect(third.vaddr != 0, "third resolve under BURST_LIMIT=1 must not EXIT");
	Expect(first.vaddr == second.vaddr, "burst reuse must keep stable address");
	Expect(second.vaddr == third.vaddr, "third resolve must keep stable address");
	Expect(diagnostics.used == 1, "used must stay one under burst reuse");
	Expect(diagnostics.unique_symbols == 1, "unique must stay one under burst reuse");
	Expect(diagnostics.resolution_attempts == 3, "each resolve counts an attempt");
	return 0;
}

int ScenarioRealExportWins()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	// Resolve remaps library/module *ids* to LibraryId/ModuleId names.
	Loader::SymbolResolve sr {};
	sr.name                 = U"sanitizedNid";
	sr.library              = U"libIntegration";
	sr.library_version      = 1;
	sr.module               = U"moduleIntegration";
	sr.module_version_major = 1;
	sr.module_version_minor = 0;
	sr.type                 = Loader::SymbolType::Func;

	AddSyntheticSymbol(linker, sr, 0x123456780ULL);

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	Loader::SymbolRecord result {};
	linker.Resolve(U"sanitizedNid#lib-id#module-id", Loader::SymbolType::Func, &program, &result, nullptr);
	const auto diagnostics = linker.GetMissingImportDiagnostics();

	Expect(result.vaddr == 0x123456780ULL, "real export did not win");
	Expect(diagnostics.used == 0, "unexpected stub usage with real export");
	Expect(diagnostics.unique_symbols == 0, "unexpected symbol usage with real export");
	Expect(diagnostics.resolution_attempts == 0, "export path must not count missing-import attempts");
	return 0;
}

int ScenarioNonFunctionImportsStayStrict()
{
	// Under unsafe + missing_function_import, non-Func imports remain unresolved
	// exactly as in strict mode and never receive callable stubs.
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	Loader::SymbolRecord out_object {};
	linker.Resolve(U"sanitizedObj#lib-id#module-id", Loader::SymbolType::Object, &program, &out_object, nullptr);
	Expect(out_object.vaddr == 0, "non-Func import must remain unresolved");
	return 0;
}

int ScenarioConcurrentDeduplication()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	const auto               thread_count = 16u;
	const char*              identity     = "sanitizedNid#lib-id#module-id";
	std::vector<std::thread> workers;
	std::vector<uint64_t>    addresses(thread_count);
	std::atomic<int>         failures {0};
	for (unsigned i = 0; i < thread_count; ++i)
	{
		workers.emplace_back([&linker, &program, &addresses, i, &failures, identity]() {
			Loader::SymbolRecord result {};
			linker.Resolve(String::FromPrintf("%s", identity), Loader::SymbolType::Func, &program, &result, nullptr);
			if (result.vaddr == 0)
			{
				failures.fetch_add(1, std::memory_order_relaxed);
			} else
			{
				addresses[i] = result.vaddr;
			}
		});
	}
	for (auto& w: workers)
	{
		w.join();
	}
	Expect(failures.load(std::memory_order_relaxed) == 0, "concurrent resolve produced zero address");
	for (unsigned i = 1; i < thread_count; ++i)
	{
		Expect(addresses[i] == addresses[0], "same identity must share stub address");
	}
	const auto diagnostics = linker.GetMissingImportDiagnostics();
	Expect(diagnostics.unique_symbols == 1, "dedup failed");
	Expect(diagnostics.used == 1, "used slots must match unique after dedupe");
	Expect(diagnostics.resolution_attempts == thread_count,
	       "each concurrent resolve must count as a resolution attempt");
	Expect(diagnostics.resolution_attempts > diagnostics.used, "attempts must exceed used under concurrency");
	return 0;
}

int ScenarioConcurrentBurstAccounting()
{
	constexpr unsigned kThreadCount = 64;
	std::atomic<unsigned> ready {0};
	std::atomic_bool      start {false};
	std::atomic<unsigned> continues {0};
	std::atomic<unsigned> halts {0};
	std::vector<std::thread> workers;
	workers.reserve(kThreadCount);

	for (unsigned i = 0; i < kThreadCount; ++i)
	{
		workers.emplace_back([&] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			const auto decision =
			    Core::BringUp::Report(Core::BringUp::Feature::NotImplemented, Core::BringUp::Subsystem::Core,
			                          "concurrent-burst-accounting", __FILE__, __LINE__);
			if (decision == Core::BringUp::Decision::Continue)
			{
				continues.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			halts.fetch_add(1, std::memory_order_relaxed);
		});
	}

	while (ready.load(std::memory_order_acquire) != kThreadCount)
	{
		std::this_thread::yield();
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers)
	{
		worker.join();
	}

	Expect(continues.load(std::memory_order_relaxed) == 1, "burst limit must admit exactly one concurrent report");
	Expect(halts.load(std::memory_order_relaxed) == kThreadCount - 1,
	       "burst limit must reject every concurrent report after the first");
	const auto diagnostics = Core::BringUp::GetDiagnostics();
	Expect(diagnostics.total_continuations == 1, "concurrent continuation diagnostics mismatch");
	Expect(diagnostics.total_halts == kThreadCount - 1, "concurrent halt diagnostics mismatch");
	return 0;
}

int ScenarioLongCanonicalIdentitiesStayDistinct()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	const std::string common_prefix(220, 'x');
	const auto        first_name  = String::FromPrintf("%sA#lib-id#module-id", common_prefix.c_str());
	const auto        second_name = String::FromPrintf("%sB#lib-id#module-id", common_prefix.c_str());
	Loader::SymbolRecord first {};
	Loader::SymbolRecord second {};
	linker.Resolve(first_name, Loader::SymbolType::Func, &program, &first, nullptr);
	linker.Resolve(second_name, Loader::SymbolType::Func, &program, &second, nullptr);

	const auto diagnostics = linker.GetMissingImportDiagnostics();
	Expect(first.vaddr != 0 && second.vaddr != 0, "long identities must receive non-zero stubs");
	Expect(first.vaddr != second.vaddr, "distinct full canonical identities must not alias");
	Expect(diagnostics.unique_symbols == 2, "long canonical identities must remain distinct");
	Expect(diagnostics.used == 2, "long canonical identities must reserve separate slots");
	return 0;
}

int ScenarioStubCapacityExhaustion()
{
	Loader::RuntimeLinker linker;
	Loader::DynamicInfo   dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program program {};
	program.rt           = &linker;
	program.dynamic_info = &dynamic_info;

	Loader::SymbolRecord result {};
	for (int i = 0; i < 2048; ++i)
	{
		const auto name = String::FromPrintf("sanitizedNid%d#lib-id#module-id", i);
		linker.Resolve(name, Loader::SymbolType::Func, &program, &result, nullptr);
		Expect(result.vaddr != 0, "assign within capacity");
	}
	const auto after_full = linker.GetMissingImportDiagnostics();
	Expect(after_full.used == 2048, "table not full");

	Loader::SymbolRecord overflow {};
	linker.Resolve(U"sanitizedOverflow#lib-id#module-id", Loader::SymbolType::Func, &program, &overflow, nullptr);
	Die("capacity exhaust should have aborted");
}

int ScenarioGraphicsFeatureDisabled()
{
	const auto decision =
	    Core::BringUp::Report(Core::BringUp::Feature::GraphicsPermissive, Core::BringUp::Subsystem::Graphics,
	                          "graphics-feature-disabled", __FILE__, __LINE__);
	Expect(decision == Core::BringUp::Decision::Halt, "disabled graphics feature must halt");
	return 0;
}

int ScenarioRemovedStubFlagRejected()
{
	// InitializeBringUp must have rejected already; reach here only on policy bug.
	Die("KYTY_STUB_MISSING must fail at InitializeFromEnvironment");
}

int ScenarioRemovedGraphicsFlagRejected()
{
	Die("KYTY_GFX_PERMISSIVE must fail at InitializeFromEnvironment");
}

int ScenarioEmptyModeRejected()
{
	Die("empty KYTY_BRINGUP_MODE must fail at InitializeFromEnvironment");
}

int ScenarioEmptyFeaturesRejected()
{
	Die("empty KYTY_BRINGUP_FEATURES must fail at InitializeFromEnvironment");
}

int ScenarioEmptyBurstLimitRejected()
{
	Die("empty KYTY_BRINGUP_BURST_LIMIT must fail at InitializeFromEnvironment");
}

int ScenarioAgentDiagnostics()
{
	// Produce a live continue so counters are non-zero in the snapshot.
	(void)Core::BringUp::Report(Core::BringUp::Feature::NotImplemented, Core::BringUp::Subsystem::Graphics,
	                            "agent-diag-site", __FILE__, __LINE__);
	const auto config      = Core::BringUp::GetConfig();
	const auto diagnostics = Core::BringUp::GetDiagnostics();
	const auto imports     = Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics();
	const auto load_plan   = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	const std::string result = Kyty::Emulator::Agent::BuildDiagnosticsResult(config, diagnostics, imports, load_plan);
	Expect(result.find("\"protocol_version\":4") != std::string::npos, "protocol version 4");
	Expect(result.find("\"mode\":\"unsafe\"") != std::string::npos, "mode unsafe");
	Expect(result.find("\"not_implemented\"") != std::string::npos, "enabled features list");
	Expect(result.find("\"enabled_subsystems\"") != std::string::npos, "enabled subsystems field");
	Expect(result.find("\"loader\"") != std::string::npos || result.find("\"graphics\"") != std::string::npos,
	       "subsystem listed");
	Expect(result.find("\"total_halts\"") != std::string::npos, "total_halts field");
	Expect(result.find("\"config_rejections\"") != std::string::npos, "config_rejections field");
	Expect(result.find("\"continues_by_feature\"") != std::string::npos, "continues_by_feature field");
	Expect(result.find("\"halts_by_subsystem\"") != std::string::npos, "halts_by_subsystem field");
	Expect(result.find("\"missing_imports\"") != std::string::npos, "missing_imports block");
	Expect(result.find("\"resolution_attempts\"") != std::string::npos, "resolution_attempts field");
	Expect(result.find("\"table_capacity\"") != std::string::npos, "table_capacity field");
	Expect(result.find("\"load_plan\"") != std::string::npos, "load_plan field");
	Expect(result.find("\"discovery_enabled\"") != std::string::npos, "load_plan discovery_enabled");
	Expect(result.find("STUB_MISSING") == std::string::npos, "no legacy STUB_MISSING key");
	Expect(result.find("GFX_PERMISSIVE") == std::string::npos, "no legacy GFX_PERMISSIVE key");
	Expect(diagnostics.total_continuations >= 1, "live continue in snapshot");
	Expect(diagnostics.config.mode == Core::BringUp::Mode::Unsafe, "snapshot embeds effective mode");
	Expect(imports.table_capacity == 2048, "import diagnostics expose capacity");
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: kyty_bringup_integration <scenario>\n");
		return 125;
	}

	const int init = InitializeBringUp();
	if (init != 0)
	{
		return init;
	}

	const int init_thread = InitializeHostThread(argc, argv);
	if (init_thread != 0)
	{
		return init_thread;
	}

	const std::string scenario(argv[1]);

	if (scenario == "strict_not_implemented")
	{
		return ScenarioStrictNotImplemented();
	}
	if (scenario == "unsafe_not_implemented")
	{
		return ScenarioUnsafeNotImplemented();
	}
	if (scenario == "disabled_subsystem")
	{
		return ScenarioDisabledSubsystem();
	}
	if (scenario == "invalid_configuration")
	{
		// InitializeBringUp must have rejected already (exit 125). Reaching
		// here means unknown/invalid env was wrongly accepted.
		Die("invalid configuration must fail at InitializeFromEnvironment");
	}
	if (scenario == "burst_breaker")
	{
		return ScenarioBurstBreaker();
	}
	if (scenario == "slow_repeats")
	{
		return ScenarioSlowRepeats();
	}
	if (scenario == "missing_function_import")
	{
		return ScenarioMissingFunctionImport();
	}
	if (scenario == "missing_import_burst_reuse")
	{
		return ScenarioMissingImportBurstReuse();
	}
	if (scenario == "real_export_wins")
	{
		return ScenarioRealExportWins();
	}
	if (scenario == "non_function_imports_stay_strict")
	{
		return ScenarioNonFunctionImportsStayStrict();
	}
	if (scenario == "concurrent_deduplication")
	{
		return ScenarioConcurrentDeduplication();
	}
	if (scenario == "concurrent_burst_accounting")
	{
		return ScenarioConcurrentBurstAccounting();
	}
	if (scenario == "long_canonical_identities_stay_distinct")
	{
		return ScenarioLongCanonicalIdentitiesStayDistinct();
	}
	if (scenario == "stub_capacity_exhaustion")
	{
		return ScenarioStubCapacityExhaustion();
	}
	if (scenario == "graphics_feature_disabled")
	{
		return ScenarioGraphicsFeatureDisabled();
	}
	if (scenario == "agent_diagnostics")
	{
		return ScenarioAgentDiagnostics();
	}
	if (scenario == "removed_stub_flag_rejected")
	{
		return ScenarioRemovedStubFlagRejected();
	}
	if (scenario == "removed_graphics_flag_rejected")
	{
		return ScenarioRemovedGraphicsFlagRejected();
	}
	if (scenario == "empty_mode_rejected")
	{
		return ScenarioEmptyModeRejected();
	}
	if (scenario == "empty_features_rejected")
	{
		return ScenarioEmptyFeaturesRejected();
	}
	if (scenario == "empty_burst_limit_rejected")
	{
		return ScenarioEmptyBurstLimitRejected();
	}

	std::fprintf(stderr, "unknown integration scenario: %s\n", argv[1]);
	return 125;
}
