#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRECORDER_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRECORDER_H_

#include "Emulator/Common.h"
#include "Emulator/Libs/TrophyRules.h"

#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Trophies {

// Feeds one event the title posted through the Universal Data System to its trophy rules and saves the stats and
// unlocks it changes. The first call loads the title's trophy set from /app0/sce_sys and the user's saved progress.
void TrophyRecordEvent(const std::string& event, const EventValues& values);

} // namespace Kyty::Libs::Trophies

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRECORDER_H_ */
