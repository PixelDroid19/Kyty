#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_CATALOG_EXPORTCATALOG_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_CATALOG_EXPORTCATALOG_H_

#include <cstdint>

namespace Kyty::DevTools {

// Local versioned export/NID inventory. Separate from HLE registration:
// catalog entries never install trampolines or mark guest imports executable.

enum class ExportStatus: uint8_t
{
	Known          = 0, // name/NID association recorded from a public source
	Required       = 1, // observed required by a workload (sanitized) but not implemented
	Implemented    = 2, // HLE exists with evidenced ABI (catalog only records the fact)
	Unimplemented  = 3, // known name/NID, no HLE, not claimed executable
	Conflict       = 4, // multi-source disagreement on name/NID/library pairing
	UnknownAbi     = 5  // known association; signature/return/effects not proven
};

enum class ExportSymbolType: uint8_t
{
	Unknown = 0,
	Func    = 1,
	Object  = 2
};

inline constexpr uint32_t kExportCatalogSchemaMajor = 1u;
inline constexpr uint32_t kExportCatalogSchemaMinor = 0u;
inline constexpr uint32_t kExportCatalogMaxEntries  = 512u; // seed capacity for phase-1

struct ExportEntry
{
	char             library[64]     = {};
	char             module[64]      = {};
	char             export_name[128] = {};
	char             nid[16]         = {}; // PS NID string (≤11) + NUL
	char             firmware[24]    = {}; // e.g. "3.20", "any", "unknown"
	char             source[48]      = {}; // provenance tag, not a free-form dump
	uint16_t         library_version = 0;
	uint8_t          module_version_major = 0;
	uint8_t          module_version_minor = 0;
	ExportSymbolType symbol_type     = ExportSymbolType::Unknown;
	ExportStatus     status          = ExportStatus::Unimplemented;
	uint32_t         flags           = 0; // reserved; must be 0 in v1
};

enum class ExportCatalogResult: uint8_t
{
	Ok              = 0,
	InvalidArgument = 1,
	Full            = 2,
	NotFound        = 3,
	Duplicate       = 4,
	Conflict        = 5
};

class ExportCatalog
{
public:
	ExportCatalog() noexcept = default;

	[[nodiscard]] uint32_t SchemaMajor() const noexcept { return kExportCatalogSchemaMajor; }
	[[nodiscard]] uint32_t SchemaMinor() const noexcept { return kExportCatalogSchemaMinor; }
	[[nodiscard]] uint32_t Size() const noexcept { return count_; }
	[[nodiscard]] uint32_t Capacity() const noexcept { return kExportCatalogMaxEntries; }

	void Clear() noexcept;

	// Insert a record. Never registers HLE. Rejects empty NID/name/library.
	// Same NID + same library + different export_name → Conflict entry or Conflict result.
	[[nodiscard]] ExportCatalogResult Insert(const ExportEntry& entry) noexcept;

	// Exact match helpers. out_index may be null.
	[[nodiscard]] ExportCatalogResult FindByNid(const char* nid, uint32_t* out_index) const noexcept;
	[[nodiscard]] ExportCatalogResult FindByName(const char* export_name, uint32_t* out_index) const noexcept;
	// Writes up to max_out matching library entries into out_indices; sets *out_count.
	[[nodiscard]] ExportCatalogResult FindByLibrary(const char* library, uint32_t* out_indices, uint32_t max_out,
	                                                uint32_t* out_count) const noexcept;

	[[nodiscard]] const ExportEntry* At(uint32_t index) const noexcept;
	[[nodiscard]] uint32_t CountByStatus(ExportStatus status) const noexcept;

	// True iff no entry with status Implemented is treated as auto-executable here
	// (catalog never holds function pointers — structural invariant for tests).
	[[nodiscard]] static bool IsNonExecutableInventory() noexcept { return true; }

private:
	ExportEntry entries_[kExportCatalogMaxEntries] {};
	uint32_t    count_ = 0;
};

// Load multi-source sanitized public seed (stub-name associations + observed Kyty names).
// Does not touch HLE registration tables.
[[nodiscard]] uint32_t SeedPublicExportCatalog(ExportCatalog* catalog) noexcept;

// Status name for diagnostics (compiled allowlist).
[[nodiscard]] const char* ExportStatusName(ExportStatus s) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_CATALOG_EXPORTCATALOG_H_ */
