#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_SYMBOLREGISTRY_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_SYMBOLREGISTRY_H_

#include "Kyty/Core/String.h"

#include <cstdint>
#include <initializer_list>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

// Neutral export sink shared by HLE providers and the loader. Keeping this
// contract in Hle prevents Graphics and other providers from depending on the
// Libs aggregate just to publish guest symbols.
enum class HleSymbolType
{
	Func,
	Object,
};

struct HleSymbolResolve
{
	Core::String  name;
	Core::String  library;
	int           library_version      = 0;
	Core::String  module;
	int           module_version_major = 0;
	int           module_version_minor = 0;
	HleSymbolType type                 = HleSymbolType::Func;
};

class HleSymbolRegistry
{
public:
	virtual ~HleSymbolRegistry() = default;

	virtual void AddHle(const HleSymbolResolve& symbol, uint64_t vaddr) = 0;
	virtual void AddHle(const HleSymbolResolve& symbol, uint64_t vaddr, const Core::String& dbg_name) = 0;
	virtual void AddHleAliases(HleSymbolResolve symbol, std::initializer_list<const char*> names, uint64_t vaddr,
	                          const Core::String& dbg_name) = 0;
};

} // namespace Kyty::Hle

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_SYMBOLREGISTRY_H_ */
