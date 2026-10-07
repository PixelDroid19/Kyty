#include "Emulator/Libs/CxxRtti.h"

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/CxxLocale.h"

#include <array>
#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC {

namespace {

// Itanium mangling of every fundamental type the C++ runtime provides type_info for.
constexpr std::array<const char*, 23> kFundamentalCodes = {"v", "Dn", "b", "w", "Ds", "Di", "c", "a", "h", "s", "t", "i",
                                                           "j", "l", "m", "x", "y", "n", "o", "f", "d", "e", "g"};
constexpr size_t                      kCount            = kFundamentalCodes.size();
constexpr uint32_t                    kConstMask        = 0x1; // __pbase_type_info::__const_mask

// __pbase_type_info: type_info, then __flags and __pointee.
struct alignas(8) CxxPointerTypeInfoLayout
{
	void**                   vtable;
	const char*              name;
	uint32_t                 flags;
	uint32_t                 padding;
	const CxxTypeInfoLayout* pointee;
};

static_assert(sizeof(CxxPointerTypeInfoLayout) == 32);

KYTY_SYSV_ABI void* VtableNoop(void* self)
{
	return self;
}

KYTY_SYSV_ABI const char* BadWeakPtrWhat(void* /*self*/)
{
	return "bad_weak_ptr";
}

// An Itanium vtable: offset-to-top, type_info, then the virtual slots. Objects
// point at the first slot (the address point).
struct Vtable
{
	std::array<void*, 8> entries {};

	explicit Vtable(void* last_slot = reinterpret_cast<void*>(&VtableNoop))
	{
		for (size_t i = 2; i < entries.size(); i++)
		{
			entries[i] = reinterpret_cast<void*>(&VtableNoop);
		}
		entries[4] = last_slot;
	}

	void** AddressPoint() { return &entries[2]; }
};

// std::bad_weak_ptr: complete and deleting destructors, then what().
Vtable g_bad_weak_ptr_vtable(reinterpret_cast<void*>(&BadWeakPtrWhat));
Vtable g_fundamental_vtable;
Vtable g_pointer_vtable;
Vtable g_enum_vtable;

struct Tables
{
	std::array<std::string, kCount>              value_names;
	std::array<std::string, kCount>              pointer_names;
	std::array<std::string, kCount>              const_pointer_names;
	std::array<std::string, kCount * 3>          symbols;
	std::array<CxxTypeInfoLayout, kCount>        values {};
	std::array<CxxPointerTypeInfoLayout, kCount> pointers {};
	std::array<CxxPointerTypeInfoLayout, kCount> const_pointers {};
	std::vector<CxxRttiObject>                   objects;
};

void AddFundamental(Tables* t, size_t i)
{
	const std::string code = kFundamentalCodes[i];
	t->value_names[i]         = code;
	t->pointer_names[i]       = "P" + code;
	t->const_pointer_names[i] = "PK" + code;
	t->values[i]              = {g_fundamental_vtable.AddressPoint(), t->value_names[i].c_str()};
	t->pointers[i]            = {g_pointer_vtable.AddressPoint(), t->pointer_names[i].c_str(), 0, 0, &t->values[i]};
	t->const_pointers[i] = {g_pointer_vtable.AddressPoint(), t->const_pointer_names[i].c_str(), kConstMask, 0, &t->values[i]};
	t->symbols[i * 3]     = "_ZTI" + code;
	t->symbols[i * 3 + 1] = "_ZTIP" + code;
	t->symbols[i * 3 + 2] = "_ZTIPK" + code;
	t->objects.push_back({t->symbols[i * 3].c_str(), &t->values[i]});
	t->objects.push_back({t->symbols[i * 3 + 1].c_str(), &t->pointers[i]});
	t->objects.push_back({t->symbols[i * 3 + 2].c_str(), &t->const_pointers[i]});
}

Tables* BuildTables()
{
	auto* t = new Tables;
	for (size_t i = 0; i < kCount; i++)
	{
		AddFundamental(t, i);
	}
	t->objects.push_back({"_ZTVN10__cxxabiv123__fundamental_type_infoE", g_fundamental_vtable.entries.data()});
	t->objects.push_back({"_ZTVN10__cxxabiv116__enum_type_infoE", g_enum_vtable.entries.data()});
	t->objects.push_back({"_ZTVSt12bad_weak_ptr", g_bad_weak_ptr_vtable.entries.data()});
	return t;
}

} // namespace

const std::vector<CxxRttiObject>& CxxRttiObjects()
{
	// Built once and never freed: guest code keeps the addresses for its lifetime.
	static const Tables* tables = BuildTables();
	return tables->objects;
}

} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED
