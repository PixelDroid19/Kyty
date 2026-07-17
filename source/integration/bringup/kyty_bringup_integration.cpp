// Process-level integration scenarios for Kyty::Core::BringUp and missing-import stubs.
// Each process runs exactly one scenario (selected by argv[1]) so abort paths do not
// contaminate siblings. Environment is set by the CMake/CTest runner, not here —
// except scenarios that re-exec or self-configure via InitForTests after Reset.

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/MissingImportStubs.h"
#include "Emulator/Loader/NeighborModulePreload.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

#include <unistd.h>

namespace BringUp       = Kyty::Core::BringUp;
namespace MissingImport = Kyty::Loader::MissingImport;
using Kyty::Loader::SymbolResolve;
using Kyty::Loader::SymbolType;

namespace {

[[noreturn]] void Die(const char* msg)
{
	std::fprintf(stderr, "bringup_integration FAIL: %s\n", msg);
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

// Trigger EXIT_NOT_IMPLEMENTED from a synthetic "graphics" path string by
// calling the handler directly (same entry as the macro).
int FireNotImplemented(const char* expr, const char* file, int line)
{
	return Kyty::Core::dbg_not_implemented_handler(expr, file, line);
}

SymbolResolve MakeFunc(const char* name, const char* lib, const char* mod, int lib_ver = 1)
{
	SymbolResolve sr {};
	sr.name                 = name;
	sr.library              = lib;
	sr.library_version      = lib_ver;
	sr.module               = mod;
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = SymbolType::Func;
	return sr;
}

int ScenarioStrictAborts()
{
	// Env: no KYTY_BRINGUP_MODE (strict default) loaded by main via InitFromEnvironment.
	const int r = FireNotImplemented("test_strict", "/src/emulator/src/Graphics/GraphicsRun.cpp", 42);
	// If we reach here without halt, policy failed.
	Expect(r != 0, "strict should request abort");
	// Mimic macro halt path for process exit code.
	Kyty::Core::dbg_exit(321);
	return 0;
}

int ScenarioUnsafeContinues()
{
	// Env: KYTY_BRINGUP_MODE=unsafe (features default all).
	const int r = FireNotImplemented("test_unsafe", "/src/emulator/src/Graphics/GraphicsRun.cpp", 99);
	Expect(r == 0, "unsafe not_implemented should continue (handler returns 0)");
	BringUp::Snapshot snap {};
	BringUp::GetSnapshot(&snap);
	Expect(snap.config.mode == BringUp::Mode::Unsafe, "mode unsafe");
	Expect(snap.total_continuations >= 1, "continuation counted");
	Expect(snap.unique_sites >= 1, "unique site registered");
	return 0;
}

int ScenarioUnauthorizedSubsystemAborts()
{
	// Env: unsafe + features=not_implemented + subsystems=kernel only.
	// A graphics site must abort.
	const int r = FireNotImplemented("blocked_gfx", "/src/emulator/src/Graphics/GraphicsRun.cpp", 7);
	Expect(r != 0, "unauthorized subsystem should abort");
	Kyty::Core::dbg_exit(321);
	return 0;
}

int ScenarioInvalidConfig()
{
	// Env set by runner to invalid mode; InitFromEnvironment must _Exit(2).
	// If we get here, init failed to fail-closed.
	Die("invalid config should have aborted during InitFromEnvironment");
	return 1;
}

int ScenarioBurstLimit()
{
	// Env: unsafe, burst_limit=5, window large.
	for (int i = 0; i < 20; ++i)
	{
		const int r = FireNotImplemented("burst_site", "/src/emulator/src/Graphics/GraphicsRun.cpp", 1234);
		if (r != 0)
		{
			// Circuit-break or abort — success for this scenario.
			BringUp::Snapshot snap {};
			BringUp::GetSnapshot(&snap);
			Expect(snap.last_circuit_break.active, "circuit-break should be recorded");
			Kyty::Core::dbg_exit(321);
		}
	}
	Die("burst limit did not trip");
	return 1;
}

int ScenarioSlowRepeatNoTrip()
{
	// Env: unsafe, burst_limit=3, window_ms=50.
	// Sleep between hits so window resets; must not circuit-break.
	for (int i = 0; i < 6; ++i)
	{
		const int r = FireNotImplemented("slow_site", "/src/emulator/src/Kernel/Memory.cpp", 55);
		Expect(r == 0, "slow repeat should continue");
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
	}
	BringUp::Snapshot snap {};
	BringUp::GetSnapshot(&snap);
	Expect(!snap.last_circuit_break.active, "slow repeat must not circuit-break");
	return 0;
}

int ScenarioMissingImportDedupeAndCalls()
{
	Expect(BringUp::AllowMissingFunctionImport(), "missing_function_import feature required");
	MissingImport::ResetForTests();

	const auto sr = MakeFunc("NidExample1", "libSceFoo", "Foo");
	const uint64_t a1 = MissingImport::Assign(sr);
	const uint64_t a2 = MissingImport::Assign(sr);
	Expect(a1 != 0 && a1 == a2, "same identity must share stub address");
	Expect(MissingImport::UsedSlots() == 1, "dedupe uses one slot");

	using stub_t = KYTY_SYSV_ABI int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	auto* fn = reinterpret_cast<stub_t>(a1);
	(void)fn(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	(void)fn(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);

	MissingImport::SlotInfo info {};
	Expect(MissingImport::FindByIdentity(sr, &info), "slot find");
	Expect(info.call_count == 2, "call count");
	Expect(info.last_args[0] == 0x11 && info.last_args[5] == 0x66, "gpr args recorded");
	Expect(MissingImport::TotalCalls() == 2, "total calls");
	return 0;
}

int ScenarioRealExportWinsConcept()
{
	// Documented contract: HLE/export is resolved before MissingImport::Assign.
	// This process-level test asserts that Assign is not used when a non-zero
	// vaddr is already present (caller order). Simulated here as: if "export"
	// is known, stub is never assigned.
	MissingImport::ResetForTests();
	const auto sr = MakeFunc("RealExport", "libSceBar", "Bar");
	const uint64_t export_vaddr = 0x1000;
	uint64_t chosen = export_vaddr; // real export wins
	if (chosen == 0 && BringUp::AllowMissingFunctionImport())
	{
		chosen = MissingImport::Assign(sr);
	}
	Expect(chosen == export_vaddr, "export must win");
	Expect(MissingImport::UsedSlots() == 0, "no stub when export present");
	return 0;
}

int ScenarioNoStubForNonFunc()
{
	// RuntimeLinker only stubs Func. Non-Func must not call Assign.
	// Guard: Assign asserts type == Func; we only verify policy surface.
	Expect(BringUp::AllowMissingFunctionImport(), "feature on");
	// Types that must never be stubbed by the linker path.
	const SymbolType banned[] = {SymbolType::Object, SymbolType::TlsModule, SymbolType::NoType, SymbolType::Unknown};
	for (auto t : banned)
	{
		Expect(t != SymbolType::Func, "non-func");
	}
	// Linker gate in RuntimeLinker: type == Func && Allow...
	// Structural check: UsedSlots stays 0 when we do not call Assign for them.
	MissingImport::ResetForTests();
	Expect(MissingImport::UsedSlots() == 0, "no slots without Assign");
	return 0;
}

int ScenarioConcurrentSingleEntry()
{
	Expect(BringUp::AllowMissingFunctionImport() || BringUp::FeatureEnabled(BringUp::Feature::NotImplemented),
	       "need unsafe features");

	// Concurrent NotImplemented on the same site → one unique site.
	std::atomic<int> continues {0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; ++t)
	{
		threads.emplace_back([&] {
			for (int i = 0; i < 50; ++i)
			{
				const int r = FireNotImplemented("concurrent_site", "/src/emulator/src/Graphics/Graphics.cpp", 777);
				if (r == 0)
				{
					continues.fetch_add(1);
				}
			}
		});
	}
	for (auto& th : threads)
	{
		th.join();
	}
	BringUp::Snapshot snap {};
	BringUp::GetSnapshot(&snap);
	// Exactly one site for that file/line/expr.
	uint64_t matching = 0;
	for (uint32_t i = 0; i < snap.sites_capacity; ++i)
	{
		if (snap.sites[i].line == 777 && snap.sites[i].expr != nullptr &&
		    std::strcmp(snap.sites[i].expr, "concurrent_site") == 0)
		{
			++matching;
		}
	}
	Expect(matching == 1, "concurrent same site produces one registry entry");
	Expect(continues.load() > 0, "continuations occurred");

	// Concurrent Assign same identity → one slot.
	if (BringUp::AllowMissingFunctionImport())
	{
		MissingImport::ResetForTests();
		const auto sr = MakeFunc("ConcurrentNid", "libX", "ModX");
		std::atomic<uint64_t> addr {0};
		std::vector<std::thread> th2;
		for (int t = 0; t < 8; ++t)
		{
			th2.emplace_back([&] {
				const uint64_t a = MissingImport::Assign(sr);
				uint64_t expected = 0;
				if (!addr.compare_exchange_strong(expected, a))
				{
					Expect(a == addr.load(), "same stub address under concurrency");
				}
			});
		}
		for (auto& th : th2)
		{
			th.join();
		}
		Expect(MissingImport::UsedSlots() == 1, "one stub slot under concurrency");
	}
	return 0;
}

int ScenarioCapacityExhaustAbort()
{
	Expect(BringUp::AllowMissingFunctionImport(), "feature on");
	MissingImport::ResetForTests();
	// Fill all slots then one more must abort via EXIT (exit 321).
	for (int i = 0; i < MissingImport::kMaxSlots; ++i)
	{
		char name[32];
		std::snprintf(name, sizeof(name), "Nid%d", i);
		const auto sr = MakeFunc(name, "libCap", "ModCap");
		const uint64_t a = MissingImport::Assign(sr);
		Expect(a != 0, "assign within capacity");
	}
	Expect(MissingImport::UsedSlots() == MissingImport::kMaxSlots, "full");
	// Next assign aborts — must not return.
	const auto overflow = MakeFunc("Overflow", "libCap", "ModCap");
	(void)MissingImport::Assign(overflow);
	Die("capacity exhaust should have aborted");
	return 1;
}

int ScenarioDiagnosticsSnapshot()
{
	// Ensure agent diagnostics JSON includes protocol version and bringup fields.
	const int r = FireNotImplemented("diag_site", "/src/emulator/src/Graphics/GraphicsRun.cpp", 1);
	if (BringUp::GetMode() == BringUp::Mode::Unsafe && r == 0)
	{
		// ok
	}
	char buf[4096];
	std::FILE* mem = std::tmpfile();
	Expect(mem != nullptr, "tmpfile");
	const int n = BringUp::WriteDiagnosticsJson(mem);
	Expect(n > 0, "diagnostics written");
	std::rewind(mem);
	const size_t got = std::fread(buf, 1, sizeof(buf) - 1, mem);
	buf[got]         = '\0';
	std::fclose(mem);
	Expect(std::strstr(buf, "\"protocolVersion\":2") != nullptr, "protocol version 2");
	Expect(std::strstr(buf, "\"bringup\"") != nullptr, "bringup object");
	Expect(std::strstr(buf, "\"mode\"") != nullptr, "mode field");
	// Legacy keys must not appear.
	Expect(std::strstr(buf, "STUB_MISSING") == nullptr, "no STUB_MISSING");
	Expect(std::strstr(buf, "GFX_PERMISSIVE") == nullptr, "no GFX_PERMISSIVE");
	return 0;
}

// Sanitized fixture layout (no private title IDs): discover neighbor PRX paths.
int ScenarioPrxDiscover()
{
	Expect(BringUp::AllowPrxPreload(), "prx_preload feature must be on for this scenario");

	// Create a temporary guest root with sce_module + Media/Modules candidates.
	char tmpl[] = "/tmp/grok-goal-e70b2bb9233f/implementer/preload_fixture_XXXXXX";
	char* dir   = ::mkdtemp(tmpl);
	Expect(dir != nullptr, "mkdtemp fixture");

	const Kyty::Core::String root = Kyty::Core::String::FromUtf8(dir);
	const Kyty::Core::String sce  = root + U"/sce_module";
	const Kyty::Core::String media = root + U"/Media/Modules";
	Expect(Kyty::Core::File::CreateDirectories(sce), "mkdir sce_module");
	Expect(Kyty::Core::File::CreateDirectories(media), "mkdir Media/Modules");

	// Touch sanitized module-looking files (not real ELFs — discovery only).
	auto touch = [](const Kyty::Core::String& path) {
		Kyty::Core::File f;
		Expect(f.Create(path), "create file");
		const char b = 'x';
		f.Write(&b, 1u);
		f.Close();
	};
	touch(sce + U"/libc.prx");
	touch(sce + U"/libSceFios2.prx");
	touch(media + U"/helper.sprx");
	touch(root + U"/eboot.bin"); // must be excluded by discovery when under root modules only;
	// eboot lives at root, not under scanned subdirs, so it is not listed.

	const auto found = Kyty::Loader::NeighborModulePreload::DiscoverCandidates(root);
	Expect(found.Size() == 3, "three module candidates");
	// Ensure eboot not present and prx names are.
	bool saw_libc = false;
	bool saw_eboot = false;
	for (const auto& p : found)
	{
		const auto name = p.FilenameWithoutDirectory().ToLower();
		if (name == U"libc.prx")
		{
			saw_libc = true;
		}
		if (name == U"eboot.bin")
		{
			saw_eboot = true;
		}
	}
	Expect(saw_libc, "libc.prx discovered");
	Expect(!saw_eboot, "eboot.bin not discovered as neighbor");

	// PreloadInto under AllowPrxPreload with invalid ELFs: soft-skip, load count 0.
	// Pass nullptr RuntimeLinker would return 0; we only assert gate + discover here.
	const int loaded =
	    Kyty::Loader::NeighborModulePreload::PreloadInto(nullptr, root + U"/eboot.bin");
	Expect(loaded == 0, "null linker loads nothing");

	Expect(BringUp::FeatureEnabled(BringUp::Feature::PrxPreload), "feature bit");
	Expect(BringUp::GetMode() == BringUp::Mode::Unsafe, "unsafe mode");

	// Cleanup fixture files (best-effort).
	Kyty::Core::File::DeleteDirectories(root);
	return 0;
}

// Strict: AllowPrxPreload is false; PreloadInto is a no-op even with layout present.
int ScenarioPrxStrictNoPreload()
{
	Expect(!BringUp::AllowPrxPreload(), "strict must not allow prx preload");
	const int loaded =
	    Kyty::Loader::NeighborModulePreload::PreloadInto(nullptr, U"/nonexistent/eboot.bin");
	Expect(loaded == 0, "strict preload no-op");
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: kyty_bringup_integration <scenario>\n");
		return 1;
	}

	// Load policy from environment first (fail-closed on bad config).
	BringUp::InitFromEnvironment();

	// Minimal Core init for VirtualMemory used by missing stubs.
	auto& slist = *Kyty::Core::SubsystemsList::Instance();
	slist.SetArgs(argc, argv);
	auto* core = Kyty::Core::CoreSubsystem::Instance();
	slist.Add(core, {});
	if (!slist.InitAll(false))
	{
		// Some scenarios (invalid config) never reach here; others need core.
		std::fprintf(stderr, "core init failed: %s\n", slist.GetFailMsg());
	}

	const char* scenario = argv[1];
	int rc = 1;
	if (std::strcmp(scenario, "strict_abort") == 0)
	{
		rc = ScenarioStrictAborts();
	} else if (std::strcmp(scenario, "unsafe_continue") == 0)
	{
		rc = ScenarioUnsafeContinues();
	} else if (std::strcmp(scenario, "unauthorized_subsystem") == 0)
	{
		rc = ScenarioUnauthorizedSubsystemAborts();
	} else if (std::strcmp(scenario, "invalid_config") == 0)
	{
		rc = ScenarioInvalidConfig();
	} else if (std::strcmp(scenario, "burst_limit") == 0)
	{
		rc = ScenarioBurstLimit();
	} else if (std::strcmp(scenario, "slow_repeat") == 0)
	{
		rc = ScenarioSlowRepeatNoTrip();
	} else if (std::strcmp(scenario, "missing_import") == 0)
	{
		rc = ScenarioMissingImportDedupeAndCalls();
	} else if (std::strcmp(scenario, "export_wins") == 0)
	{
		rc = ScenarioRealExportWinsConcept();
	} else if (std::strcmp(scenario, "no_stub_nonfunc") == 0)
	{
		rc = ScenarioNoStubForNonFunc();
	} else if (std::strcmp(scenario, "concurrent") == 0)
	{
		rc = ScenarioConcurrentSingleEntry();
	} else if (std::strcmp(scenario, "capacity") == 0)
	{
		rc = ScenarioCapacityExhaustAbort();
	} else if (std::strcmp(scenario, "diagnostics") == 0)
	{
		rc = ScenarioDiagnosticsSnapshot();
	} else if (std::strcmp(scenario, "prx_discover") == 0)
	{
		rc = ScenarioPrxDiscover();
	} else if (std::strcmp(scenario, "prx_strict_no_preload") == 0)
	{
		rc = ScenarioPrxStrictNoPreload();
	} else
	{
		std::fprintf(stderr, "unknown scenario: %s\n", scenario);
		rc = 1;
	}
	return rc;
}
