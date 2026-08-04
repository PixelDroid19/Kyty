#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_HLESYMBOLREGISTRY_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_HLESYMBOLREGISTRY_H_

#include "Kyty/Core/String.h"

#include <cstdint>
#include <initializer_list>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// The HLE side only needs an export sink. Keeping this contract outside the
// loader lets library modules register exports without depending on the
// loader's storage and lookup implementation.
enum class HleSymbolType
{
	Func,
	Object,
};

struct HleSymbolResolve
{
	String        name;
	String        library;
	int           library_version      = 0;
	String        module;
	int           module_version_major = 0;
	int           module_version_minor = 0;
	HleSymbolType type                 = HleSymbolType::Func;
};

class HleSymbolRegistry
{
public:
	virtual ~HleSymbolRegistry() = default;

	virtual void AddHle(const HleSymbolResolve& symbol, uint64_t vaddr) = 0;
	virtual void AddHle(const HleSymbolResolve& symbol, uint64_t vaddr, const String& dbg_name) = 0;
	virtual void AddHleAliases(HleSymbolResolve symbol, std::initializer_list<const char*> names, uint64_t vaddr,
	                          const String& dbg_name) = 0;
};

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_HLESYMBOLREGISTRY_H_ */
