// Process-isolated loader boundary scenarios: GuestExecutableLocator,
// ModuleDiscovery, ModuleLoadPlan, ModuleLifecycleCoordinator.
// Fixtures use temporary sanitized package layouts only (no title IDs).

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/Protocol.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Kyty;

namespace Kyty::Loader {

class RuntimeLinkerIntegrationAccess
{
public:
	static Program* AttachSyntheticExportModule(RuntimeLinker* linker, const Core::String& file_name)
	{
		return linker->AttachSyntheticExportModule(file_name);
	}
};

} // namespace Kyty::Loader

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "loader integration failure: %s\n", message);
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

class FixtureCleanup
{
public:
	~FixtureCleanup()
	{
		for (auto it = m_paths.rbegin(); it != m_paths.rend(); ++it)
		{
			std::error_code ec;
			std::filesystem::remove_all(*it, ec);
		}
	}

	void Add(const Core::String& path) { m_paths.emplace_back(path.C_Str()); }

private:
	std::vector<std::filesystem::path> m_paths;
};

FixtureCleanup g_fixture_cleanup;

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
		std::fprintf(stderr, "host subsystem init failed: %s\n", subsystems->GetFailMsg());
		return 125;
	}
	Libs::LibKernel::PthreadInitSelfForMainThread();
	return 0;
}

// Minimal PS-style shared ELF header that satisfies Elf64::IsValid + IsShared.
// Not a full guest module — only used for plan-time probes and soft apply.
bool WriteMinimalSharedElf(const Core::String& path, uint8_t abi_version = 2)
{
	// Elf64_Ehdr is 64 bytes; e_phentsize must be sizeof(Elf64_Phdr)=56.
	unsigned char buf[64] = {};
	buf[0]                = 0x7f;
	buf[1]                = 'E';
	buf[2]                = 'L';
	buf[3]                = 'F';
	buf[4]                = 2; // ELFCLASS64
	buf[5]                = 1; // ELFDATA2LSB
	buf[6]                = 1; // EV_CURRENT
	buf[7]                = 9; // ELFOSABI_FREEBSD
	buf[8]                = abi_version;
	// e_type = ET_DYNAMIC (0xfe18) at offset 16, little-endian
	buf[16] = 0x18;
	buf[17] = 0xfe;
	// e_machine = EM_X86_64 (62)
	buf[18] = 62;
	buf[19] = 0;
	// e_version = 1
	buf[20] = 1;
	buf[21] = 0;
	buf[22] = 0;
	buf[23] = 0;
	// e_ehsize = 64 at offset 52
	buf[52] = 64;
	buf[53] = 0;
	// e_phentsize = 56 at offset 54
	buf[54] = 56;
	buf[55] = 0;
	// e_phnum = 0
	buf[56] = 0;
	buf[57] = 0;
	// e_shentsize = 0
	buf[58] = 0;
	buf[59] = 0;

	Core::File f;
	f.Create(path);
	if (f.IsInvalid())
	{
		return false;
	}
	uint32_t written = 0;
	f.Write(buf, sizeof(buf), &written);
	f.Close();
	return written == sizeof(buf);
}

bool WriteJunkFile(const Core::String& path, const char* bytes, size_t len)
{
	Core::File f;
	f.Create(path);
	if (f.IsInvalid())
	{
		return false;
	}
	uint32_t written = 0;
	f.Write(bytes, static_cast<uint32_t>(len), &written);
	f.Close();
	return written == len;
}

Core::String MakeTempPackageRoot()
{
	char  tmpl[] = "/tmp/kyty_loader_fixture_XXXXXX";
	char* dir    = ::mkdtemp(tmpl);
	Expect(dir != nullptr, "mkdtemp failed");
	const auto root = Core::String::FromUtf8(dir);
	g_fixture_cleanup.Add(root);
	return root;
}

void EnsureDir(const Core::String& path)
{
	Core::File::CreateDirectories(path);
}

// --- Scenarios --------------------------------------------------------------

int ScenarioPrimaryOnlyPlan()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	// Primary name validation does not require a valid ELF for BuildPlan path
	// shape — only basename rules. Create empty primary file.
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}

	const auto plan = Loader::ModuleLoadPlanning::BuildPlan(primary, /*discovery_enabled=*/false);
	Expect(plan.valid, "primary-only plan must be valid");
	Expect(plan.count == 1, "primary-only entry count");
	Expect(plan.diag.discovery_enabled == false, "discovery off");
	Expect(plan.diag.discovery_attempted == false, "discovery not attempted");
	Expect(plan.diag.adjacent_count == 0, "no adjacent");
	Expect(std::strcmp(plan.entries[0].relative_key, "eboot.bin") == 0, "primary relative key");
	return 0;
}

int ScenarioMultiModuleStableOrder()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");
	EnsureDir(root + U"/Media/Modules");
	// Deliberately create in non-sorted filesystem order: z then a.
	Expect(WriteMinimalSharedElf(root + U"/sce_module/z_mod.prx"), "write z_mod");
	Expect(WriteMinimalSharedElf(root + U"/sce_module/a_mod.prx"), "write a_mod");
	Expect(WriteMinimalSharedElf(root + U"/Media/Modules/media_mod.prx"), "write media_mod");
	// Extension-only junk under package root (not supported subdir) must be ignored.
	Expect(WriteJunkFile(root + U"/loose.prx", "XXXX", 4), "write loose prx");
	// Invalid ELF under sce_module must be rejected, not planned.
	Expect(WriteJunkFile(root + U"/sce_module/bad.prx", "NOT_ELF", 7), "write bad prx");

	const auto plan1 = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	const auto plan2 = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	Expect(plan1.valid && plan2.valid, "plans valid");
	Expect(plan1.count == 4, "primary + 3 valid adjacent");
	Expect(plan1.diag.adjacent_count == 3, "three adjacent");
	Expect(plan1.diag.rejection_count >= 1, "bad.prx rejected");
	// Deterministic order of adjacent: a_mod before z_mod.
	Expect(std::strcmp(plan1.entries[1].relative_key, "Media/Modules/media_mod.prx") == 0, "first adjacent media_mod");
	Expect(std::strcmp(plan1.entries[2].relative_key, "sce_module/a_mod.prx") == 0, "second adjacent a_mod");
	Expect(std::strcmp(plan1.entries[3].relative_key, "sce_module/z_mod.prx") == 0, "third adjacent z_mod");
	Expect(plan1.count == plan2.count, "stable count");
	for (uint32_t i = 0; i < plan1.count; ++i)
	{
		Expect(std::strcmp(plan1.entries[i].relative_key, plan2.entries[i].relative_key) == 0, "stable order across two BuildPlan runs");
	}
	// Loose PRX outside supported dirs must never appear.
	for (uint32_t i = 0; i < plan1.count; ++i)
	{
		Expect(std::strstr(plan1.entries[i].relative_key, "loose") == nullptr, "loose prx not planned");
	}
	return 0;
}

int ScenarioSymlinkEscapeRejected()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");

	const Core::String outside = root + U"_outside.prx";
	g_fixture_cleanup.Add(outside);
	Expect(WriteMinimalSharedElf(outside), "write outside module");
	const std::string link_path = std::string(root.C_Str()) + "/sce_module/escape.prx";
	std::error_code   ec;
	std::filesystem::create_symlink(outside.C_Str(), link_path, ec);
	Expect(!ec, "create escape symlink");

	const auto plan = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	Expect(plan.valid, "plan remains valid");
	Expect(plan.diag.adjacent_count == 0, "outside symlink must not be planned");
	return 0;
}

int ScenarioDuplicateIdentityRejected()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");
	EnsureDir(root + U"/modules");
	// Same identity "dup" from two supported package locations.
	Expect(WriteMinimalSharedElf(root + U"/sce_module/dup.prx"), "sce_module dup");
	Expect(WriteMinimalSharedElf(root + U"/modules/dup.prx"), "modules dup");

	const auto plan = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	Expect(plan.valid, "plan valid with duplicate reject");
	// One adjacent accepted, one rejected as duplicate_identity.
	Expect(plan.diag.adjacent_count == 1, "one adjacent after duplicate filter");
	bool saw_dup_reject = false;
	for (uint32_t i = 0; i < plan.diag.rejection_count; ++i)
	{
		if (std::strstr(plan.diag.rejections[i], "duplicate_identity") != nullptr)
		{
			saw_dup_reject = true;
		}
	}
	Expect(saw_dup_reject, "duplicate identity reported");
	return 0;
}

int ScenarioDiscoveryFeatureOff()
{
	// Feature must be disabled in env for this scenario (strict or unsafe without feature).
	const bool enabled = Core::BringUp::IsEnabled(Core::BringUp::Feature::AdjacentModuleDiscovery, Core::BringUp::Subsystem::Loader);
	Expect(!enabled, "adjacent discovery must be off for this scenario");

	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");
	Expect(WriteMinimalSharedElf(root + U"/sce_module/extra.prx"), "extra module");

	// Coordinator path: discovery off → plan primary-only, no adjacent apply.
	Loader::RuntimeLinker linker;
	// Do not call LoadProgram (minimal eboot is not a full guest image). Exercise
	// AfterPrimaryLoaded planning only.
	Loader::ModuleLifecycleCoordinator::AfterPrimaryLoaded(&linker, primary);
	const auto diag = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	Expect(diag.discovery_enabled == false, "diag discovery off");
	Expect(diag.adjacent_count == 0, "no adjacent when feature off");
	Expect(diag.applied_count == 0, "nothing applied");
	return 0;
}

int ScenarioFailBeforeMutate()
{
	// Stage a plan with valid shared stubs, then delete one file before the HLE
	// completion callback. Re-validation must fail before loading any adjacent.
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");
	const Core::String a = root + U"/sce_module/keep.prx";
	const Core::String b = root + U"/sce_module/drop.prx";
	Expect(WriteMinimalSharedElf(a), "keep");
	Expect(WriteMinimalSharedElf(b), "drop");

	Loader::RuntimeLinker linker;
	const uint32_t        baseline = linker.LoadedProgramCount();
	Loader::ModuleLifecycleCoordinator::AfterPrimaryLoaded(&linker, primary);
	Expect(Loader::ModuleLifecycleCoordinator::HasPendingAdjacentPlan(&linker), "plan staged");
	Expect(Loader::ModuleLifecycleCoordinator::GetDiagnostics().adjacent_count == 2, "two adjacent planned");

	// Remove one file after plan — fail-before-mutate must abort the whole adjacent set.
	std::remove(b.C_Str());

	Loader::ModuleLifecycleCoordinator::AfterHleSymbolsRegistered(&linker);
	Expect(linker.LoadedProgramCount() == baseline, "no partial adjacent load on revalidation failure");
	const auto diag = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	Expect(diag.applied_count == 0, "applied_count stays 0");
	return 0;
}

int ScenarioPlatformRevalidationRejectsChange()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	Expect(WriteJunkFile(primary, "", 0), "write primary");
	EnsureDir(root + U"/sce_module");
	const Core::String adjacent = root + U"/sce_module/platform.prx";
	Expect(WriteMinimalSharedElf(adjacent, 2), "write PS5 shared module");

	Loader::RuntimeLinker linker;
	const uint32_t        baseline = linker.LoadedProgramCount();
	Loader::ModuleLifecycleCoordinator::AfterPrimaryLoaded(&linker, primary);
	Expect(Loader::ModuleLifecycleCoordinator::HasPendingAdjacentPlan(&linker), "plan staged before ABI mutation");

	Expect(WriteMinimalSharedElf(adjacent, 0), "rewrite module with PS4 ABI");
	Loader::ModuleLifecycleCoordinator::AfterHleSymbolsRegistered(&linker);
	Expect(linker.LoadedProgramCount() == baseline, "ABI mismatch does not partially load adjacent modules");

	const auto diag = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	bool       saw_platform_rejection = false;
	for (uint32_t i = 0; i < diag.rejection_count; ++i)
	{
		saw_platform_rejection = saw_platform_rejection ||
		                         std::strstr(diag.rejections[i], "apply_aborted_platform_mismatch") != nullptr;
	}
	Expect(saw_platform_rejection, "revalidation reports ABI mutation");
	return 0;
}

int ScenarioAgentLoadPlanDiagnostics()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	{
		Core::File f;
		f.Create(primary);
		f.Close();
	}
	EnsureDir(root + U"/sce_module");
	Expect(WriteMinimalSharedElf(root + U"/sce_module/mod_b.prx"), "mod_b");
	Expect(WriteMinimalSharedElf(root + U"/sce_module/mod_a.prx"), "mod_a");

	const auto plan = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	Expect(plan.valid, "plan valid");
	Loader::ModuleLifecycleCoordinator::PublishDiagnostics(plan.diag);

	const auto        config      = Core::BringUp::GetConfig();
	const auto        diagnostics = Core::BringUp::GetDiagnostics();
	const auto        imports     = Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics();
	const auto        load_plan   = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	const std::string json        = Kyty::Emulator::Agent::BuildDiagnosticsResult(config, diagnostics, imports, load_plan);

	Expect(json.find("\"load_plan\"") != std::string::npos, "load_plan present");
	Expect(json.find("\"discovery_enabled\":true") != std::string::npos, "discovery_enabled true");
	Expect(json.find("sce_module/mod_a.prx") != std::string::npos, "sanitized entry a");
	Expect(json.find("sce_module/mod_b.prx") != std::string::npos, "sanitized entry b");
	// Host paths must not leak into agent diagnostics.
	Expect(json.find(root.C_Str()) == std::string::npos, "no host package root in json");
	Expect(json.find("/tmp/") == std::string::npos, "no /tmp host path in json");
	// Evidence line for scratch capture (sanitized relative keys only).
	std::printf("LOAD_PLAN_JSON_FRAGMENT load_plan.entries=%s\n",
	            json.find("sce_module/mod_a.prx") != std::string::npos ? "mod_a,mod_b" : "?");
	std::printf("LOAD_PLAN_DISCOVERY discovery_enabled=true entry_count=%u adjacent_count=%u\n", load_plan.entry_count,
	            load_plan.adjacent_count);
	std::fflush(stdout);
	return 0;
}

int ScenarioLocatorPrimaryNames()
{
	Expect(Loader::GuestExecutableLocator::IsPrimaryExecutableName(U"/x/eboot.bin"), "eboot.bin");
	Expect(Loader::GuestExecutableLocator::IsPrimaryExecutableName(U"main.elf"), "main.elf");
	Expect(!Loader::GuestExecutableLocator::IsPrimaryExecutableName(U"libc.prx"), "libc not primary");
	Expect(Loader::ModuleDiscovery::IsSupportedPackageSubdir("sce_module/"), "sce_module supported");
	Expect(Loader::ModuleDiscovery::IsSupportedPackageSubdir("modules/"), "modules supported");
	Expect(!Loader::ModuleDiscovery::IsSupportedPackageSubdir("Media/TitleStuff/"), "no title path");
	return 0;
}

// Real RuntimeLinker::Resolve path: non-allocator module exports retain
// precedence over HLE records with the same identity.
int ScenarioModuleExportWinsOverHle()
{
	Loader::RuntimeLinker linker;
	// Synthetic export module (no full PRX image — LoadProgram requires dynsym).
	Loader::Program* mod = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/mod_hle.prx");
	Expect(mod != nullptr, "synthetic module attached");
	Expect(mod->export_symbols != nullptr, "export_symbols present");

	Loader::SymbolResolve sr {};
	sr.name                 = U"sanitizedNid";
	sr.library              = U"libIntegration";
	sr.library_version      = 1;
	sr.module               = U"moduleIntegration";
	sr.module_version_major = 1;
	sr.module_version_minor = 0;
	sr.type                 = Loader::SymbolType::Func;

	const uint64_t hle_vaddr    = 0x111100001111ULL;
	const uint64_t module_vaddr = 0x222200002222ULL;
	linker.Symbols()->Add(sr, hle_vaddr);
	mod->export_symbols->Add(sr, module_vaddr);

	Loader::DynamicInfo dynamic_info {};
	dynamic_info.import_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	dynamic_info.import_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});
	mod->dynamic_info->export_libs.Add(Loader::LibraryId {U"lib-id", 1, U"libIntegration"});
	mod->dynamic_info->export_modules.Add(Loader::ModuleId {U"module-id", 1, 0, U"moduleIntegration"});

	Loader::Program importer {};
	importer.rt           = &linker;
	importer.dynamic_info = &dynamic_info;

	Loader::SymbolRecord out {};
	Loader::ImportResolver::Resolve(&linker, U"sanitizedNid#lib-id#module-id", Loader::SymbolType::Func, &importer, &out);
	Expect(out.vaddr == module_vaddr, "Resolve must return module export for non-allocator identity");
	Expect(Loader::ImportResolver::HleOwns(&linker, sr), "HLE owns identity");
	Expect(!Loader::ImportResolver::ResolvedMatchesHle(&linker, sr, out.vaddr), "non-allocator export is not HLE-bound");
	Expect(out.vaddr != hle_vaddr, "must not replace module export with HLE");
	return 0;
}

// Conflicting exports across two loaded modules are reported deterministically.
int ScenarioExportConflictReported()
{
	Loader::RuntimeLinker linker;
	Loader::Program*      a = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/mod_a.prx");
	Loader::Program*      b = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/mod_b.prx");
	Expect(a != nullptr && b != nullptr, "both modules attached");

	Loader::SymbolResolve sr {};
	sr.name                 = U"sharedExportNid";
	sr.library              = U"libShared";
	sr.library_version      = 1;
	sr.module               = U"modShared";
	sr.module_version_major = 1;
	sr.module_version_minor = 0;
	sr.type                 = Loader::SymbolType::Func;
	a->export_symbols->Add(sr, 0xA000ULL);
	b->export_symbols->Add(sr, 0xB000ULL);

	Loader::ModuleLoadPlanDiagnostics diag {};
	const uint32_t                    found = Loader::ModuleLifecycleCoordinator::DetectExportConflicts(&linker, &diag);
	Expect(found >= 1, "at least one export conflict");
	Expect(diag.export_conflict_count >= 1, "export_conflict_count");

	const Core::String canon = Loader::SymbolDatabase::GenerateName(sr);
	bool               saw   = false;
	for (uint32_t i = 0; i < diag.export_conflict_count; ++i)
	{
		const char* note = diag.export_conflicts[i];
		if (std::strstr(note, "export_conflict") != nullptr && std::strstr(note, canon.C_Str()) != nullptr &&
		    std::strstr(note, "mod_a.prx") != nullptr && std::strstr(note, "mod_b.prx") != nullptr)
		{
			// Distinct basenames — not "mod_b.prx mod_b.prx" from a shared label buffer.
			saw = true;
			Expect(std::strstr(note, "mod_b.prx mod_b.prx") == nullptr, "labels must not be duplicated");
			Expect(std::strstr(note, "mod_a.prx mod_a.prx") == nullptr, "labels must not be duplicated");
		}
	}
	Expect(saw, "conflict note has export_conflict + canon + both basenames");

	Loader::ModuleLoadPlanDiagnostics diag2 {};
	const uint32_t                    found2 = Loader::ModuleLifecycleCoordinator::DetectExportConflicts(&linker, &diag2);
	Expect(found2 == found, "stable conflict count");
	Expect(diag2.export_conflict_count == diag.export_conflict_count, "stable diagnostics count");
	for (uint32_t i = 0; i < diag.export_conflict_count; ++i)
	{
		Expect(std::strcmp(diag.export_conflicts[i], diag2.export_conflicts[i]) == 0, "stable conflict notes");
	}

	Loader::ModuleLifecycleCoordinator::PublishDiagnostics(diag);
	const auto        config      = Core::BringUp::GetConfig();
	const auto        diagnostics = Core::BringUp::GetDiagnostics();
	const auto        imports     = Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics();
	const auto        load_plan   = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	const std::string json        = Kyty::Emulator::Agent::BuildDiagnosticsResult(config, diagnostics, imports, load_plan);
	Expect(json.find("\"export_conflict_count\"") != std::string::npos, "export_conflict_count field");
	Expect(json.find("\"export_conflicts\"") != std::string::npos, "export_conflicts array");
	Expect(json.find("export_conflict") != std::string::npos, "conflict text in json");
	return 0;
}

// Conflict notes must retain the full canonical identity. Two identities that
// differ only after the legacy fixed diagnostic buffer must remain distinct.
int ScenarioLongExportConflictsStayDistinct()
{
	Loader::RuntimeLinker linker;
	Loader::Program*      a = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/long_a.prx");
	Loader::Program*      b = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/long_b.prx");
	Expect(a != nullptr && b != nullptr, "long-name modules attached");

	Loader::SymbolResolve first {};
	first.name                 = Core::String(U'x', 256) + U"_first";
	first.library              = U"libLong";
	first.library_version      = 1;
	first.module               = U"moduleLong";
	first.module_version_major = 1;
	first.module_version_minor = 0;
	first.type                 = Loader::SymbolType::Func;

	Loader::SymbolResolve second = first;
	second.name                  = Core::String(U'x', 256) + U"_second";

	a->export_symbols->Add(first, 0x100);
	b->export_symbols->Add(first, 0x200);
	a->export_symbols->Add(second, 0x300);
	b->export_symbols->Add(second, 0x400);

	Loader::ModuleLoadPlanDiagnostics diag {};
	Expect(Loader::ModuleLifecycleCoordinator::DetectExportConflicts(&linker, &diag) == 2, "both long canonical identities reported");
	Expect(diag.export_conflict_count == 2, "long diagnostics must not deduplicate after truncation");
	Expect(diag.export_conflict_identities[0].ContainsStr(U"_first"), "first long canonical identity retained");
	Expect(diag.export_conflict_identities[1].ContainsStr(U"_second"), "second long canonical identity retained");
	Loader::ModuleLifecycleCoordinator::PublishDiagnostics(diag);
	const auto published = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	Expect(published.export_conflict_identities[0].ContainsStr(U"_first"), "published diagnostics retain first long canonical identity");
	Expect(published.export_conflict_identities[1].ContainsStr(U"_second"), "published diagnostics retain second long canonical identity");
	return 0;
}

// The linker must publish copied export data, not raw Program pointers whose
// storage can disappear as soon as the linker lock is released.
int ScenarioExportSnapshotOutlivesModule()
{
	Loader::RuntimeLinker linker;
	Loader::Program*      module = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/snapshot.prx");
	Expect(module != nullptr, "snapshot module attached");

	Loader::SymbolResolve export_symbol {};
	export_symbol.name                 = U"snapshotExport";
	export_symbol.library              = U"libSnapshot";
	export_symbol.library_version      = 1;
	export_symbol.module               = U"moduleSnapshot";
	export_symbol.module_version_major = 1;
	export_symbol.module_version_minor = 0;
	export_symbol.type                 = Loader::SymbolType::Func;
	module->export_symbols->Add(export_symbol, 0x500);

	const auto snapshot = linker.SnapshotExportPrograms();
	linker.UnloadProgram(module);

	Expect(snapshot.Size() == 1, "snapshot captured one loaded program");
	Expect(snapshot[0].file_name == U"sce_module/snapshot.prx", "snapshot retained program name");
	Expect(snapshot[0].export_names.Size() == 1, "snapshot retained exported identity");
	Expect(snapshot[0].export_names[0] == Loader::SymbolDatabase::GenerateName(export_symbol),
	       "snapshot retained canonical export identity");
	return 0;
}

// Drive the shipped CommitAdjacentBatchOrRollback helper used by deferred
// application after each adjacent load. Inter-module export conflict must unload the whole
// batch — not leave a partial adjacent set.
int ScenarioApplyExportConflictRollback()
{
	Loader::RuntimeLinker linker;
	const uint32_t        baseline = linker.LoadedProgramCount();

	// Simulate Apply's batch after two successful loads (synthetic shells: full
	// LoadProgram needs dynsym; Apply uses this same commit helper post-load).
	Loader::Program* p0 = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/first.prx");
	Expect(p0 != nullptr, "first attached");
	Loader::Program* batch[2] = {};
	batch[0]                  = p0;

	Loader::ModuleLoadPlanDiagnostics diag {};
	// First member alone: no inter-module conflict yet.
	Expect(Loader::ModuleLifecycleCoordinator::CommitAdjacentBatchOrRollback(&linker, batch, 1, &diag) == 1, "single-entry batch commits");
	Expect(linker.LoadedProgramCount() == baseline + 1, "first remains loaded");
	Expect(diag.applied_count == 1, "applied_count 1 after first commit");

	Loader::Program* p1 = Loader::RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&linker, U"sce_module/second.prx");
	Expect(p1 != nullptr, "second attached");
	batch[1] = p1;
	Expect(linker.LoadedProgramCount() == baseline + 2, "both in linker before conflict commit");

	Loader::SymbolResolve sr {};
	sr.name                 = U"clashNid";
	sr.library              = U"libClash";
	sr.library_version      = 1;
	sr.module               = U"modClash";
	sr.module_version_major = 1;
	sr.module_version_minor = 0;
	sr.type                 = Loader::SymbolType::Func;
	p0->export_symbols->Add(sr, 0x100);
	p1->export_symbols->Add(sr, 0x200);

	// Commit second entry: ScanNewProgramConflicts must see inter-module clash and
	// RollbackAdjacentBatch both programs (deferred application path).
	Expect(Loader::ModuleLifecycleCoordinator::CommitAdjacentBatchOrRollback(&linker, batch, 2, &diag) == 0,
	       "conflict must rollback batch");
	Expect(linker.LoadedProgramCount() == baseline, "batch fully unloaded after conflict");
	Expect(diag.applied_count == 0, "applied_count cleared on rollback");
	Expect(diag.export_conflict_count >= 1, "export conflict recorded");
	bool saw_both = false;
	for (uint32_t i = 0; i < diag.export_conflict_count; ++i)
	{
		const char* note = diag.export_conflicts[i];
		if (std::strstr(note, "first.prx") != nullptr && std::strstr(note, "second.prx") != nullptr)
		{
			saw_both = true;
			Expect(std::strstr(note, "second.prx second.prx") == nullptr, "distinct labels required");
		}
	}
	Expect(saw_both, "conflict note names both batch members");
	bool saw_abort = false;
	for (uint32_t i = 0; i < diag.rejection_count; ++i)
	{
		if (std::strstr(diag.rejections[i], "apply_aborted_export_conflict") != nullptr)
		{
			saw_abort = true;
		}
	}
	Expect(saw_abort, "apply_aborted_export_conflict rejection");
	return 0;
}

int ScenarioDiscoveryCapacityRejectsWholePlan()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	Expect(WriteJunkFile(primary, "", 0), "write primary");
	EnsureDir(root + U"/sce_module");

	for (uint32_t i = 0; i <= Loader::kModuleLoadPlanMaxEntries; ++i)
	{
		char name[64];
		std::snprintf(name, sizeof(name), "/sce_module/module_%03u.prx", i);
		Expect(WriteMinimalSharedElf(root + Core::String::FromUtf8(name)), "write capacity module");
	}

	const auto plan = Loader::ModuleLoadPlanning::BuildPlan(primary, true);
	Expect(!plan.valid, "truncated discovery must invalidate the whole plan");
	Expect(plan.count == 1, "invalid plan retains only the primary description");
	Expect(plan.diag.adjacent_count == 0, "invalid plan publishes no partial adjacent set");
	Expect(std::strstr(plan.error, "capacity") != nullptr, "capacity error is explicit");
	return 0;
}

int ScenarioAdjacentApplyDeferredUntilHle()
{
	const Core::String root    = MakeTempPackageRoot();
	const Core::String primary = root + U"/eboot.bin";
	Expect(WriteJunkFile(primary, "", 0), "write primary");
	EnsureDir(root + U"/sce_module");
	const Core::String adjacent = root + U"/sce_module/deferred.prx";
	Expect(WriteMinimalSharedElf(adjacent), "write deferred module");

	Loader::RuntimeLinker linker;
	const uint32_t        baseline = linker.LoadedProgramCount();
	Loader::ModuleLifecycleCoordinator::AfterPrimaryLoaded(&linker, primary);
	Expect(Loader::ModuleLifecycleCoordinator::HasPendingAdjacentPlan(&linker), "primary load stages an adjacent plan");
	Expect(linker.LoadedProgramCount() == baseline, "staging must not mutate linker programs");

	// Make application observably fail-before-mutate. Before the HLE callback,
	// diagnostics must still describe only planning, not application.
	std::remove(adjacent.C_Str());
	auto diag = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	Expect(diag.applied_count == 0, "plan is not applied during primary load");
	for (uint32_t i = 0; i < diag.rejection_count; ++i)
	{
		Expect(std::strstr(diag.rejections[i], "apply_aborted") == nullptr, "no apply diagnostic before HLE registration");
	}

	Loader::ModuleLifecycleCoordinator::AfterHleSymbolsRegistered(&linker);
	Expect(!Loader::ModuleLifecycleCoordinator::HasPendingAdjacentPlan(&linker), "HLE callback consumes the staged plan");
	Expect(linker.LoadedProgramCount() == baseline, "failed deferred apply leaves linker unchanged");
	diag                    = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	bool saw_deferred_apply = false;
	for (uint32_t i = 0; i < diag.rejection_count; ++i)
	{
		saw_deferred_apply = saw_deferred_apply || std::strstr(diag.rejections[i], "apply_aborted_revalidation") != nullptr;
	}
	Expect(saw_deferred_apply, "deferred apply runs only after HLE registration");
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: kyty_loader_integration <scenario>\n");
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
	if (scenario == "primary_only_plan")
	{
		return ScenarioPrimaryOnlyPlan();
	}
	if (scenario == "multi_module_stable_order")
	{
		return ScenarioMultiModuleStableOrder();
	}
	if (scenario == "symlink_escape_rejected")
	{
		return ScenarioSymlinkEscapeRejected();
	}
	if (scenario == "duplicate_identity_rejected")
	{
		return ScenarioDuplicateIdentityRejected();
	}
	if (scenario == "discovery_feature_off")
	{
		return ScenarioDiscoveryFeatureOff();
	}
	if (scenario == "fail_before_mutate")
	{
		return ScenarioFailBeforeMutate();
	}
	if (scenario == "platform_revalidation_rejects_change")
	{
		return ScenarioPlatformRevalidationRejectsChange();
	}
	if (scenario == "agent_load_plan_diagnostics")
	{
		return ScenarioAgentLoadPlanDiagnostics();
	}
	if (scenario == "locator_primary_names")
	{
		return ScenarioLocatorPrimaryNames();
	}
	if (scenario == "module_export_wins_over_hle")
	{
		return ScenarioModuleExportWinsOverHle();
	}
	if (scenario == "export_conflict_reported")
	{
		return ScenarioExportConflictReported();
	}
	if (scenario == "long_export_conflicts_stay_distinct")
	{
		return ScenarioLongExportConflictsStayDistinct();
	}
	if (scenario == "export_snapshot_outlives_module")
	{
		return ScenarioExportSnapshotOutlivesModule();
	}
	if (scenario == "apply_export_conflict_rollback")
	{
		return ScenarioApplyExportConflictRollback();
	}
	if (scenario == "discovery_capacity_rejects_whole_plan")
	{
		return ScenarioDiscoveryCapacityRejectsWholePlan();
	}
	if (scenario == "adjacent_apply_deferred_until_hle")
	{
		return ScenarioAdjacentApplyDeferredUntilHle();
	}

	std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
	return 125;
}
