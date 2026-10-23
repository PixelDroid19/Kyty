#include "Emulator/GuestCodeRanges.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::GuestCodeRanges {
namespace {

constexpr uint64_t kPageShift = 12;
constexpr uint64_t kPageMask  = (uint64_t {1} << kPageShift) - 1;
// Guest code lives in the 48-bit address space; no range may extend past it.
constexpr uint64_t kAddressLimit = uint64_t {1} << 48;

// Each slot packs the first page of one range and its page count into a single
// word, so a lock-free reader always sees both bounds of the same range. The
// 36-bit page number covers the whole 48-bit address space and the 28-bit count
// allows 1 TiB per range. A range is never empty, so zero marks a free slot.
constexpr uint32_t kCountBits = 28;
constexpr uint64_t kCountMask = (uint64_t {1} << kCountBits) - 1;
// Touching entries are merged, so a slot holds one contiguous run of pages.
// Signal handlers scan slots up to the used count; 1024 slots (8 KiB) leave room
// for the runs the kernel maps as executable.
constexpr size_t kCapacity = 1024;

std::atomic<uint64_t> g_ranges[kCapacity] {};
std::atomic<size_t>   g_used {0};
std::mutex            g_writer_mutex;

// Pages [first, end).
struct PageRange
{
	uint64_t first = 0;
	uint64_t end   = 0;
};

bool IsEmpty(PageRange pages)
{
	return pages.first >= pages.end;
}

bool Overlaps(PageRange a, PageRange b)
{
	return !IsEmpty(a) && !IsEmpty(b) && a.first < b.end && b.first < a.end;
}

bool Touches(PageRange a, PageRange b)
{
	return !IsEmpty(a) && !IsEmpty(b) && a.first <= b.end && b.first <= a.end;
}

bool Covers(PageRange outer, PageRange inner)
{
	return outer.first <= inner.first && inner.end <= outer.end;
}

PageRange Hull(PageRange a, PageRange b)
{
	return {std::min(a.first, b.first), std::max(a.end, b.end)};
}

// Whole pages touched by [begin, end). The host protects and frees whole pages,
// so a partly covered page counts as covered in both Add and Remove: a stale
// entry could name an address the host reuses, while a missing one only defers
// a safe point.
PageRange TouchedPages(uint64_t begin, uint64_t end)
{
	EXIT_IF(begin >= end || end > kAddressLimit);
	return {begin >> kPageShift, (end + kPageMask) >> kPageShift};
}

uint64_t Pack(PageRange pages)
{
	EXIT_IF(IsEmpty(pages) || pages.end - pages.first > kCountMask);
	return (pages.first << kCountBits) | (pages.end - pages.first);
}

PageRange Unpack(uint64_t packed)
{
	const uint64_t first = packed >> kCountBits;
	return {first, first + (packed & kCountMask)};
}

PageRange EntryAt(size_t index)
{
	return Unpack(g_ranges[index].load(std::memory_order_relaxed));
}

// A free slot below the used count, or the slot just above it.
size_t FindFreeSlot(size_t used)
{
	for (size_t i = 0; i < used; i++)
	{
		if (g_ranges[i].load(std::memory_order_relaxed) == 0)
		{
			return i;
		}
	}
	EXIT_IF(used == kCapacity);
	return used;
}

// The first slot whose entry touches pages, where their union is stored.
size_t FindSlotFor(PageRange pages, size_t used)
{
	for (size_t i = 0; i < used; i++)
	{
		if (Touches(EntryAt(i), pages))
		{
			return i;
		}
	}
	return FindFreeSlot(used);
}

// A slot above the used count raises the count after the store, so a reader that
// sees the new count also sees the slot.
void SetSlot(size_t index, uint64_t packed)
{
	g_ranges[index].store(packed, std::memory_order_release);
	if (index >= g_used.load(std::memory_order_relaxed))
	{
		g_used.store(index + 1, std::memory_order_release);
	}
}

// Lowers the used count past free slots at its top, so scans stop at the last entry.
void TrimUnusedSlots()
{
	size_t used = g_used.load(std::memory_order_relaxed);
	while (used != 0 && g_ranges[used - 1].load(std::memory_order_relaxed) == 0)
	{
		used--;
	}
	g_used.store(used, std::memory_order_release);
}

// Widens pages until no entry touches it, so one entry can hold the whole union.
PageRange MergeTouchingEntries(PageRange pages, size_t used)
{
	bool grew = true;
	while (grew)
	{
		grew = false;
		for (size_t i = 0; i < used; i++)
		{
			const PageRange entry = EntryAt(i);
			if (Touches(entry, pages) && !Covers(pages, entry))
			{
				pages = Hull(pages, entry);
				grew  = true;
			}
		}
	}
	return pages;
}

// Removes pages from the entry in one slot. A removal inside the entry keeps both
// sides: the right-hand piece is published before the entry shrinks to the
// left-hand piece.
void RemoveFromSlot(size_t index, PageRange entry, PageRange pages)
{
	const bool keep_left  = entry.first < pages.first;
	const bool keep_right = pages.end < entry.end;
	if (keep_left && keep_right)
	{
		SetSlot(FindFreeSlot(g_used.load(std::memory_order_relaxed)), Pack({pages.end, entry.end}));
		SetSlot(index, Pack({entry.first, pages.first}));
	} else if (keep_left)
	{
		SetSlot(index, Pack({entry.first, pages.first}));
	} else if (keep_right)
	{
		SetSlot(index, Pack({pages.end, entry.end}));
	} else
	{
		SetSlot(index, 0);
	}
}

} // namespace

// The merged entry is published before the entries it absorbs are cleared, so
// their pages never leave the table.
void Add(uint64_t begin, uint64_t end) noexcept
{
	const PageRange pages = TouchedPages(begin, end);
	std::lock_guard lock(g_writer_mutex);
	const size_t    used   = g_used.load(std::memory_order_relaxed);
	const PageRange merged = MergeTouchingEntries(pages, used);
	const size_t    target = FindSlotFor(merged, used);
	SetSlot(target, Pack(merged));
	for (size_t i = 0; i < used; i++)
	{
		if (i != target && Touches(EntryAt(i), merged))
		{
			SetSlot(i, 0);
		}
	}
	TrimUnusedSlots();
}

void Remove(uint64_t begin, uint64_t end) noexcept
{
	const PageRange pages = TouchedPages(begin, end);
	std::lock_guard lock(g_writer_mutex);
	const size_t    used = g_used.load(std::memory_order_relaxed);
	for (size_t i = 0; i < used; i++)
	{
		const PageRange entry = EntryAt(i);
		if (Overlaps(entry, pages))
		{
			RemoveFromSlot(i, entry, pages);
		}
	}
	TrimUnusedSlots();
}

bool Contains(uint64_t address) noexcept
{
	const uint64_t page = address >> kPageShift;
	const size_t   used = g_used.load(std::memory_order_acquire);
	for (size_t i = 0; i < used; i++)
	{
		const PageRange pages = Unpack(g_ranges[i].load(std::memory_order_acquire));
		if (page >= pages.first && page < pages.end)
		{
			return true;
		}
	}
	return false;
}

} // namespace Kyty::Emulator::GuestCodeRanges

#endif // KYTY_EMU_ENABLED
