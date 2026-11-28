#include "Emulator/Kernel/AmmMemory.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cinttypes>
#include <iterator>
#include <map>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Amm {

// Window layout: 256 GiB of AMM range followed by 32 GiB of multimap range,
// 64 GiB aligned. Taken from a secondary implementation and consistent with the
// sub-ranges the first title using AMM derives from the start; not measured on
// a console.
constexpr uint64_t kPageSize        = 0x4000;
constexpr uint64_t kRangeBytes      = UINT64_C(256) << 30u;
constexpr uint64_t kMultimapBytes   = UINT64_C(32) << 30u;
constexpr uint64_t kWindowBytes     = kRangeBytes + kMultimapBytes;
constexpr uint64_t kWindowAlignment = UINT64_C(64) << 30u;
constexpr int      kMapFixed        = 0x10;

// Disjoint half-open ranges, merged on insert.
class RangeSet
{
public:
	void Insert(uint64_t begin, uint64_t end)
	{
		auto it = m_ranges.upper_bound(begin);
		if (it != m_ranges.begin() && std::prev(it)->second >= begin)
		{
			--it;
			begin = it->first;
			end   = std::max(end, it->second);
			it    = m_ranges.erase(it);
		}
		while (it != m_ranges.end() && it->first <= end)
		{
			end = std::max(end, it->second);
			it  = m_ranges.erase(it);
		}
		m_ranges.emplace(begin, end);
	}

	void Erase(uint64_t begin, uint64_t end)
	{
		auto it = m_ranges.upper_bound(begin);
		if (it != m_ranges.begin())
		{
			--it;
		}
		while (it != m_ranges.end() && it->first < end)
		{
			const uint64_t range_begin = it->first;
			const uint64_t range_end   = it->second;
			if (range_end <= begin)
			{
				++it;
				continue;
			}
			it = m_ranges.erase(it);
			if (range_begin < begin)
			{
				m_ranges.emplace(range_begin, begin);
			}
			if (range_end > end)
			{
				m_ranges.emplace(end, range_end);
				break;
			}
		}
	}

	// Merged ranges: a covered span lies inside one range.
	[[nodiscard]] bool Covers(uint64_t begin, uint64_t end) const
	{
		auto it = m_ranges.upper_bound(begin);
		return it != m_ranges.begin() && std::prev(it)->second >= end;
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

	[[nodiscard]] std::pair<uint64_t, uint64_t> First() const { return *m_ranges.begin(); }

private:
	std::map<uint64_t, uint64_t> m_ranges;
};

struct State
{
	std::mutex                   mutex;
	uint64_t                     base = 0;
	RangeSet                     pool;   // free physical offsets
	RangeSet                     prt;    // PRT virtual ranges
	std::map<uint64_t, uint64_t> backed; // backed page address -> physical offset
};

static State& GetState()
{
	static auto* state = new State;
	return *state;
}

static int EnsureWindowLocked(State* state)
{
	if (state->base != 0)
	{
		return OK;
	}
	void*     address = nullptr;
	const int result  = Memory::KernelReserveVirtualRange(&address, kWindowBytes, 0, kWindowAlignment);
	if (result != OK)
	{
		return result;
	}
	state->base = reinterpret_cast<uint64_t>(address);
	KYTY_LOG_DEBUG("AMM window 0x%016" PRIx64 "+0x%" PRIx64 "\n", state->base, kWindowBytes);
	return OK;
}

static bool InWindow(const State& state, uint64_t address, uint64_t size)
{
	return size != 0 && ((address | size) & (kPageSize - 1)) == 0 && address >= state.base && size <= kWindowBytes &&
	       address - state.base <= kWindowBytes - size;
}

// Opens the window and validates a command range inside it.
static int CheckRangeLocked(State* state, uint64_t address, uint64_t size)
{
	const int result = EnsureWindowLocked(state);
	if (result != OK)
	{
		return result;
	}
	return InWindow(*state, address, size) ? OK : KERNEL_ERROR_EINVAL;
}

// Returns the backed pages of a range to the pool and turns their mappings
// back into reservation.
static int ReleaseBackedLocked(State* state, uint64_t address, uint64_t size)
{
	auto it = state->backed.lower_bound(address);
	while (it != state->backed.end() && it->first < address + size)
	{
		const uint64_t run_begin = it->first;
		uint64_t       run_end   = run_begin;
		while (it != state->backed.end() && it->first == run_end && run_end < address + size)
		{
			state->pool.Insert(it->second, it->second + kPageSize);
			run_end += kPageSize;
			it = state->backed.erase(it);
		}
		// A GPU-visible mapping cannot turn back into reservation in place, so
		// the run is unmapped and then reserved again.
		int result = Memory::KernelMunmap(run_begin, run_end - run_begin);
		if (result != OK)
		{
			return result;
		}
		void* run = reinterpret_cast<void*>(run_begin);
		result    = Memory::KernelReserveVirtualRange(&run, run_end - run_begin, kMapFixed, 0);
		if (result != OK)
		{
			return result;
		}
	}
	return OK;
}

// Maps pool pages over an unbacked run, one physically contiguous piece at a time.
static int BackRunLocked(State* state, uint64_t address, uint64_t size, int mtype, int protection)
{
	while (size != 0)
	{
		if (state->pool.Empty())
		{
			return KERNEL_ERROR_ENOMEM;
		}
		const auto [physical, physical_end] = state->pool.First();
		const uint64_t piece                = std::min(size, physical_end - physical);
		void*          target               = reinterpret_cast<void*>(address);
		const int      result               = Memory::KernelMapDirectMemory2(&target, piece, mtype, protection, kMapFixed,
		                                                                     static_cast<int64_t>(physical), 0);
		if (result != OK)
		{
			return result;
		}
		state->pool.Erase(physical, physical + piece);
		for (uint64_t offset = 0; offset < piece; offset += kPageSize)
		{
			state->backed.emplace(address + offset, physical + offset);
		}
		address += piece;
		size -= piece;
	}
	return OK;
}

int GetVirtualRanges(VirtualRanges* out)
{
	auto&                  state = GetState();
	const std::lock_guard  lock(state.mutex);
	const int              result = EnsureWindowLocked(&state);
	if (result != OK)
	{
		return result;
	}
	out->start          = state.base;
	out->end            = state.base + kRangeBytes;
	out->multimap_start = state.base + kRangeBytes;
	out->multimap_end   = state.base + kWindowBytes;
	return OK;
}

int GiveDirectMemory(int64_t search_start, int64_t search_end, uint64_t size, uint64_t alignment, int usage, int64_t* offset)
{
	if (offset == nullptr || (usage != 0 && usage != 1))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int result = Memory::KernelAllocateDirectMemory(search_start, search_end, size, alignment, 0, offset);
	if (result != OK || usage == 0)
	{
		return result;
	}
	auto&                 state = GetState();
	const std::lock_guard lock(state.mutex);
	const auto            start = static_cast<uint64_t>(*offset);
	state.pool.Insert(start, start + size);
	return OK;
}

int MapAsPrt(uint64_t address, uint64_t size)
{
	auto&                 state = GetState();
	const std::lock_guard lock(state.mutex);
	int                   result = CheckRangeLocked(&state, address, size);
	if (result == OK)
	{
		result = ReleaseBackedLocked(&state, address, size);
	}
	if (result == OK)
	{
		state.prt.Insert(address, address + size);
	}
	return result;
}

int AllocatePaForPrt(uint64_t address, uint64_t size, int mtype, int protection)
{
	auto&                 state = GetState();
	const std::lock_guard lock(state.mutex);
	const int             checked = CheckRangeLocked(&state, address, size);
	if (checked != OK)
	{
		return checked;
	}
	// The kernel mapping calls validate the type and protection.
	if (!state.prt.Covers(address, address + size))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const uint64_t end = address + size;
	for (uint64_t at = address; at < end;)
	{
		const bool backed = state.backed.count(at) != 0;
		uint64_t   run    = at + kPageSize;
		while (run < end && (state.backed.count(run) != 0) == backed)
		{
			run += kPageSize;
		}
		const int result = backed ? Memory::KernelMtypeprotect(reinterpret_cast<void*>(at), run - at, mtype, protection)
		                          : BackRunLocked(&state, at, run - at, mtype, protection);
		if (result != OK)
		{
			return result;
		}
		at = run;
	}
	return OK;
}

int UnmapToPrt(uint64_t address, uint64_t size)
{
	auto&                 state = GetState();
	const std::lock_guard lock(state.mutex);
	const int             checked = CheckRangeLocked(&state, address, size);
	if (checked != OK)
	{
		return checked;
	}
	if (!state.prt.Covers(address, address + size))
	{
		return KERNEL_ERROR_EINVAL;
	}
	return ReleaseBackedLocked(&state, address, size);
}

} // namespace Kyty::Kernel::Amm

#endif // KYTY_EMU_ENABLED
