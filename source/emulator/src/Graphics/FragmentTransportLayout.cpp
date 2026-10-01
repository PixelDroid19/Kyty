#include "Emulator/Graphics/FragmentTransportLayout.h"

#include <algorithm>

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

bool Valid(const Request& request, const Limits& limits) noexcept
{
	// The wave lookup key packs a primitive and its quad-page ordinal into
	// sixteen bits each, then adds one to reserve zero for an empty slot.
	return request.quad_capacity > 0 && request.quad_capacity <= 0x100000u && request.primitive_count > 0 &&
	       request.primitive_count <= 0xffffu && request.lane_words > 0 && request.lane_words <= 1u + 256u + 32u * 16u &&
	       request.wave_header_words <= 1u && limits.storage_buffer_bytes > 0 && limits.total_bytes > 0 && limits.dispatch_groups_x > 0;
}

uint32_t LookupCapacity(uint32_t entries) noexcept
{
	uint32_t capacity = 1;
	while (capacity < entries * 2u)
	{
		capacity <<= 1u;
	}
	return capacity;
}

void SetBytes(Layout* layout, Buffer buffer, uint64_t words) noexcept
{
	const uint64_t bytes                                = words * sizeof(uint32_t);
	layout->buffer_bytes[static_cast<uint32_t>(buffer)] = bytes;
	layout->total_bytes += bytes;
}

Layout MakeLayout(const Request& request) noexcept
{
	Layout layout {};
	layout.request              = request;
	layout.quad_words           = QUAD_HEADER_WORDS + QUAD_LANES * request.lane_words;
	layout.quad_lookup_capacity = LookupCapacity(request.quad_capacity);
	layout.quad_lookup_offset   = CONTROL_WORDS + PRIMITIVE_WORDS * request.primitive_count;
	// Each nonempty primitive can start one page with a single quad. Every
	// additional page needs sixteen more quads, even if other pages are partial.
	const uint32_t nonempty_primitives    = std::min(request.quad_capacity, request.primitive_count);
	layout.wave_capacity                  = nonempty_primitives + (request.quad_capacity - nonempty_primitives) / WAVE_QUADS;
	layout.wave_lookup_capacity           = LookupCapacity(layout.wave_capacity);
	layout.wave_lookup_offset             = layout.quad_lookup_offset + QUAD_LOOKUP_WORDS * layout.quad_lookup_capacity;
	layout.active_wave_reference_offset   = layout.wave_lookup_capacity * WAVE_QUADS;
	layout.record_output_reference_offset = layout.active_wave_reference_offset + layout.wave_capacity;
	layout.input_words_per_wave           = request.wave_header_words + WAVE_LANES * request.lane_words;
	layout.pack_groups_x                  = (layout.wave_lookup_capacity + 63u) / 64u;
	SetBytes(&layout, Buffer::Records, static_cast<uint64_t>(request.quad_capacity) * layout.quad_words);
	SetBytes(&layout, Buffer::Control, static_cast<uint64_t>(layout.wave_lookup_offset) + layout.wave_lookup_capacity);
	SetBytes(&layout, Buffer::References, static_cast<uint64_t>(layout.record_output_reference_offset) + request.quad_capacity);
	SetBytes(&layout, Buffer::WaveInput, static_cast<uint64_t>(layout.wave_capacity) * layout.input_words_per_wave);
	SetBytes(&layout, Buffer::WaveOutput, static_cast<uint64_t>(layout.wave_capacity) * layout.output_words_per_wave);
	return layout;
}

Check CheckLimits(const Layout& layout, const Limits& limits) noexcept
{
	for (uint64_t bytes: layout.buffer_bytes)
	{
		if (bytes > limits.storage_buffer_bytes)
		{
			return {Failure::StorageBufferLimit, bytes, limits.storage_buffer_bytes};
		}
	}
	const uint64_t budget = std::min(limits.total_bytes, MAX_TRANSIENT_BYTES);
	if (layout.total_bytes > budget)
	{
		return {Failure::TotalBudget, layout.total_bytes, budget};
	}
	const uint32_t groups = std::max(layout.wave_capacity, layout.pack_groups_x);
	if (groups > limits.dispatch_groups_x)
	{
		return {Failure::DispatchLimit, groups, limits.dispatch_groups_x};
	}
	return {};
}

} // namespace

uint64_t Layout::Bytes(Buffer buffer) const noexcept
{
	const auto index = static_cast<uint32_t>(buffer);
	return index < buffer_bytes.size() ? buffer_bytes[index] : 0u;
}

Check BuildLayout(const Request& request, const Limits& limits, Layout* output) noexcept
{
	if (output == nullptr || !Valid(request, limits))
	{
		return {Failure::InvalidArgument, 0, 0};
	}
	const auto layout = MakeLayout(request);
	const auto check  = CheckLimits(layout, limits);
	if (check.failure != Failure::None)
	{
		return check;
	}
	*output = layout;
	return {};
}

KernelParameters GetKernelParameters(const Layout& layout) noexcept
{
	return {layout.request.quad_capacity, layout.request.primitive_count, layout.request.lane_words,  layout.request.wave_header_words,
	        layout.quad_lookup_capacity,  layout.wave_capacity,           layout.wave_lookup_capacity};
}

} // namespace Kyty::Libs::Graphics::FragmentTransport
