#ifndef EMULATOR_INCLUDE_EMULATOR_PORTS_SOCKETEVENTPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_PORTS_SOCKETEVENTPORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

// Neutral contract between the kernel event queue (EVFILT_READ / EVFILT_WRITE
// on a socket) and the network HLE, so the kernel archive does not depend on
// the HLE one. The composition root installs the HLE implementation; before
// install no descriptor is a socket and the other calls fail closed.
namespace Kyty::Emulator::Ports {

// Runs once on the network watcher thread when the socket becomes ready in
// the watched direction.
using SocketReadyNotify = void (*)(uint64_t owner, uint64_t generation, int id, bool write);

struct SocketEventCallbacks
{
	bool (*is_socket)(int id)                                                                       = nullptr;
	int (*readiness)(int id, bool write, bool* ready, int64_t* data)                                = nullptr;
	int (*watch)(int id, bool write, uint64_t owner, uint64_t generation, SocketReadyNotify notify) = nullptr;
	void (*unwatch)(int id, bool write, uint64_t owner)                                             = nullptr;
};

class SocketEventPort final
{
public:
	// Error returned by Readiness and Watch before install.
	static constexpr int NOT_INSTALLED = -1;

	static void Install(SocketEventCallbacks callbacks);

	static bool IsSocket(int id);
	// `data` receives the pending receive byte count for reads.
	static int Readiness(int id, bool write, bool* ready, int64_t* data);
	// Arming the same (id, write, owner) again keeps a single watch.
	static int  Watch(int id, bool write, uint64_t owner, uint64_t generation, SocketReadyNotify notify);
	static void Unwatch(int id, bool write, uint64_t owner);
};

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PORTS_SOCKETEVENTPORT_H_ */
