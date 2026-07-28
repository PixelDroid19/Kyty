#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRARYREGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRARYREGISTRATION_H_

#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>
#include <type_traits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// Complete guest-visible identity for one HLE library. Keeping this identity
// as data lets related standard/native libraries reuse one canonical export
// list without depending on namespace-scoped LIB_VERSION macro state.
struct LibraryIdentity
{
	const char* library;
	int         library_version;
	const char* module;
	int         module_version_major;
	int         module_version_minor;
};

template <typename Function>
void RegisterLibraryFunction(Loader::SymbolDatabase* symbols, const LibraryIdentity& identity, const char* nid, Function function,
                             const char32_t* debug_name)
{
	static_assert(std::is_pointer_v<Function>, "HLE exports must be function pointers");
	Loader::SymbolResolve symbol {};
	symbol.name                 = nid;
	symbol.library              = identity.library;
	symbol.library_version      = identity.library_version;
	symbol.module               = identity.module;
	symbol.module_version_major = identity.module_version_major;
	symbol.module_version_minor = identity.module_version_minor;
	symbol.type                 = Loader::SymbolType::Func;
	symbols->Add(symbol, reinterpret_cast<uint64_t>(function), debug_name);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRARYREGISTRATION_H_ */
