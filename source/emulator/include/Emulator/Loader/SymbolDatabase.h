#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/HleSymbolRegistry.h"

#include <initializer_list>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

enum class SymbolType
{
	Unknown,
	Func,
	Object,
	TlsModule,
	NoType,
};

struct SymbolRecord
{
	String   name;
	String   dbg_name;
	uint64_t vaddr;
	SymbolType type = SymbolType::Unknown;
};

struct SymbolResolve
{
	String     name;
	String     library;
	int        library_version;
	String     module;
	int        module_version_major;
	int        module_version_minor;
	SymbolType type;
};

class SymbolDatabase: public Libs::HleSymbolRegistry
{
public:
	SymbolDatabase()          = default;
	virtual ~SymbolDatabase() = default;

	void AddHle(const Libs::HleSymbolResolve& s, uint64_t vaddr) override;
	void AddHle(const Libs::HleSymbolResolve& s, uint64_t vaddr, const String& dbg_name) override;
	void AddHleAliases(Libs::HleSymbolResolve s, std::initializer_list<const char*> names, uint64_t vaddr,
	                  const String& dbg_name) override;

	void Add(const SymbolResolve& s, uint64_t vaddr);
	void Add(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name);
	void AddAliases(SymbolResolve s, std::initializer_list<const char*> names, uint64_t vaddr, const String& dbg_name);
	void AddHle(const SymbolResolve& s, uint64_t vaddr);
	void AddHle(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name);
	void AddHleAliases(SymbolResolve s, std::initializer_list<const char*> names, uint64_t vaddr, const String& dbg_name);

	// Runtime resolution requires the complete canonical identity. It never falls back to a NID-only match.
	[[nodiscard]] const SymbolRecord* Find(const SymbolResolve& s) const;
	// Deliberately NID-only inspection API; runtime linking must use Find or FindByCanonicalName.
	[[nodiscard]] const SymbolRecord* FindByNid(const String& nid, SymbolType type) const;
	// Exact lookup by full GenerateName key (used for export-conflict scans).
	[[nodiscard]] const SymbolRecord* FindByCanonicalName(const String& canonical_name) const;
	[[nodiscard]] uint32_t            SymbolCount() const;
	[[nodiscard]] const SymbolRecord* SymbolAt(uint32_t index) const;
	[[nodiscard]] bool                HleOwnsModule(const SymbolResolve& s) const;
	[[nodiscard]] bool                HleOwnsCanonicalModule(const String& canonical_name) const;

	void DbgDump(const String& folder, const String& file_name);

	KYTY_CLASS_NO_COPY(SymbolDatabase);

	static String GenerateName(const SymbolResolve& s);
	static String CanonicalModuleName(const String& module);

private:
	Vector<SymbolRecord>            m_symbols;
	Core::Hashmap<String, uint32_t> m_map;
	Vector<String>                  m_hle_modules;
};

[[nodiscard]] String EncodeNameAsNid(const char* name);

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_ */
