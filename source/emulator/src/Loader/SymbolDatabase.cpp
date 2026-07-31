#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/Vector.h"

#include <array>
#include <cstring>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

namespace {

uint32_t RotateLeft(uint32_t value, uint32_t bits)
{
	return (value << bits) | (value >> (32u - bits));
}

std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size)
{
	uint32_t h0 = 0x67452301u;
	uint32_t h1 = 0xefcdab89u;
	uint32_t h2 = 0x98badcfeu;
	uint32_t h3 = 0x10325476u;
	uint32_t h4 = 0xc3d2e1f0u;

	std::vector<uint8_t> message(data, data + size);
	const uint64_t       bit_length = static_cast<uint64_t>(size) * 8u;
	message.push_back(0x80u);
	while ((message.size() % 64u) != 56u)
	{
		message.push_back(0u);
	}
	for (int shift = 56; shift >= 0; shift -= 8)
	{
		message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xffu));
	}

	for (size_t offset = 0; offset < message.size(); offset += 64u)
	{
		uint32_t words[80] {};
		for (int i = 0; i < 16; i++)
		{
			const size_t j = offset + static_cast<size_t>(i) * 4u;
			words[i]       = (static_cast<uint32_t>(message[j]) << 24u) | (static_cast<uint32_t>(message[j + 1]) << 16u) |
			                 (static_cast<uint32_t>(message[j + 2]) << 8u) | static_cast<uint32_t>(message[j + 3]);
		}
		for (int i = 16; i < 80; i++)
		{
			words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
		}

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;
		for (int i = 0; i < 80; i++)
		{
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20)
			{
				f = (b & c) | ((~b) & d);
				k = 0x5a827999u;
			} else if (i < 40)
			{
				f = b ^ c ^ d;
				k = 0x6ed9eba1u;
			} else if (i < 60)
			{
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdcu;
			} else
			{
				f = b ^ c ^ d;
				k = 0xca62c1d6u;
			}
			const uint32_t next = RotateLeft(a, 5) + f + e + k + words[i];
			e                   = d;
			d                   = c;
			c                   = RotateLeft(b, 30);
			b                   = a;
			a                   = next;
		}
		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest {};
	const uint32_t          state[] = {h0, h1, h2, h3, h4};
	for (size_t i = 0; i < 5; i++)
	{
		digest[i * 4]     = static_cast<uint8_t>(state[i] >> 24u);
		digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16u);
		digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8u);
		digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
	}
	return digest;
}

} // namespace

String EncodeNameAsNid(const char* name)
{
	static constexpr uint8_t suffix[]   = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
	static constexpr char    alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (name == nullptr)
	{
		return {};
	}

	const size_t         name_size = std::strlen(name);
	std::vector<uint8_t> input(name_size + sizeof(suffix));
	std::memcpy(input.data(), name, name_size);
	std::memcpy(input.data() + name_size, suffix, sizeof(suffix));
	const auto digest = Sha1(input.data(), input.size());

	uint64_t value = 0;
	std::memcpy(&value, digest.data(), sizeof(value));
	char nid[12] {};
	for (int i = 0; i < 10; i++)
	{
		nid[i] = alphabet[(value >> (58 - i * 6)) & 0x3fu];
	}
	nid[10] = alphabet[(value & 0xfu) * 4u];
	return String::FromUtf8(nid);
}

constexpr char32_t LIB_PREFIX[] = {0x0000006c, 0x00000069, 0x00000062, 0x00000053, 0x00000063, 0x00000065, 0};

static String CanonicalComponent(const String& value)
{
	// ELF metadata may include the syntactic libSce prefix. Removing that prefix
	// does not merge distinct API identities such as Agc/Graphics5 or Gnm/Graphics.
	return value.StartsWith(LIB_PREFIX) ? value.RemoveFirst(6) : value;
}

static String GenerateModuleIdentity(const SymbolResolve& s)
{
	const auto module = SymbolDatabase::CanonicalModuleName(s.module);
	return String::FromPrintf("[%s_v%d.%d]", module.C_Str(), s.module_version_major, s.module_version_minor);
}

String SymbolDatabase::CanonicalModuleName(const String& module)
{
	return CanonicalComponent(module);
}

String SymbolDatabase::GenerateName(const SymbolResolve& s)
{
	auto library = CanonicalComponent(s.library);
	auto module  = CanonicalModuleName(s.module);
	return String::FromPrintf("%s[%s_v%d][%s_v%d.%d][%s]", s.name.C_Str(), library.C_Str(), s.library_version, module.C_Str(),
	                          s.module_version_major, s.module_version_minor, Core::EnumName(s.type).C_Str());
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr)
{
	SymbolRecord r {};
	r.name  = GenerateName(s);
	r.vaddr = vaddr;
	r.type  = s.type;
	m_map.Put(r.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name)
{
	SymbolRecord r {};
	r.name     = GenerateName(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	r.type     = s.type;
	m_map.Put(r.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::AddAliases(SymbolResolve s, std::initializer_list<const char*> names, uint64_t vaddr, const String& dbg_name)
{
	for (const auto* name: names)
	{
		EXIT_IF(name == nullptr);
		s.name = name;
		Add(s, vaddr, dbg_name);
	}
}

void SymbolDatabase::AddHle(const SymbolResolve& s, uint64_t vaddr)
{
	AddHle(s, vaddr, {});
}

void SymbolDatabase::AddHle(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name)
{
	const String module = GenerateModuleIdentity(s);
	bool         known  = false;
	for (const auto& registered: m_hle_modules)
	{
		known = known || registered == module;
	}
	if (!known)
	{
		m_hle_modules.Add(module);
	}
	Add(s, vaddr, dbg_name);
}

void SymbolDatabase::AddHleAliases(SymbolResolve s, std::initializer_list<const char*> names, uint64_t vaddr, const String& dbg_name)
{
	for (const auto* name: names)
	{
		EXIT_IF(name == nullptr);
		s.name = name;
		AddHle(s, vaddr, dbg_name);
	}
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
	return m_symbols.IndexValid(index) ? &m_symbols.At(index) : nullptr;
}

const SymbolRecord* SymbolDatabase::FindByNid(const String& nid, SymbolType type) const
{
	if (nid.IsEmpty())
	{
		return nullptr;
	}
	const String suffix = String::FromPrintf("[%s]", Core::EnumName(type).C_Str());
	for (const auto& symbol: m_symbols)
	{
		if (symbol.name.StartsWith(nid + U"[") && symbol.name.EndsWith(suffix))
		{
			return &symbol;
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
	return m_symbols.IndexValid(index) ? &m_symbols.At(index) : nullptr;
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

bool SymbolDatabase::HleOwnsModule(const SymbolResolve& s) const
{
	const String module = GenerateModuleIdentity(s);
	for (const auto& registered: m_hle_modules)
	{
		if (registered == module)
		{
			return true;
		}
	}
	return false;
}

bool SymbolDatabase::HleOwnsCanonicalModule(const String& canonical_name) const
{
	for (const auto& module: m_hle_modules)
	{
		if (canonical_name.ContainsStr(module))
		{
			return true;
		}
	}
	return false;
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
