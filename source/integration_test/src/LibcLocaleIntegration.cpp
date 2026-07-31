#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/CxxString.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Kyty;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "libc-locale integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

const Loader::SymbolRecord* FindLibcSymbol(Loader::SymbolDatabase& symbols, const char* nid, Loader::SymbolType type)
{
	Loader::SymbolResolve resolve {};
	resolve.name                 = nid;
	resolve.library              = U"libc";
	resolve.library_version      = 1;
	resolve.module               = U"libc";
	resolve.module_version_major = 1;
	resolve.module_version_minor = 1;
	resolve.type                 = type;
	return symbols.Find(resolve);
}

} // namespace

int main()
{
	Loader::SymbolDatabase symbols;
	Expect(Libs::Init(U"libc_1", &symbols), "libc HLE registration must succeed");

	const auto* getpctype_record = FindLibcSymbol(symbols, "sUP1hBaouOw", Loader::SymbolType::Func);
	const auto* getptimes_record = FindLibcSymbol(symbols, "8xXiEPby8h8", Loader::SymbolType::Func);
	const auto* time_put_record = FindLibcSymbol(symbols, "j9LU8GsuEGw", Loader::SymbolType::Func);
	const auto* locale_record    = FindLibcSymbol(symbols, "Qoo175Ig+-k", Loader::SymbolType::Object);
	const auto* time_put_vtable_record = FindLibcSymbol(symbols, "OwfBD-2nhJQ", Loader::SymbolType::Object);
	const auto* generic_category_record = FindLibcSymbol(symbols, "YxwfcCH5Q0I", Loader::SymbolType::Func);
	Expect(getpctype_record != nullptr, "_Getpctype must resolve");
	Expect(getptimes_record != nullptr, "_Getptimes must resolve");
	Expect(time_put_record != nullptr, "the narrow time-output entry point must resolve");
	Expect(locale_record != nullptr, "the classic locale object must resolve");
	Expect(time_put_vtable_record != nullptr, "the narrow time-output facet vtable must resolve");
	Expect(generic_category_record != nullptr, "the generic error category must resolve");

	using GetpctypeFn = const std::uint16_t*(KYTY_SYSV_ABI*)();
	const auto getpctype = reinterpret_cast<GetpctypeFn>(getpctype_record->vaddr);
	const auto* table     = getpctype();
	Expect(table != nullptr, "_Getpctype must return a table");
	Expect((table['S'] & 0x002u) != 0, "uppercase characters must use the guest mask");
	Expect((table['s'] & 0x010u) != 0, "lowercase characters must use the guest mask");
	Expect((table['9'] & 0x021u) == 0x021u, "digits must include decimal and hexadecimal masks");
	Expect((table[' '] & 0x144u) != 0, "spaces must satisfy guest whitespace checks");

	const auto* locale = reinterpret_cast<const Libs::LibC::CxxLocaleLayout*>(locale_record->vaddr);
	Expect(locale->ptr != nullptr, "the classic locale must reference its implementation");
	Expect(Libs::LibC::CxxLocimpFacetLookupOk(*locale->ptr, Libs::LibC::kCxxCtypeCharId),
	       "the classic locale must contain the narrow-character facet");

	const auto* facet =
	    static_cast<const Libs::LibC::CxxCtypeFacetLayout*>(locale->ptr->facet_vec[Libs::LibC::kCxxCtypeCharId]);
	Expect(facet->table == table, "the C API and C++ facet must share one classification table");

	using GetptimesFn = const char* const*(KYTY_SYSV_ABI*)();
	const auto getptimes = reinterpret_cast<GetptimesFn>(getptimes_record->vaddr);
	const auto* times     = getptimes();
	Expect(times != nullptr, "_Getptimes must return stable locale data");
	Expect(std::strcmp(times[0], ":AM:PM") == 0, "the time locale must expose AM and PM markers");
	Expect(std::strcmp(times[1], ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday") == 0,
	       "the time locale must expose weekday names");
	Expect(std::strcmp(times[4],
	                   ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:"
	                   "Oct:October:Nov:November:Dec:December") == 0,
	       "the time locale must expose month names");
	Expect(std::strcmp(times[7], "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p") == 0,
	       "the time locale must expose formatting patterns");
	Expect(times[17][0] == '\0' && times[20][0] == '\0', "optional time-locale fields must be empty strings");

	struct FacetBase
	{
		void**        vtable;
		std::uint32_t references;
		std::uint32_t reserved;
	};
	auto** time_put_vtable = reinterpret_cast<void**>(time_put_vtable_record->vaddr);
	Expect(time_put_vtable[4] != nullptr && time_put_vtable[5] != nullptr, "the facet vtable must implement reference ownership");

	FacetBase facet_base {time_put_vtable + 2, 0, 0};
	using IncrefFn = void(KYTY_SYSV_ABI*)(FacetBase*);
	using DecrefFn = FacetBase*(KYTY_SYSV_ABI*)(FacetBase*);
	reinterpret_cast<IncrefFn>(time_put_vtable[4])(&facet_base);
	Expect(facet_base.references == 1, "facet incref must retain the object");
	Expect(reinterpret_cast<DecrefFn>(time_put_vtable[5])(&facet_base) == &facet_base,
	       "the final facet decref must return the object for deletion");
	Expect(facet_base.references == 0, "facet decref must release the retained reference");

	using CategoryFn = const void*(KYTY_SYSV_ABI*)();
	const auto category = reinterpret_cast<CategoryFn>(generic_category_record->vaddr)();
	Expect(category != nullptr, "the generic error category must return an object");
	auto** category_vtable = *reinterpret_cast<void***>(const_cast<void*>(category));
	using MessageFn = Libs::LibC::CxxStringLayout*(KYTY_SYSV_ABI*)(Libs::LibC::CxxStringLayout*, const void*, std::int32_t);
	Libs::LibC::CxxStringLayout message {};
	const auto message_result = reinterpret_cast<MessageFn>(category_vtable[3])(&message, category, 2);
	Expect(message_result == &message, "error-category message must return the hidden result object");
	Expect(message.size != 0, "error-category message must describe a known error");
	Expect(std::strlen(Libs::LibC::CxxStringData(message)) == message.size, "error-category message must expose a valid guest string");
	return 0;
}
