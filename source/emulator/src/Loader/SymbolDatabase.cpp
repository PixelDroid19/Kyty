#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/Vector.h"


#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

constexpr char32_t LIB_PREFIX[] = {0x0000006c, 0x00000069, 0x00000062, 0x00000053, 0x00000063, 0x00000065, 0};
constexpr char32_t LIB_OLD_4[]  = {0x00000047, 0x0000006e, 0x0000006d, 0};
constexpr char32_t LIB_NEW_4[]  = {0x00000047, 0x00000072, 0x00000061, 0x00000070, 0x00000068, 0x00000069, 0x00000063, 0x00000073, 0};
constexpr char32_t LIB_OLD_5[]  = {0x00000041, 0x00000067, 0x00000063, 0};
constexpr char32_t LIB_NEW_5[]  = {0x00000047, 0x00000072, 0x00000061, 0x00000070, 0x00000068,
                                   0x00000069, 0x00000063, 0x00000073, 0x00000035, 0};

static String update_name(const String& str)
{
	auto ret = (str.StartsWith(LIB_PREFIX) ? str.RemoveFirst(6) : str);
	return ret.ReplaceStr(LIB_OLD_4, LIB_NEW_4).ReplaceStr(LIB_OLD_5, LIB_NEW_5);
}

String SymbolDatabase::GenerateName(const SymbolResolve& s)
{
	auto library = update_name(s.library);
	auto module  = update_name(s.module);
	return String::FromPrintf("%s[%s_v%d][%s_v%d.%d][%s]", s.name.C_Str(), library.C_Str(), s.library_version, module.C_Str(),
	                          s.module_version_major, s.module_version_minor, Core::EnumName(s.type).C_Str());
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr)
{
	SymbolRecord r {};
	r.name  = GenerateName(s);
	r.vaddr = vaddr;
	m_map.Put(r.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name)
{
	SymbolRecord r {};
	r.name     = GenerateName(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	m_map.Put(r.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::DbgDump(const String& folder, const String& file_name)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + file_name);

	for (const auto& sym: m_symbols)
	{
		f.Printf("%" PRIx64 " %s\n", sym.vaddr, sym.name.C_Str());
	}

	f.Close();
}

const SymbolRecord* SymbolDatabase::Find(const SymbolResolve& s) const
{
	const String key   = GenerateName(s);
	const auto   index = m_map.Get(key, decltype(m_symbols)::INVALID_INDEX);
	if (m_symbols.IndexValid(index))
	{
		return &m_symbols.At(index);
	}

	// Fallback when the hashmap misses a key that is present in m_symbols.
	// Observed for some Gen5 NIDs (e.g. NpTrophy2 Fbshr7OQ6Q): Put/Get key
	// mismatch while DbgDump still lists the record. Prefer the vector scan
	// over a silent unresolved PLT so HLE registrations remain authoritative.
	for (const auto& sym: m_symbols)
	{
		if (sym.name == key)
		{
			return &sym;
		}
	}

	// Last resort: match on NID-only prefix (before first '[').
	const uint32_t bracket = key.FindIndex(U'[');
	if (bracket != Core::STRING_INVALID_INDEX && bracket > 0)
	{
		const String nid = key.Left(bracket);
		for (const auto& sym: m_symbols)
		{
			if (sym.name.StartsWith(nid + U"["))
			{
				return &sym;
			}
		}
	}

	return nullptr;
}

const SymbolRecord* SymbolDatabase::FindByCanonicalName(const String& canonical_name) const
{
	if (canonical_name.IsEmpty())
	{
		return nullptr;
	}
	const auto index = m_map.Get(canonical_name, decltype(m_symbols)::INVALID_INDEX);
	if (m_symbols.IndexValid(index))
	{
		return &m_symbols.At(index);
	}
	for (const auto& sym: m_symbols)
	{
		if (sym.name == canonical_name)
		{
			return &sym;
		}
	}
	return nullptr;
}

uint32_t SymbolDatabase::SymbolCount() const
{
	return m_symbols.Size();
}

const SymbolRecord* SymbolDatabase::SymbolAt(uint32_t index) const
{
	if (!m_symbols.IndexValid(index))
	{
		return nullptr;
	}
	return &m_symbols.At(index);
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
