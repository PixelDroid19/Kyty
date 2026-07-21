#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

	void Store(const GpuMemoryRangeQueryKey& key, const Value& value)
	{
		if (!key.Valid())
		{
			return;
		}
		m_entries[static_cast<size_t>(key.Hash()) & (Capacity - 1u)] = Entry {m_epoch, key, value};
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
		for (auto& entry: m_entries)
		{
			if (entry.has_value() && entry->epoch == m_epoch && entry->key.Overlaps(address, size))
			{
				entry.reset();
			}
		}
	}

private:
	std::array<std::optional<Entry>, Capacity> m_entries {};
	uint64_t                                   m_epoch = 1u;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYRANGEQUERYCACHE_H_ */
