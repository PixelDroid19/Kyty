#include "Kyty/DevTools/Catalog/ExportCatalog.h"

#include <cstdio>
#include <cstring>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] bool IsEmpty(const char* s) noexcept
{
	return s == nullptr || s[0] == '\0';
}

[[nodiscard]] bool CopyField(char* dst, size_t dst_cap, const char* src) noexcept
{
	if (dst == nullptr || dst_cap == 0u || IsEmpty(src))
	{
		return false;
	}
	const size_t n = std::strlen(src);
	if (n + 1u > dst_cap)
	{
		return false;
	}
	std::memcpy(dst, src, n + 1u);
	return true;
}

[[nodiscard]] bool Eq(const char* a, const char* b) noexcept
{
	if (a == nullptr || b == nullptr)
	{
		return false;
	}
	return std::strcmp(a, b) == 0;
}

[[nodiscard]] bool ValidEntry(const ExportEntry& e) noexcept
{
	if (IsEmpty(e.library) || IsEmpty(e.export_name) || IsEmpty(e.nid) || IsEmpty(e.source))
	{
		return false;
	}
	if (std::strlen(e.nid) > 15u || std::strlen(e.export_name) > 127u || std::strlen(e.library) > 63u)
	{
		return false;
	}
	if (e.flags != 0u)
	{
		return false;
	}
	switch (e.status)
	{
		case ExportStatus::Known:
		case ExportStatus::Required:
		case ExportStatus::Implemented:
		case ExportStatus::Unimplemented:
		case ExportStatus::Conflict:
		case ExportStatus::UnknownAbi: return true;
		default: return false;
	}
}

} // namespace

const char* ExportStatusName(ExportStatus s) noexcept
{
	switch (s)
	{
		case ExportStatus::Known: return "KNOWN";
		case ExportStatus::Required: return "REQUIRED";
		case ExportStatus::Implemented: return "IMPLEMENTED";
		case ExportStatus::Unimplemented: return "UNIMPLEMENTED";
		case ExportStatus::Conflict: return "CONFLICT";
		case ExportStatus::UnknownAbi: return "UNKNOWN_ABI";
		default: return "INVALID";
	}
}

void ExportCatalog::Clear() noexcept
{
	for (uint32_t i = 0; i < count_; ++i)
	{
		entries_[i] = ExportEntry {};
	}
	count_ = 0;
}

ExportCatalogResult ExportCatalog::Insert(const ExportEntry& entry) noexcept
{
	if (!ValidEntry(entry))
	{
		return ExportCatalogResult::InvalidArgument;
	}
	// Detect same library+NID with different export name → conflict.
	for (uint32_t i = 0; i < count_; ++i)
	{
		if (Eq(entries_[i].nid, entry.nid) && Eq(entries_[i].library, entry.library))
		{
			if (!Eq(entries_[i].export_name, entry.export_name))
			{
				// Record conflict on existing entry when sources disagree.
				entries_[i].status = ExportStatus::Conflict;
				return ExportCatalogResult::Conflict;
			}
			return ExportCatalogResult::Duplicate;
		}
	}
	if (count_ >= kExportCatalogMaxEntries)
	{
		return ExportCatalogResult::Full;
	}
	ExportEntry& slot = entries_[count_];
	slot              = {};
	if (!CopyField(slot.library, sizeof(slot.library), entry.library) ||
	    !CopyField(slot.module, sizeof(slot.module), entry.module[0] != '\0' ? entry.module : entry.library) ||
	    !CopyField(slot.export_name, sizeof(slot.export_name), entry.export_name) ||
	    !CopyField(slot.nid, sizeof(slot.nid), entry.nid) ||
	    !CopyField(slot.firmware, sizeof(slot.firmware), entry.firmware[0] != '\0' ? entry.firmware : "unknown") ||
	    !CopyField(slot.source, sizeof(slot.source), entry.source))
	{
		return ExportCatalogResult::InvalidArgument;
	}
	slot.library_version      = entry.library_version;
	slot.module_version_major = entry.module_version_major;
	slot.module_version_minor = entry.module_version_minor;
	slot.symbol_type          = entry.symbol_type;
	slot.status               = entry.status;
	slot.flags                = 0;
	++count_;
	return ExportCatalogResult::Ok;
}

ExportCatalogResult ExportCatalog::FindByNid(const char* nid, uint32_t* out_index) const noexcept
{
	if (IsEmpty(nid))
	{
		return ExportCatalogResult::InvalidArgument;
	}
	for (uint32_t i = 0; i < count_; ++i)
	{
		if (Eq(entries_[i].nid, nid))
		{
			if (out_index != nullptr)
			{
				*out_index = i;
			}
			return ExportCatalogResult::Ok;
		}
	}
	return ExportCatalogResult::NotFound;
}

ExportCatalogResult ExportCatalog::FindByName(const char* export_name, uint32_t* out_index) const noexcept
{
	if (IsEmpty(export_name))
	{
		return ExportCatalogResult::InvalidArgument;
	}
	for (uint32_t i = 0; i < count_; ++i)
	{
		if (Eq(entries_[i].export_name, export_name))
		{
			if (out_index != nullptr)
			{
				*out_index = i;
			}
			return ExportCatalogResult::Ok;
		}
	}
	return ExportCatalogResult::NotFound;
}

ExportCatalogResult ExportCatalog::FindByLibrary(const char* library, uint32_t* out_indices, uint32_t max_out,
                                                uint32_t* out_count) const noexcept
{
	if (IsEmpty(library) || out_count == nullptr)
	{
		return ExportCatalogResult::InvalidArgument;
	}
	*out_count = 0;
	for (uint32_t i = 0; i < count_; ++i)
	{
		if (!Eq(entries_[i].library, library))
		{
			continue;
		}
		if (out_indices != nullptr && *out_count < max_out)
		{
			out_indices[*out_count] = i;
		}
		++(*out_count);
	}
	return (*out_count > 0u) ? ExportCatalogResult::Ok : ExportCatalogResult::NotFound;
}

const ExportEntry* ExportCatalog::At(uint32_t index) const noexcept
{
	if (index >= count_)
	{
		return nullptr;
	}
	return &entries_[index];
}

uint32_t ExportCatalog::CountByStatus(ExportStatus status) const noexcept
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < count_; ++i)
	{
		if (entries_[i].status == status)
		{
			++n;
		}
	}
	return n;
}

namespace {

void SeedOne(ExportCatalog* cat, const char* library, const char* module, const char* name, const char* nid,
             const char* firmware, const char* source, ExportStatus status, ExportSymbolType type,
             uint16_t lib_ver) noexcept
{
	ExportEntry e {};
	std::snprintf(e.library, sizeof(e.library), "%s", library);
	std::snprintf(e.module, sizeof(e.module), "%s", module);
	std::snprintf(e.export_name, sizeof(e.export_name), "%s", name);
	std::snprintf(e.nid, sizeof(e.nid), "%s", nid);
	std::snprintf(e.firmware, sizeof(e.firmware), "%s", firmware);
	std::snprintf(e.source, sizeof(e.source), "%s", source);
	e.status          = status;
	e.symbol_type     = type;
	e.library_version = lib_ver;
	(void)cat->Insert(e);
}

} // namespace

uint32_t SeedPublicExportCatalog(ExportCatalog* catalog) noexcept
{
	if (catalog == nullptr)
	{
		return 0;
	}
	catalog->Clear();

	// All NID strings below appear as in-tree comments / unit-test fixtures.
	// No invented NIDs. Catalog never registers HLE trampolines.

	// --- Source A: public-table / stub vocabulary (ABI may still be incomplete).
	SeedOne(catalog, "libSceLibcInternal", "libSceLibcInternal", "vsnprintf", "Q2V+iqvjgC0", "any",
	        "seed:public-stub-names", ExportStatus::UnknownAbi, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceLibcInternal", "libSceLibcInternal", "strcasecmp", "AV6ipCNa4Rw", "any",
	        "seed:public-stub-names", ExportStatus::UnknownAbi, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceSaveData", "libSceSaveData", "sceSaveDataDirNameSearch", "dyIhnXq-0SM", "any",
	        "seed:public-stub-names", ExportStatus::UnknownAbi, ExportSymbolType::Func, 1);

	// --- Source B: names with in-tree HLE bodies (inventory flag only).
	SeedOne(catalog, "libSceAudioOut", "libSceAudioOut", "sceAudioOut2Initialize", "g2tViFIohHE", "any",
	        "seed:kyty-hle-name", ExportStatus::Implemented, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceAudioOut", "libSceAudioOut", "sceAudioOut2ContextCreate", "0x6o1VVAYSY", "any",
	        "seed:kyty-hle-name", ExportStatus::Implemented, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceAudioOut", "libSceAudioOut", "sceAudioOut2ContextDestroy", "on6ZH7Abo10", "any",
	        "seed:kyty-hle-name", ExportStatus::Implemented, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceSaveData", "libSceSaveData", "sceSaveDataCreateTransactionResource", "gjRZNnw0JPE", "any",
	        "seed:kyty-hle-name", ExportStatus::Implemented, ExportSymbolType::Func, 1);

	// --- Source C: named but unresolved / not claimed executable.
	SeedOne(catalog, "libSceNpTrophy2", "libSceNpTrophy2", "sceNpTrophy2CreateContext", "Fbshr7OQ6Q", "any",
	        "seed:kyty-observed-name", ExportStatus::Unimplemented, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceNpTrophy2", "libSceNpTrophy2", "sceNpTrophy2CreateHandle", "Gz1rmUZpROM", "any",
	        "seed:kyty-observed-name", ExportStatus::Unimplemented, ExportSymbolType::Func, 1);
	SeedOne(catalog, "libSceNp", "libSceNp", "sceNpEntitlementAccessGetAddcontEntitlementInfo", "xddD23+8TfQ", "any",
	        "seed:kyty-observed-name", ExportStatus::Required, ExportSymbolType::Func, 1);

	// --- Source D: packet-builder NID observed in fixtures (name not fully public; KNOWN NID only).
	SeedOne(catalog, "libSceAgc", "libSceAgc", "CommandBufferAllocateDwords", "LtTouSCZjHM", "any",
	        "seed:kyty-packet-fixture", ExportStatus::Known, ExportSymbolType::Func, 1);

	// --- Multi-source conflict: same library+NID, second source uses a different name.
	// First name is the in-tree association; second disagrees → existing entry becomes CONFLICT.
	SeedOne(catalog, "libSceSaveData", "libSceSaveData", "sceSaveDataTransactionCommit", "uW4vfTwMQVo", "any",
	        "seed:public-stub-names", ExportStatus::Known, ExportSymbolType::Func, 1);
	{
		ExportEntry e {};
		std::snprintf(e.library, sizeof(e.library), "%s", "libSceSaveData");
		std::snprintf(e.module, sizeof(e.module), "%s", "libSceSaveData");
		std::snprintf(e.export_name, sizeof(e.export_name), "%s", "sceSaveDataTransactionCommitAlt");
		std::snprintf(e.nid, sizeof(e.nid), "%s", "uW4vfTwMQVo");
		std::snprintf(e.firmware, sizeof(e.firmware), "%s", "any");
		std::snprintf(e.source, sizeof(e.source), "%s", "seed:fw-table-disagreement");
		e.status      = ExportStatus::Known;
		e.symbol_type = ExportSymbolType::Func;
		(void)catalog->Insert(e);
	}

	return catalog->Size();
}

} // namespace Kyty::DevTools
