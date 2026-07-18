#pragma once

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::LibC {

// Minimal MSVC/Orbis-style locale::_Locimp layout used by Dreaming Sarah's
// ctype facet lookup (use_facet path at guest 0x900134a80):
//   +0x00 vtable
//   +0x10 facet**
//   +0x18 facet count (lookup requires count > id)
//   +0x24 flag byte
//   +0x28 name-ish pointer
// std::locale itself is a single Locimp* at +0x00 (_sceLibcClassicLocale).
struct alignas(8) CxxLocimpLayout
{
	void**             vtable;
	void*              reserved_08;
	void**             facet_vec;
	std::uint64_t      facet_count;
	std::uint32_t      reserved_20;
	std::uint8_t       flag_24;
	std::uint8_t       pad_25[3];
	const char*        name;
};

struct alignas(8) CxxLocaleLayout
{
	CxxLocimpLayout* ptr;
};

// Itanium type_info (libstdc++): [0]=vtable, [8]=name (mangled; leading '*' = plain).
// __si_class_type_info adds [16]=base type_info*.
struct alignas(8) CxxTypeInfoLayout
{
	void**      vtable;
	const char* name;
};

struct alignas(8) CxxSiTypeInfoLayout
{
	void**                   vtable;
	const char*              name;
	const CxxTypeInfoLayout* base;
};

// Pre-assign ctype<char>::id = 1 and install a facet at index 1 with count 2 so
// the first classic-locale probe does not need runtime id allocation.
inline constexpr std::uint64_t kCxxCtypeCharId = 1;
inline constexpr std::uint64_t kCxxLocimpFacetCount = 2;

[[nodiscard]] inline bool CxxLocimpFacetLookupOk(const CxxLocimpLayout& locimp, std::uint64_t id)
{
	if (locimp.facet_vec == nullptr || locimp.facet_count <= id)
	{
		return false;
	}
	return locimp.facet_vec[id] != nullptr;
}

} // namespace Kyty::Libs::LibC
