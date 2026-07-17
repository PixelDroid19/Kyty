#pragma once

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::LibC {

// Itanium __cxa_dynamic_cast address arithmetic (src2dst ABI constant).
// Shared by the HLE export and unit tests — no RTTI walk here.
[[nodiscard]] inline void* CxaDynamicCastApply(void* src, std::int64_t src2dst)
{
	if (src == nullptr)
	{
		return nullptr;
	}
	// src2dst >= 0: src is unique public non-virtual base of dst at that offset
	// from the most-derived object → result = src - src2dst.
	if (src2dst >= 0)
	{
		return static_cast<std::uint8_t*>(src) - static_cast<std::ptrdiff_t>(src2dst);
	}
	// -1: unspecified relationship (common free cast). Same-address optimistic
	// result when full RTTI is unavailable (guest type_info vtables unresolved).
	if (src2dst == -1)
	{
		return src;
	}
	// -2 not a public base; -3 multiple public bases.
	return nullptr;
}

} // namespace Kyty::Libs::LibC
