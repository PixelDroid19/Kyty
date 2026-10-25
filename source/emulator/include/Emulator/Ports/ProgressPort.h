#ifndef EMULATOR_INCLUDE_EMULATOR_PORTS_PROGRESSPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_PORTS_PROGRESSPORT_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

// Neutral contract between the runtime's long-running host operations and
// the supervised-run telemetry. Each lane is one operation kind on one host
// thread; the supervisor reports a stall when a lane stays busy past its
// threshold. A GuestThread lane spans a guest thread's life instead and
// records whether it is parked in a blocking guest wait. Calls before install
// (standalone runs) are safe no-ops.
namespace Kyty::Emulator::Ports {

enum class ProgressLane: uint8_t
{
	GraphicsCommands = 0,
	ComputeCommands  = 1,
	FenceWait        = 2,
	Present          = 3,
	GuestThread      = 4,
	Count            = 5
};

// Blocking guest waits that only another thread or the emulator can end.
enum class GuestWait: uint8_t
{
	Mutex,
	Cond,
	Semaphore,
	EventFlag,
	EventQueue,
	Join,
	Rwlock,
	Address
};

class ProgressPort final
{
public:
	struct Sink
	{
		void (*begin)(ProgressLane lane) = nullptr;
		void (*end)(ProgressLane lane)   = nullptr;
		void (*begin_compile)()          = nullptr;
		void (*end_compile)()            = nullptr;
		void (*begin_wait)(GuestWait wait) = nullptr;
		void (*end_wait)()                 = nullptr;
	};

	static void Install(Sink sink);
	static void Begin(ProgressLane lane);
	static void End(ProgressLane lane);
	static void BeginCompile();
	static void EndCompile();
	static void BeginGuestWait(GuestWait wait);
	static void EndGuestWait();
};

class ScopedProgress final
{
public:
	explicit ScopedProgress(ProgressLane lane): m_lane(lane) { ProgressPort::Begin(lane); }
	~ScopedProgress() { ProgressPort::End(m_lane); }

	ScopedProgress(const ScopedProgress&)            = delete;
	ScopedProgress& operator=(const ScopedProgress&) = delete;

private:
	ProgressLane m_lane;
};

// Marks the command lane open on this thread as compiling a pipeline, so a
// long synchronous compile is judged against compile thresholds instead of
// being reported as a stuck command batch.
class ScopedShaderCompile final
{
public:
	ScopedShaderCompile() { ProgressPort::BeginCompile(); }
	~ScopedShaderCompile() { ProgressPort::EndCompile(); }

	ScopedShaderCompile(const ScopedShaderCompile&)            = delete;
	ScopedShaderCompile& operator=(const ScopedShaderCompile&) = delete;
};

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PORTS_PROGRESSPORT_H_ */
