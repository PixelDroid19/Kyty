#ifndef EMULATOR_INCLUDE_EMULATOR_PRESENTATIONSTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_PRESENTATIONSTATS_H_

#include <cstdint>
#include <mutex>

namespace Kyty::Emulator::PresentationStats {

// Graphics-owned presentation state exposed as immutable observation data.
// It carries no graphics handles and is safe for Kernel opt-in diagnostics.
struct Snapshot
{
	int      frame            = 0;
	uint64_t present          = 0;
	double   fps              = 0.0;
	bool     capture_ready    = false;
	bool     capture_dir_set  = false;
	bool     graphic_ready    = false;
	uint64_t ms_since_present = 0;
	uint64_t ms_since_frame   = 0;
};

using QueryCallback = bool (*)(void* context, Snapshot* out);

struct Callbacks
{
	void*         context = nullptr;
	QueryCallback query   = nullptr;
};

// A complete callback bundle is installed once and remains process-lifetime.
// Query fails closed before installation. The callback runs outside the port
// mutex, so the graphics adapter may re-enter Kernel without deadlocking.
class Port
{
public:
	[[nodiscard]] bool Install(const Callbacks& callbacks);
	[[nodiscard]] bool Query(Snapshot* out) const;

private:
	mutable std::mutex m_mutex;
	Callbacks          m_callbacks {};
};

Port& GetPort();

} // namespace Kyty::Emulator::PresentationStats

#endif /* EMULATOR_INCLUDE_EMULATOR_PRESENTATIONSTATS_H_ */
