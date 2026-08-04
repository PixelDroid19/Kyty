#ifndef EMULATOR_INCLUDE_EMULATOR_PROFILER_H_
#define EMULATOR_INCLUDE_EMULATOR_PROFILER_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Profiling.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Profiler {

KYTY_SUBSYSTEM_DEFINE(Profiler);

} // namespace Kyty::Profiler

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PROFILER_H_ */
