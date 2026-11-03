#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GAMEPATCH_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GAMEPATCH_H_

#include "Kyty/Core/String.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

struct Program;

struct GamePatchWrite
{
	uint64_t             offset = 0;
	std::vector<uint8_t> off;
	std::vector<uint8_t> on;
};

struct GamePatchPlan
{
	Core::String                title_id;
	Core::String                version;
	Core::String                process;
	std::vector<Core::String>   mods;
	std::vector<GamePatchWrite> writes;
};

// Reads a mods file: {id, version, process, mods: [{name, enabled, memory: [{offset, off, on}]}]}.
// Mods with enabled=false add nothing to the plan. Returns the reason on rejection, empty on success.
Core::String GamePatchParse(const Core::String& text, GamePatchPlan* plan);

// Applies the enabled mods of the file to the process they name, before any guest code runs.
// A file that does not match the loaded game stops the run: it was requested explicitly.
void GamePatchApply(const Core::String& file_name, const Vector<Program*>& programs);

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GAMEPATCH_H_ */
