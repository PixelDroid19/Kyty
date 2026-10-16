#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYMATERIALIZATIONCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYMATERIALIZATIONCACHE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace Kyty::Libs::Graphics {

class GpuMemoryMaterializationKey final
{
public:
	static constexpr int MaxRanges = 3;
	static constexpr int MaxParams = 12;

	[[nodiscard]] static GpuMemoryMaterializationKey Create(uint64_t guest_submit, uint32_t host_queue, uint64_t host_sequence,
	                                                       const uint64_t* address, const uint64_t* size, int range_count,
	                                                       uint32_t object_type, const uint64_t* params, int param_count,
	                                                       bool check_hash, bool read_only)
	{
		GpuMemoryMaterializationKey key;
		if (guest_submit == 0u || host_sequence == 0u || object_type == 0u || address == nullptr || size == nullptr ||
		    range_count <= 0 || range_count > MaxRanges || param_count < 0 || param_count > MaxParams ||
		    (param_count > 0 && params == nullptr))
		{
			return key;
		}

		for (int i = 0; i < range_count; ++i)
		{
			if (size[i] == 0u || address[i] > std::numeric_limits<uint64_t>::max() - size[i])
			{
				return {};
			}
			key.m_address[static_cast<size_t>(i)] = address[i];
			key.m_size[static_cast<size_t>(i)]    = size[i];
		}
		for (int i = 0; i < param_count; ++i)
		{
			key.m_params[static_cast<size_t>(i)] = params[i];
		}

		key.m_guest_submit = guest_submit;
		key.m_host_sequence = host_sequence;
		key.m_host_queue    = host_queue;
		key.m_object_type   = object_type;
		key.m_range_count   = static_cast<uint8_t>(range_count);
		key.m_param_count   = static_cast<uint8_t>(param_count);
		key.m_check_hash    = check_hash;
		key.m_read_only     = read_only;
		key.m_valid         = true;
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

	// Smallest [begin, end) covering every range; empty for an invalid key.
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
		uint64_t hash = 0x510e527fade682d1ull;
		HashCombine(&hash, m_guest_submit);
		HashCombine(&hash, m_host_sequence);
		HashCombine(&hash, m_host_queue);
		HashCombine(&hash, m_object_type);
		HashCombine(&hash, m_range_count);
		HashCombine(&hash, m_param_count);
		HashCombine(&hash, m_check_hash ? 1u : 0u);
		HashCombine(&hash, m_read_only ? 1u : 0u);
		for (uint8_t i = 0; i < m_range_count; ++i)
		{
			HashCombine(&hash, m_address[i]);
			HashCombine(&hash, m_size[i]);
		}
		for (uint8_t i = 0; i < m_param_count; ++i)
		{
			HashCombine(&hash, m_params[i]);
		}
		return hash;
	}

	[[nodiscard]] bool operator==(const GpuMemoryMaterializationKey& other) const
	{
		return m_valid == other.m_valid && m_guest_submit == other.m_guest_submit && m_host_sequence == other.m_host_sequence &&
		       m_host_queue == other.m_host_queue && m_object_type == other.m_object_type && m_address == other.m_address &&
		       m_size == other.m_size && m_params == other.m_params && m_range_count == other.m_range_count &&
		       m_param_count == other.m_param_count && m_check_hash == other.m_check_hash && m_read_only == other.m_read_only;
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
	std::array<uint64_t, MaxParams> m_params {};
	uint64_t                        m_guest_submit = 0;
	uint64_t                        m_host_sequence = 0;
	uint32_t                        m_host_queue   = 0;
	uint32_t                        m_object_type  = 0;
	uint8_t                         m_range_count  = 0;
	uint8_t                         m_param_count  = 0;
	bool                            m_check_hash   = false;
	bool                            m_read_only    = false;
	bool                            m_valid        = false;
};

// Bounded exact memoization for repeated bindings while recording one guest
// and host submission. Full-key equality makes direct-map collisions harmless.
// The owner invalidates the cache whenever the logical object graph changes.
template <typename Value, size_t Capacity = 2048>
class GpuMemoryMaterializationCache final
{
	static_assert(Capacity != 0u && (Capacity & (Capacity - 1u)) == 0u, "cache capacity must be a power of two");

	struct Entry
	{
		uint64_t                    epoch = 0;
		GpuMemoryMaterializationKey key;
		Value                       value;
	};

public:
	[[nodiscard]] bool Lookup(const GpuMemoryMaterializationKey& key, Value* value) const
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

	void Store(const GpuMemoryMaterializationKey& key, const Value& value)
	{
		if (!key.Valid())
		{
			return;
		}
		const size_t slot = static_cast<size_t>(key.Hash()) & (Capacity - 1u);
		m_entries[slot]   = Entry {m_epoch, key, value};
		m_spans[slot]     = key.Span();
		m_store_log[static_cast<size_t>(m_store_seq % StoreLogSize)] = slot;
		++m_store_seq;
	}

	void Invalidate()
	{
		m_spans.fill({0u, 0u});
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
		// Recording invalidates the same attachment ranges every draw. After a
		// range has been invalidated no live entry overlaps it, so repeating
		// it only needs the slots stored since; other ranges scan every span.
		const uint64_t end   = size > std::numeric_limits<uint64_t>::max() - address ? std::numeric_limits<uint64_t>::max() : address + size;
		auto*          clean = FindCleanRange(address, end);
		if (clean != nullptr && m_store_seq - clean->store_seq <= StoreLogSize)
		{
			for (uint64_t seq = clean->store_seq; seq < m_store_seq; ++seq)
			{
				InvalidateSlot(m_store_log[static_cast<size_t>(seq % StoreLogSize)], address, size, end);
			}
			clean->store_seq = m_store_seq;
			return;
		}
		for (size_t slot = 0; slot < Capacity; ++slot)
		{
			InvalidateSlot(slot, address, size, end);
		}
		if (clean == nullptr)
		{
			clean = &m_clean_ranges[m_clean_next];
			m_clean_next = (m_clean_next + 1u) % m_clean_ranges.size();
		}
		*clean = CleanRange {address, end, m_store_seq};
	}

private:
	static constexpr uint64_t StoreLogSize = 256u;

	struct CleanRange
	{
		uint64_t address   = 0;
		uint64_t end       = 0;
		uint64_t store_seq = 0;
	};

	[[nodiscard]] CleanRange* FindCleanRange(uint64_t address, uint64_t end)
	{
		for (auto& range: m_clean_ranges)
		{
			if (range.end != 0u && range.address == address && range.end == end)
			{
				return &range;
			}
		}
		return nullptr;
	}

	void InvalidateSlot(size_t slot, uint64_t address, uint64_t size, uint64_t end)
	{
		if (m_spans[slot].second <= address || m_spans[slot].first >= end)
		{
			return;
		}
		auto& entry = m_entries[slot];
		if (entry.has_value() && entry->epoch == m_epoch && entry->key.Overlaps(address, size))
		{
			entry.reset();
			m_spans[slot] = {0u, 0u};
		}
	}

	std::array<std::optional<Entry>, Capacity> m_entries {};
	std::array<std::pair<uint64_t, uint64_t>, Capacity> m_spans {};
	std::array<size_t, StoreLogSize>                    m_store_log {};
	std::array<CleanRange, 4>                           m_clean_ranges {};
	uint64_t                                            m_store_seq  = 0;
	size_t                                              m_clean_next = 0;
	uint64_t                                            m_epoch      = 1u;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUMEMORYMATERIALIZATIONCACHE_H_ */
