#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTECOLORFILL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTECOLORFILL_H_

#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"

#include <array>
#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ComputeColorFillIdentity
{
	uint64_t address = 0;
	uint64_t size = 0;
	uint64_t logical_generation = 0;
	uint64_t backing_generation = 0;
	uint64_t content_sequence = 0;
};

struct ComputeColorFillEvent
{
	ComputeColorFillIdentity identity;
	SubmissionId producer;
	std::array<uint32_t, 4> words {};
	uint64_t sequence = 0;
	GpuDirtyReadObservation cpu_observation;
};

// A pending fill is a write event, not persistent image state. The renderer
// owns synchronization and validates both the source binding and image format.
// A first overlapping consumer removes the event even when its identity or
// queue cannot be proven. This prevents stale replay after an unsupported use.
class ComputeColorFillEvents
{
public:
	static constexpr uint32_t CAPACITY = 64;

	[[nodiscard]] bool Empty() const { return m_count == 0; }
	void DiscardAll()
	{
		m_events = {};
		m_count = 0;
	}
	void DiscardOverlaps(uint64_t address, uint64_t size)
	{
		for (auto& event: m_events)
		{
			if (event.sequence != 0 && Overlaps(event.identity, address, size))
			{
				event = {};
				--m_count;
			}
		}
	}
	[[nodiscard]] bool Publish(const ComputeColorFillIdentity& identity, SubmissionId producer,
	                           const std::array<uint32_t, 4>& words, const GpuDirtyReadObservation& cpu_observation = {})
	{
		DiscardOverlaps(identity.address, identity.size);
		if (!Valid(identity) || producer.sequence == 0 || m_next_sequence == UINT64_MAX)
		{
			return false;
		}
		auto* slot = &m_events[0];
		for (auto& event: m_events)
		{
			if (event.sequence == 0)
			{
				slot = &event;
				break;
			}
			if (event.sequence < slot->sequence) { slot = &event; }
		}
		if (slot->sequence == 0) { ++m_count; }
		*slot = {identity, producer, words, m_next_sequence++, cpu_observation};
		return true;
	}
	[[nodiscard]] bool Consume(const ComputeColorFillIdentity& identity, SubmissionId consumer, ComputeColorFillEvent* out)
	{
		if (out != nullptr) { *out = {}; }
		bool matched = false;
		for (auto& event: m_events)
		{
			if (event.sequence == 0 || !Overlaps(event.identity, identity.address, identity.size)) { continue; }
			const auto& source = event.identity;
			if (out != nullptr && Valid(identity) && source.address == identity.address && source.size == identity.size &&
			    source.logical_generation == identity.logical_generation && source.backing_generation == identity.backing_generation &&
			    source.content_sequence == identity.content_sequence && consumer.queue == event.producer.queue &&
			    consumer.sequence >= event.producer.sequence)
			{
				*out = event;
				matched = true;
			}
			event = {};
			--m_count;
		}
		return matched;
	}

private:
	[[nodiscard]] static bool Valid(const ComputeColorFillIdentity& identity)
	{
		return identity.address != 0 && identity.size != 0 && identity.address <= UINT64_MAX - identity.size &&
		       identity.logical_generation != 0 && identity.backing_generation != 0 && identity.content_sequence != 0;
	}
	[[nodiscard]] static bool Overlaps(const ComputeColorFillIdentity& identity, uint64_t address, uint64_t size)
	{
		return size != 0 && address <= UINT64_MAX - size && identity.address < address + size &&
		       address < identity.address + identity.size;
	}
	std::array<ComputeColorFillEvent, CAPACITY> m_events {};
	uint64_t m_next_sequence = 1;
	uint32_t m_count = 0;
};

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTECOLORFILL_H_
