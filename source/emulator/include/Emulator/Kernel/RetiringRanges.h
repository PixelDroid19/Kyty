#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_RETIRINGRANGES_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_RETIRINGRANGES_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/GpuMappingLifecycle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Memory {

// Physical ranges that were released while the GPU may still read them. They are neither allocated nor free: the
// allocator must skip them until the GPU work that was in flight at release time has finished (their ticket
// retires). Tickets are issued in increasing order and retire in that order.
class RetiringRanges
{
public:
	struct Entry
	{
		uint64_t start            = 0;
		uint64_t size             = 0;
		uint64_t ticket           = 0;
		bool     discard_backing  = false; // the release left no mapping, so its bytes are dropped on retirement
		// Guest views whose GPU state the release dropped; checked again when the ticket retires.
		std::vector<KernelGpuMappingRange> gpu_ranges;
	};

	// Bounds the memory the GPU can hold back; callers wait for retirement instead of growing past it.
	static constexpr size_t kCapacity = 64;

	[[nodiscard]] bool IsFull() const { return m_entries.size() >= kCapacity; }
	[[nodiscard]] bool IsEmpty() const { return m_entries.empty(); }
	[[nodiscard]] size_t Size() const { return m_entries.size(); }

	void Add(const Entry& entry)
	{
		const auto position = std::upper_bound(m_entries.begin(), m_entries.end(), entry.start,
		                                       [](uint64_t start, const Entry& other) { return start < other.start; });
		m_entries.insert(position, entry);
	}

	// End of the first entry overlapping [start, start + size).
	[[nodiscard]] bool FindOverlap(uint64_t start, uint64_t size, uint64_t* end) const
	{
		for (const auto& entry: m_entries)
		{
			if (entry.start < start + size && start < entry.start + entry.size)
			{
				*end = entry.start + entry.size;
				return true;
			}
		}
		return false;
	}

	// Oldest ticket among the entries overlapping [begin, end).
	[[nodiscard]] bool OldestTicketIn(uint64_t begin, uint64_t end, uint64_t* ticket) const
	{
		bool found = false;
		for (const auto& entry: m_entries)
		{
			if (entry.start < end && begin < entry.start + entry.size && (!found || entry.ticket < *ticket))
			{
				*ticket = entry.ticket;
				found   = true;
			}
		}
		return found;
	}

	[[nodiscard]] std::vector<uint64_t> Tickets() const
	{
		std::vector<uint64_t> tickets;
		for (const auto& entry: m_entries)
		{
			tickets.push_back(entry.ticket);
		}
		return tickets;
	}

	// Copies of the entries whose ticket `is_retired` accepts; they stay listed (and occupied) until Remove.
	template <typename IsRetired> [[nodiscard]] std::vector<Entry> Collect(IsRetired&& is_retired) const
	{
		std::vector<Entry> retired;
		std::copy_if(m_entries.begin(), m_entries.end(), std::back_inserter(retired), [&](const Entry& entry) { return is_retired(entry.ticket); });
		return retired;
	}

	void Remove(uint64_t start)
	{
		m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [start](const Entry& entry) { return entry.start == start; }),
		                m_entries.end());
	}

private:
	std::vector<Entry> m_entries; // ordered by start
};

} // namespace Kyty::Kernel::Memory

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_RETIRINGRANGES_H_ */
