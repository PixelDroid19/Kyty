#include "Emulator/Loader/ModuleLoad.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Validation/DomainValidators.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {
namespace {

using Core::File;
using Core::String;

Core::Mutex               g_diag_mutex;
ModuleLoadPlanDiagnostics g_last_diag {};

struct PendingModulePlan
{
	RuntimeLinker* owner = nullptr;
	ModuleLoadPlan plan {};
};

PendingModulePlan g_pending {};

void CopyCStr(char* dst, size_t cap, const char* src)
{
	if (dst == nullptr || cap == 0)
	{
		return;
	}
	if (src == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, cap, "%s", src);
}

void CopyString(char* dst, size_t cap, const String& src)
{
	CopyCStr(dst, cap, src.C_Str());
}

void PushRejection(ModuleLoadPlanDiagnostics* diag, const char* note)
{
	if (diag == nullptr || note == nullptr)
	{
		return;
	}
	if (diag->rejection_count >= kModuleLoadPlanMaxRejections)
	{
		return;
	}
	CopyCStr(diag->rejections[diag->rejection_count], sizeof(diag->rejections[0]), note);
	++diag->rejection_count;
}

void PushRejection(ModuleLoadPlanDiagnostics* diag, const String& note)
{
	if (diag == nullptr || note.IsEmpty() || diag->rejection_count >= kModuleLoadPlanMaxRejections)
	{
		return;
	}
	CopyString(diag->rejections[diag->rejection_count], sizeof(diag->rejections[0]), note);
	++diag->rejection_count;
}

bool HasAdjacentExtension(const String& name)
{
	const String lower = name.ToLower();
	// Adjacent discovery accepts only PRX/SPRX — not arbitrary .elf next to the
	// package (extension alone is never enough; shared-ELF probe still required).
	return lower.EndsWith(U".prx") || lower.EndsWith(U".sprx");
}

bool IsContainedRegularFile(const String& package_root, const String& host_path)
{
	std::error_code ec;
	const auto      root = std::filesystem::canonical(package_root.C_Str(), ec);
	if (ec)
	{
		return false;
	}
	const auto candidate = std::filesystem::canonical(host_path.C_Str(), ec);
	if (ec || !std::filesystem::is_regular_file(candidate, ec) || ec)
	{
		return false;
	}
	const auto relative = std::filesystem::relative(candidate, root, ec);
	if (ec || relative.empty() || relative.is_absolute())
	{
		return false;
	}
	const auto first = relative.begin();
	return first == relative.end() || *first != "..";
}

bool ProbeSharedElf(const String& package_root, const String& host_path)
{
	if (!IsContainedRegularFile(package_root, host_path))
	{
		return false;
	}

	Elf64 elf;
	elf.Open(host_path);
	return elf.IsValid() && elf.IsShared();
}

String NormalizeRoot(const String& root_in)
{
	String root = root_in;
	if (root.IsEmpty())
	{
		return root;
	}
	if (!root.EndsWith(U"/") && !root.EndsWith(U"\\"))
	{
		root = root + U"/";
	}
	return root;
}

void SortEntries(ModulePlanEntry* entries, uint32_t count)
{
	if (count < 2)
	{
		return;
	}
	std::sort(entries, entries + count,
	          [](const ModulePlanEntry& a, const ModulePlanEntry& b) { return std::strcmp(a.relative_key, b.relative_key) < 0; });
}

bool IdentitySeen(const ModulePlanEntry* entries, uint32_t count, const char* identity)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		if (std::strcmp(entries[i].identity, identity) == 0)
		{
			return true;
		}
	}
	return false;
}

} // namespace

// --- GuestExecutableLocator -------------------------------------------------

namespace GuestExecutableLocator {

bool IsPrimaryExecutableName(const String& file_name)
{
	const String base = file_name.FilenameWithoutDirectory().ToLower();
	return base == U"eboot.bin" || base == U"main.elf" || base == U"eboot.elf";
}

String PackageRootFromPrimary(const String& primary_host_path)
{
	if (!ValidatePrimaryPath(primary_host_path))
	{
		return U"";
	}
	return primary_host_path.DirectoryWithoutFilename();
}

bool ValidatePrimaryPath(const String& primary_host_path)
{
	if (primary_host_path.IsEmpty())
	{
		return false;
	}
	Emulator::Validation::GuestExecutableRequest greq {};
	greq.root_path          = primary_host_path.C_Str();
	greq.require_eboot_name = false;
	if (!Emulator::Validation::ValidateGuestExecutable(greq).Ok())
	{
		return false;
	}
	return IsPrimaryExecutableName(primary_host_path);
}

} // namespace GuestExecutableLocator

// --- ModuleDiscovery --------------------------------------------------------

namespace ModuleDiscovery {

bool IsSupportedPackageSubdir(const char* relative_subdir)
{
	if (relative_subdir == nullptr)
	{
		return false;
	}
	// Generic package locations only — no title-specific directories.
	return std::strcmp(relative_subdir, "sce_module/") == 0 || std::strcmp(relative_subdir, "modules/") == 0 ||
	       std::strcmp(relative_subdir, "Media/Modules/") == 0;
}

DiscoveryResult DiscoverAdjacentCandidates(const String& package_root_host, String* out_host_paths, String* out_relative_keys,
                                           uint32_t capacity)
{
	DiscoveryResult result {};
	if (out_host_paths == nullptr || out_relative_keys == nullptr || capacity == 0)
	{
		return result;
	}
	if (package_root_host.IsEmpty())
	{
		return result;
	}

	const String root      = NormalizeRoot(package_root_host);
	const char*  subdirs[] = {"sce_module/", "modules/", "Media/Modules/"};

	for (const char* sub: subdirs)
	{
		if (!IsSupportedPackageSubdir(sub))
		{
			continue;
		}
		const String dir = root + String::FromUtf8(sub);
		if (!File::IsDirectoryExisting(dir))
		{
			continue;
		}
		const auto entries = File::GetDirEntries(dir);
		for (const auto& entry: entries)
		{
			const String name = entry.name;
			if (name.IsEmpty() || name == U"." || name == U"..")
			{
				continue;
			}
			if (!entry.is_file)
			{
				continue;
			}
			if (!HasAdjacentExtension(name))
			{
				continue;
			}
			if (GuestExecutableLocator::IsPrimaryExecutableName(name))
			{
				continue;
			}
			const String full = dir + name;
			if (!File::IsFileExisting(full))
			{
				continue;
			}
			// Dedup by relative key within this scan.
			const String rel  = String::FromUtf8(sub) + name;
			bool         seen = false;
			for (uint32_t i = 0; i < result.count; ++i)
			{
				if (out_relative_keys[i] == rel)
				{
					seen = true;
					break;
				}
			}
			if (seen)
			{
				continue;
			}
			if (result.count >= capacity)
			{
				result.truncated = true;
				continue;
			}
			out_host_paths[result.count]    = full;
			out_relative_keys[result.count] = rel;
			++result.count;
		}
	}
	return result;
}

} // namespace ModuleDiscovery

// --- ModuleLoadPlanning -----------------------------------------------------

namespace ModuleLoadPlanning {

ModuleLoadPlan BuildPlan(const String& primary_host_path, bool discovery_enabled)
{
	ModuleLoadPlan plan {};
	plan.diag.discovery_enabled = discovery_enabled;

	if (!GuestExecutableLocator::ValidatePrimaryPath(primary_host_path))
	{
		CopyCStr(plan.error, sizeof(plan.error), "primary path failed validation");
		plan.valid = false;
		return plan;
	}

	// Primary entry always first in conceptual role; sort puts relative keys
	// alphabetically so primary "eboot.bin" key is explicit.
	ModulePlanEntry primary {};
	CopyString(primary.host_path, sizeof(primary.host_path), primary_host_path);
	const String primary_base = primary_host_path.FilenameWithoutDirectory();
	CopyString(primary.relative_key, sizeof(primary.relative_key), primary_base);
	CopyString(primary.identity, sizeof(primary.identity), primary_base.FilenameWithoutExtension());
	primary.role = ModulePlanRole::Primary;

	plan.entries[0] = primary;
	plan.count      = 1;
	CopyCStr(plan.diag.primary_identity, sizeof(plan.diag.primary_identity), primary.identity);

	if (!discovery_enabled)
	{
		plan.diag.entry_count    = plan.count;
		plan.diag.adjacent_count = 0;
		CopyCStr(plan.diag.entries[0], sizeof(plan.diag.entries[0]), plan.entries[0].relative_key);
		plan.valid = true;
		return plan;
	}

	plan.diag.discovery_attempted = true;
	const String package_root     = GuestExecutableLocator::PackageRootFromPrimary(primary_host_path);
	if (package_root.IsEmpty())
	{
		CopyCStr(plan.error, sizeof(plan.error), "package root unresolved");
		plan.valid = false;
		return plan;
	}

	constexpr uint32_t kAdjacentCapacity = kModuleLoadPlanMaxEntries - 1;
	String             host_buf[kAdjacentCapacity];
	String             rel_buf[kAdjacentCapacity];
	const auto         discovery = ModuleDiscovery::DiscoverAdjacentCandidates(package_root, host_buf, rel_buf, kAdjacentCapacity);
	if (discovery.truncated)
	{
		PushRejection(&plan.diag, "reject discovery_capacity");
		CopyCStr(plan.error, sizeof(plan.error), "adjacent discovery capacity exceeded");
		plan.diag.entry_count    = 1;
		plan.diag.adjacent_count = 0;
		CopyCStr(plan.diag.entries[0], sizeof(plan.diag.entries[0]), plan.entries[0].relative_key);
		plan.valid = false;
		return plan;
	}

	for (uint32_t i = 0; i < discovery.count; ++i)
	{
		const String& host = host_buf[i];
		const String& rel  = rel_buf[i];
		const String  file = host.FilenameWithoutDirectory();
		const String  base = file.FilenameWithoutExtension();

		// Identity validation (basename only).
		Emulator::Validation::ModuleMetadataRequest mreq {};
		mreq.name         = base.C_Str();
		const auto mod_ok = Emulator::Validation::ValidateModuleMetadata(mreq);
		if (!mod_ok.Ok())
		{
			PushRejection(&plan.diag, U"reject identity " + rel + U": " + String::FromUtf8(mod_ok.error.code));
			continue;
		}

		// Shared ELF required — never load extension-only junk.
		if (!ProbeSharedElf(package_root, host))
		{
			PushRejection(&plan.diag, U"reject not_shared_elf " + rel);
			continue;
		}

		char identity[96];
		CopyString(identity, sizeof(identity), base);
		if (IdentitySeen(plan.entries, plan.count, identity))
		{
			PushRejection(&plan.diag, U"reject duplicate_identity " + rel);
			continue;
		}

		ModulePlanEntry entry {};
		CopyString(entry.host_path, sizeof(entry.host_path), host);
		CopyString(entry.relative_key, sizeof(entry.relative_key), rel);
		CopyCStr(entry.identity, sizeof(entry.identity), identity);
		entry.role               = ModulePlanRole::AdjacentShared;
		plan.entries[plan.count] = entry;
		++plan.count;
	}

	// Deterministic order: primary stays role Primary; all entries sorted by
	// relative_key so the same package layout always yields the same order.
	// Keep primary at index 0 after sort by using a key that sorts first? No —
	// acceptance requires stable module order for the package; primary is
	// always applied separately. Sort only adjacent entries.
	if (plan.count > 1)
	{
		SortEntries(plan.entries + 1, plan.count - 1);
	}

	plan.diag.entry_count    = plan.count;
	plan.diag.adjacent_count = plan.count > 0 ? plan.count - 1 : 0;
	for (uint32_t i = 0; i < plan.count && i < kModuleLoadPlanMaxEntries; ++i)
	{
		CopyCStr(plan.diag.entries[i], sizeof(plan.diag.entries[i]), plan.entries[i].relative_key);
	}
	plan.valid = true;
	return plan;
}

} // namespace ModuleLoadPlanning

// --- ProgramLoader / RelocationEngine / ImportResolver ----------------------

namespace ProgramLoader {

Program* Load(RuntimeLinker* rt, const String& host_path)
{
	EXIT_IF(rt == nullptr);
	return rt->LoadProgram(host_path);
}

void Unload(RuntimeLinker* rt, Program* program)
{
	EXIT_IF(rt == nullptr);
	if (program == nullptr)
	{
		return;
	}
	rt->UnloadProgram(program);
}

} // namespace ProgramLoader

namespace RelocationEngine {

void RelocateAll(RuntimeLinker* rt)
{
	EXIT_IF(rt == nullptr);
	rt->RelocateAll();
}

} // namespace RelocationEngine

namespace ImportResolver {

void Resolve(RuntimeLinker* rt, const String& name, SymbolType type, Program* program, SymbolRecord* out_info)
{
	EXIT_IF(rt == nullptr);
	EXIT_IF(out_info == nullptr);
	rt->Resolve(name, type, program, out_info, nullptr);
}

bool HleOwns(RuntimeLinker* rt, const SymbolResolve& sr)
{
	EXIT_IF(rt == nullptr);
	SymbolDatabase* hle = rt->Symbols();
	if (hle == nullptr)
	{
		return false;
	}
	const SymbolRecord* rec = hle->Find(sr);
	return rec != nullptr && rec->vaddr != 0;
}

bool ResolvedMatchesHle(RuntimeLinker* rt, const SymbolResolve& sr, uint64_t resolved_vaddr)
{
	EXIT_IF(rt == nullptr);
	SymbolDatabase* hle = rt->Symbols();
	if (hle == nullptr)
	{
		return false;
	}
	const SymbolRecord* rec = hle->Find(sr);
	return rec != nullptr && rec->vaddr != 0 && rec->vaddr == resolved_vaddr;
}

} // namespace ImportResolver

// --- ModuleLifecycleCoordinator ---------------------------------------------

namespace ModuleLifecycleCoordinator {
namespace {

void PushExportConflict(ModuleLoadPlanDiagnostics* diag, const String& note)
{
	if (diag == nullptr || note.IsEmpty())
	{
		return;
	}
	if (diag->export_conflict_count >= kModuleLoadPlanMaxConflicts)
	{
		return;
	}
	// Deterministic: skip exact duplicate notes already recorded.
	for (uint32_t i = 0; i < diag->export_conflict_count; ++i)
	{
		if (diag->export_conflict_identities[i] == note)
		{
			return;
		}
	}
	const uint32_t index                    = diag->export_conflict_count;
	diag->export_conflict_identities[index] = note;
	CopyString(diag->export_conflicts[index], sizeof(diag->export_conflicts[index]), note);
	++diag->export_conflict_count;
}

String ProgramLabel(const ProgramExportSnapshot& program)
{
	if (program.file_name.IsEmpty())
	{
		return U"unknown";
	}
	return program.file_name.FilenameWithoutDirectory();
}

bool IsLexicallyFirst(const String& first, const String& second)
{
	const auto first_utf8  = first.utf8_str();
	const auto second_utf8 = second.utf8_str();
	return std::strcmp(first_utf8.GetData(), second_utf8.GetData()) <= 0;
}

bool ExportsCanonicalName(const ProgramExportSnapshot& program, const String& canonical_name)
{
	for (const auto& export_name: program.export_names)
	{
		if (export_name == canonical_name)
		{
			return true;
		}
	}
	return false;
}

String ConflictNote(const String& canonical_name, const String& first, const String& second)
{
	return U"export_conflict " + canonical_name + U" " + first + U" " + second;
}

// True if newly loaded program's exports collide with another module (not HLE).
// HLE collisions are reported separately and do not force unload.
bool ScanNewProgramConflicts(RuntimeLinker* rt, int32_t newly_loaded_id, ModuleLoadPlanDiagnostics* diag, bool* out_inter_module_conflict)
{
	EXIT_IF(rt == nullptr);
	EXIT_IF(out_inter_module_conflict == nullptr);
	*out_inter_module_conflict                = false;
	const auto                   programs     = rt->SnapshotExportPrograms();
	const ProgramExportSnapshot* newly_loaded = nullptr;
	for (const auto& program: programs)
	{
		if (program.unique_id == newly_loaded_id)
		{
			newly_loaded = &program;
			break;
		}
	}
	if (newly_loaded == nullptr || newly_loaded->export_names.IsEmpty())
	{
		return false;
	}
	SymbolDatabase* hle       = rt->Symbols();
	const String    new_label = ProgramLabel(*newly_loaded);
	bool            any       = false;

	for (const auto& canonical_name: newly_loaded->export_names)
	{
		// HLE conflict: report, keep module. Loaded-module exports remain valid
		// resolution candidates for imports that name this module/library.
		if (hle != nullptr)
		{
			const SymbolRecord* h = hle->FindByCanonicalName(canonical_name);
			if (h != nullptr && h->vaddr != 0)
			{
				PushExportConflict(diag, ConflictNote(canonical_name, U"hle", new_label));
				any = true;
			}
		}

		// Other loaded programs (including primary and earlier adjacent).
		for (const auto& other: programs)
		{
			if (other.unique_id == newly_loaded_id || !ExportsCanonicalName(other, canonical_name))
			{
				continue;
			}
			const String other_label = ProgramLabel(other);
			const bool   new_first   = IsLexicallyFirst(new_label, other_label);
			PushExportConflict(diag,
			                   ConflictNote(canonical_name, new_first ? new_label : other_label, new_first ? other_label : new_label));
			*out_inter_module_conflict = true;
			any                        = true;
		}
	}
	return any;
}

void RollbackAdjacentBatch(RuntimeLinker* rt, Program** batch, uint32_t batch_count)
{
	// Unload in reverse order of load.
	for (uint32_t i = batch_count; i > 0; --i)
	{
		Program* p = batch[i - 1];
		if (p != nullptr)
		{
			ProgramLoader::Unload(rt, p);
		}
	}
}

} // namespace

void PublishDiagnostics(const ModuleLoadPlanDiagnostics& diag)
{
	Core::LockGuard lock(g_diag_mutex);
	g_last_diag = diag;
}

ModuleLoadPlanDiagnostics GetDiagnostics()
{
	Core::LockGuard lock(g_diag_mutex);
	return g_last_diag;
}

uint32_t DetectExportConflicts(RuntimeLinker* rt, ModuleLoadPlanDiagnostics* diag)
{
	EXIT_IF(rt == nullptr);
	EXIT_IF(diag == nullptr);

	const uint32_t  before   = diag->export_conflict_count;
	const auto      programs = rt->SnapshotExportPrograms();
	SymbolDatabase* hle      = rt->Symbols();

	// Pairwise program exports (i < j) + each program vs HLE — deterministic notes.
	for (uint32_t i = 0; i < programs.Size(); ++i)
	{
		const auto&  a       = programs[i];
		const String label_a = ProgramLabel(a);
		for (const auto& canonical_name: a.export_names)
		{
			if (hle != nullptr)
			{
				const SymbolRecord* h = hle->FindByCanonicalName(canonical_name);
				if (h != nullptr && h->vaddr != 0)
				{
					PushExportConflict(diag, ConflictNote(canonical_name, U"hle", label_a));
				}
			}
			for (uint32_t j = i + 1; j < programs.Size(); ++j)
			{
				const auto& b = programs[j];
				if (!ExportsCanonicalName(b, canonical_name))
				{
					continue;
				}
				const String label_b = ProgramLabel(b);
				const bool   a_first = IsLexicallyFirst(label_a, label_b);
				PushExportConflict(diag, ConflictNote(canonical_name, a_first ? label_a : label_b, a_first ? label_b : label_a));
			}
		}
	}
	return diag->export_conflict_count - before;
}

int CommitAdjacentBatchOrRollback(RuntimeLinker* rt, Program** batch, uint32_t batch_count, ModuleLoadPlanDiagnostics* diag)
{
	EXIT_IF(rt == nullptr);
	EXIT_IF(diag == nullptr);
	if (batch_count == 0)
	{
		diag->applied_count = 0;
		PublishDiagnostics(*diag);
		return 0;
	}
	EXIT_IF(batch == nullptr);

	Program* newly = batch[batch_count - 1];
	if (newly == nullptr)
	{
		EXIT("adjacent batch contains null program");
	}
	bool inter_module = false;
	(void)ScanNewProgramConflicts(rt, newly->unique_id, diag, &inter_module);
	if (inter_module)
	{
		std::fprintf(stderr, "KYTY_LOADER: inter-module export conflict; rolling back adjacent batch (%u)\n", batch_count);
		std::fflush(stderr);
		PushRejection(diag, "apply_aborted_export_conflict");
		RollbackAdjacentBatch(rt, batch, batch_count);
		diag->applied_count = 0;
		PublishDiagnostics(*diag);
		return 0;
	}

	diag->applied_count = batch_count;
	PublishDiagnostics(*diag);
	return static_cast<int>(batch_count);
}

namespace {

// This is deliberately not a public loader operation. Production can reach it
// only through AfterHleSymbolsRegistered, so conflict checks always observe the
// complete HLE symbol database.
int ApplyPlanAfterHle(RuntimeLinker* rt, const ModuleLoadPlan& plan)
{
	EXIT_IF(rt == nullptr);
	if (!plan.valid)
	{
		return 0;
	}

	ModuleLoadPlanDiagnostics diag = plan.diag;
	diag.applied_count             = 0;

	// Fail-before-mutate: re-probe every adjacent entry before loading any.
	for (uint32_t i = 0; i < plan.count; ++i)
	{
		if (plan.entries[i].role != ModulePlanRole::AdjacentShared)
		{
			continue;
		}
		const String host         = String::FromUtf8(plan.entries[i].host_path);
		const String package_root = String::FromUtf8(plan.entries[0].host_path).DirectoryWithoutFilename();
		if (!ProbeSharedElf(package_root, host))
		{
			std::fprintf(stderr,
			             "KYTY_LOADER: adjacent plan re-validation failed for %s; "
			             "no adjacent modules loaded\n",
			             plan.entries[i].relative_key);
			std::fflush(stderr);
			PushRejection(&diag, "apply_aborted_revalidation");
			PublishDiagnostics(diag);
			return 0;
		}
	}

	// Track only programs loaded in this apply so rollback never touches primary/HLE.
	Program* batch[kModuleLoadPlanMaxEntries] = {};
	uint32_t batch_count                      = 0;

	for (uint32_t i = 0; i < plan.count; ++i)
	{
		if (plan.entries[i].role != ModulePlanRole::AdjacentShared)
		{
			continue;
		}
		const String host = String::FromUtf8(plan.entries[i].host_path);
		std::printf("KYTY_LOADER: adjacent load %s\n", plan.entries[i].relative_key);
		std::fflush(stdout);
		Program* p = ProgramLoader::Load(rt, host);
		if (p == nullptr)
		{
			std::fprintf(stderr, "KYTY_LOADER: LoadProgram failed for %s; rolling back adjacent batch\n", plan.entries[i].relative_key);
			std::fflush(stderr);
			PushRejection(&diag, "apply_aborted_load_failed");
			RollbackAdjacentBatch(rt, batch, batch_count);
			diag.applied_count = 0;
			PublishDiagnostics(diag);
			return 0;
		}
		Emulator::Agent::Lifecycle::EmitModuleLoaded(plan.entries[i].relative_key);
		p->fail_if_global_not_resolved = false;
		if (batch_count < kModuleLoadPlanMaxEntries)
		{
			batch[batch_count++] = p;
		}

		// Conflict scan + possible full-batch rollback (shared with tests).
		if (CommitAdjacentBatchOrRollback(rt, batch, batch_count, &diag) == 0)
		{
			return 0;
		}
	}

	diag.applied_count = batch_count;
	PublishDiagnostics(diag);
	return static_cast<int>(batch_count);
}

} // namespace

void AfterPrimaryLoaded(RuntimeLinker* rt, const String& primary_host_path)
{
	EXIT_IF(rt == nullptr);

	{
		Core::LockGuard lock(g_diag_mutex);
		g_pending = {};
	}

	const bool diagnostic =
	    Core::BringUp::IsEnabled(Core::BringUp::Feature::AdjacentModuleDiscovery, Core::BringUp::Subsystem::Loader);

	// Always publish a strict plan snapshot. Adjacent package modules are guest
	// load inputs, not a diagnostic-only behavior.
	const ModuleLoadPlan plan = ModuleLoadPlanning::BuildPlan(primary_host_path, true);
	PublishDiagnostics(plan.diag);

	// Agent observation only (sanitized basenames / relative keys).
	Emulator::Agent::Lifecycle::EmitExecutableDiscovered(plan.diag.primary_identity);
	Emulator::Agent::Lifecycle::EmitModuleDiscovery(plan.diag.entry_count, plan.diag.adjacent_count, plan.diag.rejection_count);

	if (!plan.valid)
	{
		std::fprintf(stderr, "KYTY_LOADER: adjacent discovery plan invalid: %s\n", plan.error);
		std::fflush(stderr);
		return;
	}

	if (plan.diag.adjacent_count == 0)
	{
		return;
	}

	if (diagnostic)
	{
		const auto decision = Core::BringUp::Report(Core::BringUp::Feature::AdjacentModuleDiscovery, Core::BringUp::Subsystem::Loader,
		                                            "adjacent_module_discovery", __FILE__, __LINE__);
		if (decision != Core::BringUp::Decision::Continue)
		{
			std::fprintf(stderr, "KYTY_LOADER: adjacent discovery policy Halt; plan not applied\n");
			std::fflush(stderr);
			return;
		}

		std::printf("KYTY_LOADER: plan entries=%u adjacent=%u rejections=%u\n", plan.diag.entry_count, plan.diag.adjacent_count,
		            plan.diag.rejection_count);
		for (uint32_t i = 0; i < plan.diag.entry_count; ++i)
		{
			std::printf("KYTY_LOADER: plan[%u]=%s\n", i, plan.diag.entries[i]);
		}
		for (uint32_t i = 0; i < plan.diag.rejection_count; ++i)
		{
			std::printf("KYTY_LOADER: reject[%u]=%s\n", i, plan.diag.rejections[i]);
		}
		std::fflush(stdout);
	}

	Core::LockGuard lock(g_diag_mutex);
	g_pending.owner = rt;
	g_pending.plan  = plan;
}

bool HasPendingAdjacentPlan(RuntimeLinker* rt)
{
	Core::LockGuard lock(g_diag_mutex);
	return rt != nullptr && g_pending.owner == rt;
}

void AfterHleSymbolsRegistered(RuntimeLinker* rt)
{
	EXIT_IF(rt == nullptr);

	ModuleLoadPlan plan {};
	{
		Core::LockGuard lock(g_diag_mutex);
		if (g_pending.owner != rt)
		{
			return;
		}
		plan      = g_pending.plan;
		g_pending = {};
	}

	(void)ApplyPlanAfterHle(rt, plan);
}

} // namespace ModuleLifecycleCoordinator

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
