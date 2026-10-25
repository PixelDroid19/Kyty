#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Kyty::Libs::Graphics {

class GpuMemoryRangeQueryKey final
{
public:
	static constexpr int MaxRanges = 3;

	[[nodiscard]] static GpuMemoryRangeQueryKey Create(const uint64_t* address, const uint64_t* size, int range_count,
	                                                   bool only_first)
	{
		GpuMemoryRangeQueryKey key;
		if (address == nullptr || size == nullptr || range_count <= 0 || range_count > MaxRanges)
		{
			return key;
		}

		key.m_range_count = static_cast<uint8_t>(range_count);
		key.m_only_first  = only_first;
		key.m_valid       = true;
		for (int i = 0; i < range_count; ++i)
		{
			if (size[i] == 0u || address[i] > std::numeric_limits<uint64_t>::max() - size[i])
			{
				return {};
			}
			key.m_address[static_cast<size_t>(i)] = address[i];
			key.m_size[static_cast<size_t>(i)]    = size[i];
		}
		return key;
	}

	[[nodiscard]] bool Valid() const { return m_valid; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size) const
	{
		if (!m_valid || size == 0u)
		{
			return false;
		}
		for (uint8_t i = 0; i < m_range_count; ++i)
		{
			if (RangesOverlap(m_address[i], m_size[i], address, size))
			{
				return true;
			}
		}
		return false;
	}

	// Smallest [begin, end) covering every range; Create rejects ranges whose
	// end overflows.
	[[nodiscard]] std::pair<uint64_t, uint64_t> Span() const
	{
		if (!m_valid)
		{
			return {0u, 0u};
		}
		uint64_t begin = std::numeric_limits<uint64_t>::max();
		uint64_t end   = 0u;
		for (uint8_t i = 0; i < m_range_count; ++i)
		{
			begin = std::min(begin, m_address[i]);
			end   = std::max(end, m_address[i] + m_size[i]);
		}
		return {begin, end};
	}

	[[nodiscard]] uint64_t Hash() const
	{
		uint64_t hash = 0x6a09e667f3bcc909ull;
		HashCombine(&hash, m_range_count);
		HashCombine(&hash, m_only_first ? 1u : 0u);
		for (uint8_t i = 0; i < m_range_count; ++i)
		{
			HashCombine(&hash, m_address[i]);
			HashCombine(&hash, m_size[i]);
		}
		return hash;
	}

	[[nodiscard]] bool operator==(const GpuMemoryRangeQueryKey& other) const
	{
		return m_valid == other.m_valid && m_range_count == other.m_range_count && m_only_first == other.m_only_first &&
		       m_address == other.m_address && m_size == other.m_size;
	}

private:
	static bool RangesOverlap(uint64_t lhs_address, uint64_t lhs_size, uint64_t rhs_address, uint64_t rhs_size)
	{
		if (lhs_size == 0u || rhs_size == 0u)
		{
			return false;
		}
		return lhs_address <= rhs_address ? rhs_address - lhs_address < lhs_size : lhs_address - rhs_address < rhs_size;
	}

	static uint64_t Mix(uint64_t value)
	{
		value += 0x9e3779b97f4a7c15ull;
		value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
		value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
		return value ^ (value >> 31u);
	}

	static void HashCombine(uint64_t* hash, uint64_t value)
	{
		*hash ^= Mix(value) + 0x9e3779b97f4a7c15ull + (*hash << 6u) + (*hash >> 2u);
	}

	std::array<uint64_t, MaxRanges> m_address {};
	std::array<uint64_t, MaxRanges> m_size {};
	uint8_t                         m_range_count = 0;
	bool                            m_only_first  = false;
	bool                            m_valid       = false;
};

// Slots of a range cache by the span they cover, so a range invalidation
// visits only the slots whose span can overlap it. Spans are grouped by
// length: a class whose spans are at most `bound` bytes long only holds an
// overlapping span that starts within `bound` bytes below the range.
class GpuMemoryRangeSpanIndex final
{
	using Span = std::pair<uint64_t, uint64_t>;
	using Set  = std::set<std::pair<uint64_t, uint32_t>>;

	static constexpr std::array<uint64_t, 5> kClassBounds = {uint64_t {1} << 12u, uint64_t {1} << 16u, uint64_t {1} << 20u,
	                                                         uint64_t {1} << 24u, uint64_t {1} << 28u};

public:
	void Clear()
	{
		for (auto& spans: m_classes)
		{
			spans.clear();
		}
	}

	// `previous` is the slot's current span, {0, 0} when it has none.
	void Store(uint32_t slot, Span previous, Span span)
	{
		if (previous.second == 0u)
		{
			m_classes[ClassOf(span)].insert({span.first, slot});
			return;
		}
		auto node          = m_classes[ClassOf(previous)].extract({previous.first, slot});
		node.value().first = span.first;
		m_classes[ClassOf(span)].insert(std::move(node));
	}

	void Erase(uint32_t slot, Span span) { m_classes[ClassOf(span)].erase({span.first, slot}); }

	// Slots whose span may overlap [address, end); the caller checks each span.
	const std::vector<uint32_t>& Candidates(uint64_t address, uint64_t end)
	{
		m_candidates.clear();
		for (size_t c = 0; c < m_classes.size(); ++c)
		{
			const auto& spans = m_classes[c];
			auto        it    = spans.begin();
			if (c < kClassBounds.size() && address >= kClassBounds[c])
			{
				it = spans.lower_bound({address - kClassBounds[c] + 1u, 0u});
			}
			for (; it != spans.end() && it->first < end; ++it)
			{
				m_candidates.push_back(it->second);
			}
		}
		return m_candidates;
	}

private:
	static size_t ClassOf(Span span)
	{
		const uint64_t length = span.second - span.first;
		size_t         c      = 0;
		while (c < kClassBounds.size() && length > kClassBounds[c])
		{
			++c;
		}
		return c;
	}

	std::array<Set, kClassBounds.size() + 1u> m_classes;
	std::vector<uint32_t>                     m_candidates;
};

// Bounded direct-mapped memoization for exact range queries. Collisions only
// replace entries; full-key comparison makes them harmless to correctness.
//
// The owner must call Invalidate whenever indexed ranges are inserted,
// removed, or changed. The cache is intentionally not thread-safe because
// GpuMemory serializes its use with the object-graph mutex.
template <typename Value, size_t Capacity = 1024>
class GpuMemoryRangeQueryCache final
{
	static_assert(Capacity != 0u && (Capacity & (Capacity - 1u)) == 0u, "cache capacity must be a power of two");

	struct Entry
	{
		uint64_t               epoch = 0;
		GpuMemoryRangeQueryKey key;
		Value                  value;
	};

public:
	[[nodiscard]] bool Lookup(const GpuMemoryRangeQueryKey& key, Value* value) const
	{
		if (!key.Valid() || value == nullptr)
		{
			return false;
		}

		const auto& slot = m_entries[static_cast<size_t>(key.Hash()) & (Capacity - 1u)];
		if (!slot.has_value() || slot->epoch != m_epoch || !(slot->key == key))
		{
			return false;
		}
		*value = slot->value;
		return true;
	}

	// Borrowed-lookup: returns the cached value without copying it. The caller
	// must not retain the reference past the next Store/Invalidate on this
	// cache, which is guaranteed while GpuMemory serializes queries under its
	// object-graph mutex.
	[[nodiscard]] const Value* BorrowLookup(const GpuMemoryRangeQueryKey& key) const
	{
		if (!key.Valid())
		{
			return nullptr;
		}

		const auto& slot = m_entries[static_cast<size_t>(key.Hash()) & (Capacity - 1u)];
		if (!slot.has_value() || slot->epoch != m_epoch || !(slot->key == key))
		{
			return nullptr;
		}
		return &slot->value;
	}

	void Store(const GpuMemoryRangeQueryKey& key, const Value& value)
	{
		if (!key.Valid())
		{
			return;
		}
		const size_t slot = static_cast<size_t>(key.Hash()) & (Capacity - 1u);
		const auto   span = key.Span();
		m_index.Store(static_cast<uint32_t>(slot), m_spans[slot], span);
		m_entries[slot] = Entry {m_epoch, key, value};
		m_spans[slot]   = span;
	}

	void Invalidate()
	{
		++m_epoch;
		if (m_epoch == 0u)
		{
			for (auto& entry: m_entries)
			{
				entry.reset();
			}
			m_epoch = 1u;
		}
	}

	void InvalidateRange(uint64_t address, uint64_t size)
	{
		if (size == 0u)
		{
			return;
		}
		// Every object creation invalidates its range; read a full entry only
		// where its span overlaps.
		const uint64_t end = size > std::numeric_limits<uint64_t>::max() - address ? std::numeric_limits<uint64_t>::max() : address + size;
		for (const uint32_t slot: m_index.Candidates(address, end))
		{
			if (m_spans[slot].second <= address || m_spans[slot].first >= end)
			{
				continue;
			}
			auto& entry = m_entries[slot];
			if (!entry.has_value() || entry->epoch != m_epoch || entry->key.Overlaps(address, size))
			{
				entry.reset();
				m_index.Erase(slot, m_spans[slot]);
				m_spans[slot] = {0u, 0u};
			}
		}
	}

private:
	std::array<std::optional<Entry>, Capacity>          m_entries {};
	std::array<std::pair<uint64_t, uint64_t>, Capacity> m_spans {};
	GpuMemoryRangeSpanIndex                             m_index;
	uint64_t                                            m_epoch = 1u;
};

// The same memoization with a table that follows its owner's population. A
// title can own thousands of nearly empty GPU heaps; a fixed table per heap
// held gigabytes and made every range invalidation walk all of its slots.
template <typename Value, size_t MaxCapacity = 4096>
class GpuMemoryAdaptiveRangeQueryCache final
{
	static_assert(MaxCapacity != 0u && (MaxCapacity & (MaxCapacity - 1u)) == 0u, "cache capacity must be a power of two");

	struct Entry
	{
		uint64_t               epoch = 0;
		GpuMemoryRangeQueryKey key;
		Value                  value;
	};

public:
	static constexpr size_t MIN_CAPACITY = 16;

	// Grow to four slots per indexed object, a power of two between
	// MIN_CAPACITY and MaxCapacity. Growing drops every cached entry.
	void Reserve(size_t population)
	{
		size_t wanted = MIN_CAPACITY;
		while (wanted < MaxCapacity && wanted < population * 4u)
		{
			wanted <<= 1u;
		}
		if (wanted > m_entries.size())
		{
			m_entries.assign(wanted, std::nullopt);
			m_spans.assign(wanted, {0u, 0u});
			m_index.Clear();
		}
	}

	[[nodiscard]] size_t Capacity() const { return m_entries.size(); }

	[[nodiscard]] const Value* BorrowLookup(const GpuMemoryRangeQueryKey& key) const
	{
		if (!key.Valid() || m_entries.empty())
		{
			return nullptr;
		}
		const auto& slot = m_entries[static_cast<size_t>(key.Hash()) & (m_entries.size() - 1u)];
		if (!slot.has_value() || slot->epoch != m_epoch || !(slot->key == key))
		{
			return nullptr;
		}
		return &slot->value;
	}

	void Store(const GpuMemoryRangeQueryKey& key, const Value& value)
	{
		if (!key.Valid() || m_entries.empty())
		{
			return;
		}
		const size_t slot = static_cast<size_t>(key.Hash()) & (m_entries.size() - 1u);
		const auto   span = key.Span();
		m_index.Store(static_cast<uint32_t>(slot), m_spans[slot], span);
		m_entries[slot] = Entry {m_epoch, key, value};
		m_spans[slot]   = span;
	}

	void InvalidateRange(uint64_t address, uint64_t size)
	{
		if (size == 0u)
		{
			return;
		}
		const uint64_t end = size > std::numeric_limits<uint64_t>::max() - address ? std::numeric_limits<uint64_t>::max() : address + size;
		for (const uint32_t slot: m_index.Candidates(address, end))
		{
			if (m_spans[slot].second <= address || m_spans[slot].first >= end)
			{
				continue;
			}
			auto& entry = m_entries[slot];
			if (!entry.has_value() || entry->epoch != m_epoch || entry->key.Overlaps(address, size))
			{
				entry.reset();
				m_index.Erase(slot, m_spans[slot]);
				m_spans[slot] = {0u, 0u};
			}
		}
	}

private:
	std::vector<std::optional<Entry>>          m_entries;
	std::vector<std::pair<uint64_t, uint64_t>> m_spans;
	GpuMemoryRangeSpanIndex                    m_index;
	uint64_t                                   m_epoch = 1u;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_ */
