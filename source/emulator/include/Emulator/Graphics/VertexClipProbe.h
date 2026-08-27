#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VERTEXCLIPPROBE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VERTEXCLIPPROBE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kVertexClipProbeInvalidDescriptorSet = std::numeric_limits<uint32_t>::max();

struct ShaderVertexClipProbeConfig
{
	bool     enabled             = false;
	bool     draw_scoped         = false;
	uint64_t min_present         = 0;
	uint64_t diagnostic_identity = 0;
};

enum class ShaderPixelProbeKind : uint32_t
{
	None,
	Input0,
	SampleResult,
	FinalMrtResult,
};

struct ShaderPixelInput0ProbeConfig
{
	bool     enabled             = false;
	bool     draw_scoped         = false;
	ShaderPixelProbeKind kind    = ShaderPixelProbeKind::None;
	uint32_t sample_ordinal      = 0;
	uint32_t export_ordinal      = 0;
	uint32_t mrt_target          = 0;
	uint32_t match_ordinal       = 0;
	bool     sparse_subgroup     = false;
	// Host-only, one-shot attachment observation for FinalMrtResult. This does
	// not change shader generation or the diagnostic module identity.
	bool     attachment_readback        = false;
	uint32_t attachment_min_invocations = 1;
	uint64_t min_present         = 0;
	uint64_t diagnostic_identity = 0;
};

// The host readback observes packed attachment bytes only. It deliberately
// does not decode them into guest or linear color values.
enum class VertexClipProbeAttachmentFormat : uint32_t
{
	Unsupported,
	B10G11R11Ufloat,
	Rgba16Sfloat,
	Rgba8,
	Bgra8,
};

enum class VertexClipProbeAttachmentStatus : uint32_t
{
	Ok,
	TargetUnavailable,
	Multisampled,
	TransferSourceUnavailable,
	ZeroExtent,
	UnsupportedFormat,
	TooLarge,
	BufferUnavailable,
	LoadDiscarded,
	UndefinedLayout,
	MapFailed,
	InvalidData,
};

// Exact draw occurrence timing shifts between strict processes. Keep the
// attachment probe bounded, but allow enough fenced empty matches to reach the
// next contributing occurrence without guessing a cross-process ordinal.
constexpr uint32_t kVertexClipProbeAttachmentMaxEmptyRetries = 8u;

struct VertexClipProbeAttachmentReadbackStats
{
	VertexClipProbeAttachmentFormat format                              = VertexClipProbeAttachmentFormat::Unsupported;
	uint32_t                        width                               = 0;
	uint32_t                        height                              = 0;
	uint64_t                        bytes                               = 0;
	uint64_t                        rgb_nonzero_pixels                  = 0;
	uint64_t                        rgb_nonzero_coverage_pixels         = 0;
	uint64_t                        fnv1a64                             = 0;
	bool                            coverage_bounds_available           = false;
};

struct VertexClipProbeAttachmentDeltaStats
{
	uint64_t rgb_changed_pixels                  = 0;
	uint64_t rgb_changed_coverage_pixels         = 0;
	uint64_t rgb_zero_to_nonzero_coverage_pixels = 0;
	uint64_t rgb_nonzero_to_zero_coverage_pixels = 0;
	bool     coverage_bounds_available           = false;
};

struct VertexClipProbeRawStats
{
	uint32_t invocations = 0;
	uint32_t nonfinite   = 0;
	uint32_t min_w       = std::numeric_limits<uint32_t>::max();
	uint32_t max_w       = 0;
	uint32_t min_x_w     = std::numeric_limits<uint32_t>::max();
	uint32_t max_x_w     = 0;
	uint32_t min_y_w     = std::numeric_limits<uint32_t>::max();
	uint32_t max_y_w     = 0;
	uint32_t min_z_w     = std::numeric_limits<uint32_t>::max();
	uint32_t max_z_w     = 0;
	uint32_t param0_exports   = 0;
	uint32_t param0_nonfinite = 0;
	uint32_t min_param0_x     = std::numeric_limits<uint32_t>::max();
	uint32_t max_param0_x     = 0;
	uint32_t min_param0_y     = std::numeric_limits<uint32_t>::max();
	uint32_t max_param0_y     = 0;
	uint32_t pixel_input0_samples   = 0;
	uint32_t pixel_input0_nonfinite = 0;
	uint32_t min_pixel_input0_x     = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_input0_x     = 0;
	uint32_t min_pixel_input0_y     = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_input0_y     = 0;
	// One runtime sample from the first MUBUF address setup reached by the
	// selected diagnostic VS. The atomic claim bounds this independently of
	// lanes, invocations, draws, and frames.
	uint32_t resolver_claimed           = 0;
	uint32_t resolver_instruction_pc    = 0;
	uint32_t resolver_access_width      = 0;
	uint32_t resolver_desc0             = 0;
	uint32_t resolver_desc1             = 0;
	uint32_t resolver_raw_byte_offset   = 0;
	uint32_t resolver_valid             = 0;
	uint32_t resolver_final_slot        = 0;
	uint32_t resolver_final_byte_offset = 0;
	// Mutually exclusive finite-position classes for each Vulkan clip-space
	// convention. w_nonpositive and xy_outside are shared; each convention then
	// splits the remaining positive-w, XY-inside positions into Z-outside/inside.
	uint32_t clip_w_nonpositive                 = 0;
	uint32_t clip_xy_outside                    = 0;
	uint32_t clip_z_outside_zero_to_one         = 0;
	uint32_t clip_inside_zero_to_one            = 0;
	uint32_t clip_z_outside_negative_one_to_one = 0;
	uint32_t clip_inside_negative_one_to_one    = 0;
	// Append-only post-sample aggregate for the explicitly selected
	// ImageSampleB. Keeping these ten words at the tail preserves the existing
	// raw member indices consumed by VS resolver and clip-volume diagnostics.
	uint32_t pixel_sample_invocations = 0;
	uint32_t pixel_sample_nonfinite   = 0;
	uint32_t min_pixel_sample_r       = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_sample_r       = 0;
	uint32_t min_pixel_sample_g       = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_sample_g       = 0;
	uint32_t min_pixel_sample_b       = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_sample_b       = 0;
	uint32_t min_pixel_sample_a       = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_sample_a       = 0;
	// Raster coverage for the selected final-MRT export. These coordinates are
	// diagnostic host Fragment coordinates, not guest shader values.
	uint32_t min_pixel_frag_x = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_frag_x = 0;
	uint32_t min_pixel_frag_y = std::numeric_limits<uint32_t>::max();
	uint32_t max_pixel_frag_y = 0;
};

struct VertexClipProbeResultInfo
{
	uint64_t checksum       = 0;
	uint64_t pixel_checksum = 0;
	bool     indexed        = false;
	uint32_t guest_count    = 0;
	uint32_t descriptor_set = kVertexClipProbeInvalidDescriptorSet;
};

enum class VertexClipProbeState
{
	Idle,
	Reserved,
	Recording,
	PendingFence,
	Completed,
};

class VertexClipProbeLifecycle
{
public:
	[[nodiscard]] VertexClipProbeState GetState() const { return m_state; }
	[[nodiscard]] bool Reserve(uint32_t match_ordinal = 0);
	[[nodiscard]] bool BeginRecording();
	[[nodiscard]] bool MarkPendingFence();
	[[nodiscard]] bool RetryAfterFence();
	[[nodiscard]] bool Complete();

private:
	VertexClipProbeState m_state                = VertexClipProbeState::Idle;
	uint32_t             m_matching_occurrences = 0;
};

[[nodiscard]] bool ShaderVertexClipProbeEligible(bool next_gen, bool vs_embedded);
[[nodiscard]] bool ShaderPixelInput0ProbeEligible(bool next_gen, bool ps_embedded);
[[nodiscard]] ShaderVertexClipProbeConfig ShaderResolveVertexClipProbeConfig(uint64_t code_id, bool indexed,
	                                                                           uint32_t guest_count);
[[nodiscard]] ShaderPixelInput0ProbeConfig ShaderResolvePixelInput0ProbeConfig(uint64_t code_id, bool indexed,
	                                                                              uint32_t guest_count, bool stage_enabled);
[[nodiscard]] ShaderPixelInput0ProbeConfig ShaderResolvePixelProbeConfig(uint64_t code_id, bool indexed,
	                                                                            uint32_t guest_count, bool stage_enabled);
	[[nodiscard]] bool                        VertexClipProbeCanReserveAtPresent(const ShaderVertexClipProbeConfig& config,
	                                                                             uint64_t present);
	[[nodiscard]] bool                        PixelInput0ProbeCanReserveAtPresent(const ShaderPixelInput0ProbeConfig& config,
	                                                                             uint64_t present);
	[[nodiscard]] bool                        VertexClipProbeStagesCanReserveAtPresent(
	                                           const ShaderVertexClipProbeConfig& vertex_config,
	                                           const ShaderPixelInput0ProbeConfig& pixel_config, uint64_t present);
	[[nodiscard]] bool                        VertexClipProbeValidatePairedPixelInstruction(
	                                           bool instruction_valid, ShaderVertexClipProbeConfig* vertex_config,
	                                           ShaderPixelInput0ProbeConfig* pixel_config);
	[[nodiscard]] bool                        VertexClipProbeCanReserveAutoDraw(const ShaderVertexClipProbeConfig& config,
	                                                                            bool clear_only);
	[[nodiscard]] uint64_t                    VertexClipProbeDiagnosticIdentity(uint32_t descriptor_set);
	[[nodiscard]] uint64_t PixelSampleProbeDiagnosticIdentity(uint32_t descriptor_set, uint32_t ordinal,
	                                                          bool sparse_subgroup = false);
[[nodiscard]] uint64_t                    PixelMrtProbeDiagnosticIdentity(uint32_t descriptor_set, uint32_t target,
                                                                          uint32_t ordinal);
[[nodiscard]] VertexClipProbeRawStats     VertexClipProbeInitialRawStats();
[[nodiscard]] uint32_t                    VertexClipProbeAttachmentFormatBytesPerPixel(VertexClipProbeAttachmentFormat format);
[[nodiscard]] bool VertexClipProbeAggregateAttachmentReadback(VertexClipProbeAttachmentFormat format, const uint8_t* data,
	                                                           uint64_t data_size, uint32_t width, uint32_t height,
	                                                           const VertexClipProbeRawStats& coverage,
	                                                           VertexClipProbeAttachmentReadbackStats* stats);
[[nodiscard]] bool VertexClipProbeAggregateAttachmentDelta(
	VertexClipProbeAttachmentFormat format, const uint8_t* before, const uint8_t* after, uint64_t data_size,
	uint32_t width, uint32_t height, const VertexClipProbeRawStats& coverage,
	VertexClipProbeAttachmentDeltaStats* stats);
[[nodiscard]] bool VertexClipProbeAttachmentShouldRetry(const VertexClipProbeRawStats& stats,
	                                                     uint32_t min_invocations, uint32_t retries);
[[nodiscard]] bool                        VertexClipProbeFormatResultMessage(const VertexClipProbeResultInfo& result,
	                                                                            const VertexClipProbeRawStats& stats, char* dst,
	                                                                            size_t dst_size);
[[nodiscard]] bool                        VertexClipProbeFormatPopulationResultMessage(
                                           const VertexClipProbeResultInfo& result, const VertexClipProbeRawStats& stats,
                                           char* dst, size_t dst_size);
[[nodiscard]] bool                        VertexClipProbeFormatParam0ResultMessage(const VertexClipProbeResultInfo& result,
	                                                                                  const VertexClipProbeRawStats& stats, char* dst,
	                                                                                  size_t dst_size);
	[[nodiscard]] bool                        VertexClipProbeFormatPixelInput0ResultMessage(const VertexClipProbeResultInfo& result,
	                                                                                      const VertexClipProbeRawStats& stats,
	                                                                                      char* dst, size_t dst_size);
	[[nodiscard]] bool                        VertexClipProbeFormatPixelSampleResultMessage(const VertexClipProbeResultInfo& result,
	                                                                                       uint32_t ordinal, const VertexClipProbeRawStats& stats,
	                                                                                       char* dst, size_t dst_size);
	[[nodiscard]] bool                        VertexClipProbeFormatPixelMrtResultMessage(const VertexClipProbeResultInfo& result,
	                                                                                    uint32_t target, uint32_t ordinal,
	                                                                                    const VertexClipProbeRawStats& stats, char* dst,
	                                                                                    size_t dst_size);
	[[nodiscard]] bool                        VertexClipProbeFormatPixelMrtCoverageResultMessage(
	                                           const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal,
	                                           const VertexClipProbeRawStats& stats, char* dst, size_t dst_size);
	[[nodiscard]] bool VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	    const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal, uint32_t match_ordinal,
	    uint32_t empty_retries, VertexClipProbeAttachmentStatus status,
	    const VertexClipProbeAttachmentReadbackStats* stats, char* dst, size_t dst_size);
	[[nodiscard]] bool VertexClipProbeFormatPixelMrtAttachmentDeltaMessage(
	    const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal, uint64_t guest_addr,
	    uint32_t match_ordinal,
	    uint32_t empty_retries, uint32_t load_op, VertexClipProbeAttachmentStatus status,
	    const VertexClipProbeAttachmentDeltaStats* stats, char* dst, size_t dst_size);
[[nodiscard]] bool                        VertexClipProbeFormatResolverResultMessage(const VertexClipProbeResultInfo& result,
	                                                                                   const VertexClipProbeRawStats& stats, char* dst,
	                                                                                   size_t dst_size);
[[nodiscard]] uint32_t                    VertexClipProbeEncodeOrderedFloat(float value);
[[nodiscard]] float                       VertexClipProbeDecodeOrderedFloat(uint32_t value);
[[nodiscard]] bool                        VertexClipProbeHasFiniteExtrema(const VertexClipProbeRawStats& stats);
[[nodiscard]] bool                        VertexClipProbeHasFiniteParam0Extrema(const VertexClipProbeRawStats& stats);
[[nodiscard]] bool                        VertexClipProbeHasFinitePixelInput0Extrema(const VertexClipProbeRawStats& stats);
[[nodiscard]] bool                        VertexClipProbeHasFinitePixelSampleExtrema(const VertexClipProbeRawStats& stats);
[[nodiscard]] bool                        VertexClipProbeHasFinitePixelMrtCoverageExtrema(const VertexClipProbeRawStats& stats);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VERTEXCLIPPROBE_H_ */
