#ifndef EMULATOR_INCLUDE_EMULATOR_GPUMEMORYFAULT_H_
#define EMULATOR_INCLUDE_EMULATOR_GPUMEMORYFAULT_H_

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Kyty::Emulator::GpuMemoryFault {

// The access-violation path can run from a host signal/exception handler.
// Dispatch therefore performs only an atomic callback lookup; adapters must
// remain process-live and keep their own handler path async-signal-safe.
using AccessViolationCallback       = bool (*)(uint64_t address);
using FaultHandlerInstalledCallback = void (*)();

struct Callbacks
{
	AccessViolationCallback       access_violation         = nullptr;
	FaultHandlerInstalledCallback fault_handler_installed = nullptr;
};

class Port final
{
public:
	[[nodiscard]] bool Install(const Callbacks& callbacks);
	[[nodiscard]] bool HandleAccessViolation(uint64_t address) const noexcept;
	void               NotifyFaultHandlerInstalled() const noexcept;

private:
	static bool AreComplete(const Callbacks& callbacks) noexcept;

	mutable std::mutex             m_install_mutex;
	Callbacks                      m_installed_callbacks {};
	std::atomic<const Callbacks*>  m_callbacks {nullptr};
};

Port& GetPort();

} // namespace Kyty::Emulator::GpuMemoryFault

#endif /* EMULATOR_INCLUDE_EMULATOR_GPUMEMORYFAULT_H_ */
