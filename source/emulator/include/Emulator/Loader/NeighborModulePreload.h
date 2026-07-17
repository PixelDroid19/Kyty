#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_NEIGHBORMODULEPRELOAD_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_NEIGHBORMODULEPRELOAD_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

class RuntimeLinker;

// Unsafe bring-up helper: discover neighboring shared modules next to the main
// program so real PRX exports can win over missing-import stubs. Never invents
// symbols; only loads files that open as valid ELF shared objects.
namespace NeighborModulePreload {

// Host paths under guest_root (directory that contains eboot.bin / main.elf).
// Scans, without loading:
//   <root>/sce_module/*
//   <root>/Media/Modules/*
//   <root>/modules/*
// Accepts extensions: .prx .sprx .elf (case-insensitive). Skips eboot.bin and
// non-files. Paths are absolute host paths (or as given by guest_root).
Vector<Core::String> DiscoverCandidates(const Core::String& guest_root);

// Soft-load discovered candidates into the linker when AllowPrxPreload() is
// true. Invalid ELFs are skipped (logged once per path). Already-loaded paths
// are skipped. Returns number of programs successfully loaded.
// When AllowPrxPreload() is false, returns 0 and does not scan.
int PreloadInto(RuntimeLinker* rt, const Core::String& main_program_path);

// True if path looks like a main executable name that must not be re-loaded
// as a neighbor module.
[[nodiscard]] bool IsMainExecutableName(const Core::String& file_name);

} // namespace NeighborModulePreload

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_NEIGHBORMODULEPRELOAD_H_ */
