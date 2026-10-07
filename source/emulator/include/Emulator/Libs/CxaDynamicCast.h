#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Kyty::Libs::LibC {

// Itanium __cxa_dynamic_cast address arithmetic (src2dst ABI constant).
// Fallback used when the guest RTTI cannot be walked (unknown type_info kind).
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

// Vtables the type_info objects point at. A type_info vptr is either the vtable
// start (host-built type_info) or its address point (guest relocation +0x10).
struct CxaTypeInfoVtables
{
	const void* const* class_type   = nullptr;
	const void* const* si_class     = nullptr;
	const void* const* vmi_class    = nullptr;
};

namespace CxaDynamicCastDetail {

enum class Kind
{
	Unknown,
	Class,
	Single,
	Multiple,
};

struct BaseInfo
{
	const void*  type;
	std::int64_t offset_flags;
};

struct VmiHeader
{
	const void*   vptr;
	const char*   name;
	std::uint32_t flags;
	std::uint32_t base_count;
};

constexpr std::int64_t kVirtualMask = 0x1;
constexpr std::int64_t kPublicMask  = 0x2;
constexpr int          kOffsetShift = 8;
constexpr int          kMaxDepth    = 32;
constexpr int          kMaxNodes    = 128; // runs on the guest stack

struct Node
{
	const void*         type;
	const std::uint8_t* ptr;
	bool                step_public; // derivation from the parent node is public
	bool                is_public;   // every derivation from the most-derived object is public
	int                 parent;
};

inline bool VptrIs(const void* vptr, const void* const* vtable)
{
	return vtable != nullptr && (vptr == static_cast<const void*>(vtable) || vptr == static_cast<const void*>(vtable + 2));
}

inline Kind KindOf(const CxaTypeInfoVtables& vtables, const void* type)
{
	if (type == nullptr)
	{
		return Kind::Unknown;
	}
	const void* vptr = *static_cast<const void* const*>(type);
	if (VptrIs(vptr, vtables.class_type))
	{
		return Kind::Class;
	}
	if (VptrIs(vptr, vtables.si_class))
	{
		return Kind::Single;
	}
	if (VptrIs(vptr, vtables.vmi_class))
	{
		return Kind::Multiple;
	}
	return Kind::Unknown;
}

// Type identity: same object, or same mangled name (type_info may be duplicated
// across modules). A leading '*' marks a name that must compare by address.
inline bool SameType(const void* a, const void* b)
{
	if (a == b)
	{
		return true;
	}
	if (a == nullptr || b == nullptr)
	{
		return false;
	}
	const char* name_a = static_cast<const char* const*>(a)[1];
	const char* name_b = static_cast<const char* const*>(b)[1];
	if (name_a == nullptr || name_b == nullptr || name_a[0] == '*' || name_b[0] == '*')
	{
		return false;
	}
	return std::strcmp(name_a, name_b) == 0;
}

// Flattens the subobject tree of `type` at `ptr` (pre-order, root first).
// Returns false when the hierarchy uses a type_info kind this walker does not
// know, or exceeds the size limits.
inline bool Collect(const CxaTypeInfoVtables& vtables, const void* type, const std::uint8_t* ptr, bool step_public, int parent,
                    int depth, Node* nodes, int* count)
{
	if (depth > kMaxDepth || *count >= kMaxNodes)
	{
		return false;
	}
	const bool is_public = step_public && (parent < 0 || nodes[parent].is_public);
	const int  self      = (*count)++;
	nodes[self]          = Node {type, ptr, step_public, is_public, parent};

	switch (KindOf(vtables, type))
	{
		case Kind::Class: return true;
		case Kind::Single:
		{
			const void* base = static_cast<const void* const*>(type)[2];
			return Collect(vtables, base, ptr, true, self, depth + 1, nodes, count);
		}
		case Kind::Multiple:
		{
			const auto* header = static_cast<const VmiHeader*>(type);
			const auto* bases  = reinterpret_cast<const BaseInfo*>(header + 1);
			for (std::uint32_t i = 0; i < header->base_count; i++)
			{
				const std::int64_t flags  = bases[i].offset_flags;
				std::ptrdiff_t     offset = static_cast<std::ptrdiff_t>(flags >> kOffsetShift);
				if ((flags & kVirtualMask) != 0)
				{
					// Virtual base: the offset locates the base offset in this
					// subobject's vtable.
					const auto* vtable = *reinterpret_cast<const std::uint8_t* const*>(ptr);
					offset             = *reinterpret_cast<const std::ptrdiff_t*>(vtable + offset);
				}
				if (!Collect(vtables, bases[i].type, ptr + offset, (flags & kPublicMask) != 0, self, depth + 1, nodes, count))
				{
					return false;
				}
			}
			return true;
		}
		case Kind::Unknown: return false;
	}
	return false;
}

// True when `descendant` is a base subobject of `ancestor` reached through
// public derivations only.
inline bool DerivesPublicly(const Node* nodes, int descendant, int ancestor)
{
	for (int i = descendant; i >= 0; i = nodes[i].parent)
	{
		if (i == ancestor)
		{
			return true;
		}
		if (!nodes[i].step_public)
		{
			return false;
		}
	}
	return false;
}

// Unique T object that has the source subobject as a public base, or null.
inline const std::uint8_t* FindDowncast(const Node* nodes, int count, const void* src, const void* src_type, const void* dst_type)
{
	const std::uint8_t* found = nullptr;
	for (int i = 0; i < count; i++)
	{
		if (!SameType(nodes[i].type, dst_type))
		{
			continue;
		}
		bool derives = false;
		for (int j = 0; j < count && !derives; j++)
		{
			derives = nodes[j].ptr == src && SameType(nodes[j].type, src_type) && DerivesPublicly(nodes, j, i);
		}
		if (!derives)
		{
			continue;
		}
		if (found != nullptr && found != nodes[i].ptr)
		{
			return nullptr;
		}
		found = nodes[i].ptr;
	}
	return found;
}

// Unambiguous public T base of the most-derived object, or null.
inline const std::uint8_t* FindCrosscast(const Node* nodes, int count, const void* dst_type)
{
	const std::uint8_t* found      = nullptr;
	bool                any_public = false;
	for (int i = 0; i < count; i++)
	{
		if (!SameType(nodes[i].type, dst_type))
		{
			continue;
		}
		if (found != nullptr && found != nodes[i].ptr)
		{
			return nullptr;
		}
		found      = nodes[i].ptr;
		any_public = any_public || nodes[i].is_public;
	}
	return any_public ? found : nullptr;
}

} // namespace CxaDynamicCastDetail

// Full Itanium __dynamic_cast: walks the guest RTTI of the most-derived object
// ([expr.dynamic.cast]/8: downcast to a unique T derived from the source
// subobject, else cross-cast to an unambiguous public T base). Falls back to
// the src2dst arithmetic when the RTTI kind is unknown.
[[nodiscard]] inline void* CxaDynamicCastResolve(void* src, const void* src_type, const void* dst_type, std::int64_t src2dst,
                                                 const CxaTypeInfoVtables& vtables)
{
	namespace D = CxaDynamicCastDetail;
	if (src == nullptr)
	{
		return nullptr;
	}
	auto*       src_bytes    = static_cast<std::uint8_t*>(src);
	const auto* vtable       = *static_cast<const std::int64_t* const*>(src);
	const auto  offset       = static_cast<std::ptrdiff_t>(vtable[-2]);
	const void* dynamic_type = reinterpret_cast<const void* const*>(vtable)[-1];

	D::Node nodes[D::kMaxNodes];
	int     count = 0;
	if (src_type == nullptr || dst_type == nullptr || !D::Collect(vtables, dynamic_type, src_bytes + offset, true, -1, 0, nodes, &count))
	{
		return CxaDynamicCastApply(src, src2dst);
	}

	// Source subobject: the static type at the source address.
	bool source_found  = false;
	bool source_public = false;
	for (int i = 0; i < count; i++)
	{
		if (nodes[i].ptr == src_bytes && D::SameType(nodes[i].type, src_type))
		{
			source_found  = true;
			source_public = source_public || nodes[i].is_public;
		}
	}
	if (!source_found)
	{
		return nullptr;
	}

	const std::uint8_t* result = D::FindDowncast(nodes, count, src, src_type, dst_type);
	if (result == nullptr && source_public)
	{
		result = D::FindCrosscast(nodes, count, dst_type);
	}
	return result == nullptr ? nullptr : src_bytes + (result - src_bytes);
}

} // namespace Kyty::Libs::LibC
