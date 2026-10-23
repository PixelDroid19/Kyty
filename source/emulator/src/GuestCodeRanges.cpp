#include "Emulator/GuestCodeRanges.h"

#include "Kyty/Core/DbgAssert.h"

#include <atomic>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::GuestCodeRanges {
namespace {

constexpr uint64_t kPageShift = 12;
constexpr uint64_t kPageMask  = (uint64_t {1} << kPageShift) - 1;

// Each slot packs the first and the end page of one range into a single word,
// so a lock-free reader always sees both bounds of the same range. 32-bit page
// numbers reach 16 TiB, far above where the loader places guest programs. A
// process maps few programs; a fixed table keeps lookups free of locks and
// allocation.
constexpr size_t kCapacity = 256;

std::atomic<uint64_t> g_ranges[kCapacity] {};
std::atomic<size_t>   g_used {0};
std::mutex            g_writer_mutex;

uint64_t Pack(uint64_t begin, uint64_t end)
{
	const uint64_t first_page = begin >> kPageShift;
	const uint64_t end_page   = (end + kPageMask) >> kPageShift;
	EXIT_IF(begin >= end || end_page > UINT32_MAX);
	return (first_page << 32u) | end_page;
}

} // namespace

void Add(uint64_t begin, uint64_t end) noexcept
{
	const uint64_t  packed = Pack(begin, end);
	std::lock_guard lock(g_writer_mutex);
	const size_t    used = g_used.load(std::memory_order_relaxed);
	for (size_t i = 0; i < used; i++)
	{
		if (g_ranges[i].load(std::memory_order_relaxed) == 0)
		{
			g_ranges[i].store(packed, std::memory_order_release);
			return;
		}
	}
	EXIT_IF(used == kCapacity);
	g_ranges[used].store(packed, std::memory_order_relaxed);
	g_used.store(used + 1, std::memory_order_release);
}

void Remove(uint64_t begin, uint64_t end) noexcept
{
	const uint64_t  packed = Pack(begin, end);
	std::lock_guard lock(g_writer_mutex);
	const size_t    used = g_used.load(std::memory_order_relaxed);
	for (size_t i = 0; i < used; i++)
	{
		if (g_ranges[i].load(std::memory_order_relaxed) == packed)
		{
			g_ranges[i].store(0, std::memory_order_release);
			return;
		}
	}
	EXIT("guest code range was not registered");
}

bool Contains(uint64_t address) noexcept
{
	const uint64_t page = address >> kPageShift;
	const size_t   used = g_used.load(std::memory_order_acquire);
	for (size_t i = 0; i < used; i++)
	{
		const uint64_t packed = g_ranges[i].load(std::memory_order_acquire);
		if (page >= (packed >> 32u) && page < (packed & UINT32_MAX))
		{
			return true;
		}
	}
	return false;
}

} // namespace Kyty::Emulator::GuestCodeRanges

#endif // KYTY_EMU_ENABLED
