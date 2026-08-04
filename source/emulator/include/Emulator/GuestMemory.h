#ifndef EMULATOR_INCLUDE_EMULATOR_GUESTMEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_GUESTMEMORY_H_

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Kyty::Emulator::GuestMemory {

enum class MappedRangeKind : uint8_t
{
	None,
	Physical,
	Flexible,
};

struct MappedRange
{
	MappedRangeKind kind = MappedRangeKind::None;
	uint64_t        base = 0;
	uint64_t        size = 0;
};

using QueryMappedRangeCallback = bool (*)(uint64_t address, uint64_t size, MappedRange* out);
using QueryProtectionCallback  = int (*)(void* address, void** start, void** end, int* protection);

struct Callbacks
{
	QueryMappedRangeCallback query_mapped_range = nullptr;
	QueryProtectionCallback  query_protection  = nullptr;
};

// Explicit guest-memory boundary used by graphics. The HLE memory subsystem
// installs callbacks once; graphics consumers never include Kernel/Memory.h.
class Port final
{
public:
	[[nodiscard]] bool Install(const Callbacks& callbacks);
	[[nodiscard]] bool QueryMappedRange(uint64_t address, uint64_t size, MappedRange* out) const noexcept;
	[[nodiscard]] int  QueryProtection(void* address, void** start, void** end, int* protection) const noexcept;

private:
	static bool AreComplete(const Callbacks& callbacks) noexcept;

	mutable std::mutex             m_install_mutex;
	Callbacks                      m_installed_callbacks {};
	std::atomic<const Callbacks*>  m_callbacks {nullptr};
};

Port& GetPort();

} // namespace Kyty::Emulator::GuestMemory

#endif /* EMULATOR_INCLUDE_EMULATOR_GUESTMEMORY_H_ */
