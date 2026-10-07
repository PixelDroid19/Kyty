#pragma once

#include <array>
#include <cstdint>

namespace Kyty::Libs::Graphics::FragmentTransport {

inline constexpr uint32_t QUAD_LANES          = 4;
inline constexpr uint32_t WAVE_QUADS          = 16;
inline constexpr uint32_t WAVE_LANES          = QUAD_LANES * WAVE_QUADS;
inline constexpr uint32_t CONTROL_WORDS       = 16;
inline constexpr uint32_t PRIMITIVE_WORDS     = 2;
inline constexpr uint32_t QUAD_HEADER_WORDS   = 4;
inline constexpr uint32_t QUAD_LOOKUP_WORDS   = 4;
inline constexpr uint32_t OUTPUT_LANE_WORDS   = 34;
inline constexpr uint32_t MAX_LOOKUP_PROBES   = 128;
inline constexpr uint64_t MAX_TRANSIENT_BYTES = 128ull * 1024 * 1024;

enum class Buffer : uint32_t
{
	Records,
	Control,
	References,
	WaveInput,
	WaveOutput,
	Count
};

struct Request
{
	uint32_t quad_capacity     = 0;
	uint32_t primitive_count   = 0;
	uint32_t lane_words        = 0;
	uint32_t wave_header_words = 0;
};

struct Limits
{
	uint64_t storage_buffer_bytes = 0;
	uint64_t total_bytes          = 0;
	uint32_t dispatch_groups_x    = 0;
};

struct Layout
{
	Request                                                    request;
	uint32_t                                                   quad_words                     = 0;
	uint32_t                                                   quad_lookup_capacity           = 0;
	uint32_t                                                   quad_lookup_offset             = 0;
	uint32_t                                                   wave_capacity                  = 0;
	uint32_t                                                   wave_lookup_capacity           = 0;
	uint32_t                                                   wave_lookup_offset             = 0;
	uint32_t                                                   active_wave_reference_offset   = 0;
	uint32_t                                                   record_output_reference_offset = 0;
	uint32_t                                                   input_words_per_wave           = 0;
	uint32_t                                                   output_words_per_wave          = WAVE_LANES * OUTPUT_LANE_WORDS;
	uint32_t                                                   pack_groups_x                  = 0;
	std::array<uint64_t, static_cast<uint32_t>(Buffer::Count)> buffer_bytes {};
	uint64_t                                                   total_bytes = 0;

	[[nodiscard]] uint64_t Bytes(Buffer buffer) const noexcept;
};

// Shared push-constant layout for the host scan and pack kernels. Guest
// descriptor metadata uses its own pipeline layout and is never overwritten.
struct KernelParameters
{
	uint32_t quad_capacity;
	uint32_t primitive_count;
	uint32_t lane_words;
	uint32_t wave_header_words;
	uint32_t quad_lookup_capacity;
	uint32_t wave_capacity;
	uint32_t wave_lookup_capacity;
};

static_assert(sizeof(KernelParameters) == 28u);

[[nodiscard]] KernelParameters GetKernelParameters(const Layout& layout) noexcept;

enum class Failure
{
	None,
	InvalidArgument,
	StorageBufferLimit,
	TotalBudget,
	DispatchLimit
};

struct Check
{
	Failure  failure   = Failure::None;
	uint64_t required  = 0;
	uint64_t available = 0;
};

// These are host transport records, not guest descriptors. One counter per
// primitive keeps all sixteen quads in a wave on the same parameter cache.
[[nodiscard]] Check BuildLayout(const Request& request, const Limits& limits, Layout* output) noexcept;

} // namespace Kyty::Libs::Graphics::FragmentTransport
