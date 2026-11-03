#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct ShaderVertexInputInfo;
struct ShaderPixelInputInfo;
struct ShaderComputeInputInfo;

// Host-only input contract for one logical wave64 per compute workgroup.
// Each input lane contains flags (allocated=1, initial EXEC=2, covered=4),
// raw VGPR words, then 16 words per attribute: four native interpolated
// values and twelve raw parameter words in P10/P20/P0 order.
// Each output lane contains pixel validity, a 32-bit MRT component mask and
// 32 raw FP32 words. Export VM updates validity independently of color data.
// The caller must supply captured values and enable linear compute derivatives
// plus required full subgroups of 32; this interface does not select a draw path.
// If user_sgpr_count is specified from the PS resource register, each input
// wave starts with its raw parameter-state SGPR word, followed by 64 lanes.
// That word and the parameter triples must describe the same parameter cache.
// Virtualized uses a normalized selector into the host's primitive-owned
// captured block, without a raw wave header. It requires proof that the initial
// word reaches M0 alone and that M0 never escapes into guest data or control.
enum class ShaderFragmentParameterState
{
	Captured,
	Virtualized
};

struct ShaderFragmentComputeInfo
{
	uint32_t                     descriptor_set     = 1;
	uint32_t                     input_binding      = 0;
	uint32_t                     output_binding     = 1;
	// Optional transport control buffer; word two is the active wave count.
	uint32_t                     control_binding    = UINT32_MAX;
	uint32_t                     initial_vgpr_count = 0;
	uint32_t                     user_sgpr_count    = UINT32_MAX;
	ShaderFragmentParameterState parameter_state    = ShaderFragmentParameterState::Captured;

	[[nodiscard]] uint32_t HeaderWords() const
	{
		return user_sgpr_count != UINT32_MAX && parameter_state == ShaderFragmentParameterState::Captured ? 1u : 0u;
	}
};

enum class ShaderGeometryInputTopology
{
	TriangleList,
	TriangleStrip,
	PointList
};

// Host-only contract for a merged ES+GS (NGG) program run as a compute pass.
// Each workgroup is one guest subgroup of `waves` Wave64s holding up to
// primitives_per_group input primitives of one instance, one ES thread per
// corner (three per triangle, one per point); subgroup thread T is lane T % 64
// of wave T / 64. The prologue stands in for the geometry engine: s3 carries
// each wave's counts, v0/v1 the ES slot of each corner (slot *
// kGeometryVertexOffsetScale), v2 the primitive id, v5/v8 the vertex and
// instance ids the ES fetches with.
// Exports and the allocation request land in an output buffer addressed by
// the geometry push block (ShaderBindResources::geometry_offset_dw):
// row 0 = {output_lo, output_hi, primitives per instance, groups per instance},
// row 1 = {first vertex, first instance, guest index address lo, hi}.
// An indexed draw resolves each slot vertex through the guest index buffer at
// that address; a non-indexed draw numbers its vertices from the first vertex.
// Output group g holds one header word (the GS_ALLOC_REQ payload), one
// primitive word per subgroup thread, then one record per thread: pos0, pos1, params.
struct ShaderGeometryComputeInfo
{
	ShaderGeometryInputTopology topology             = ShaderGeometryInputTopology::TriangleList;
	uint32_t                    primitives_per_group = 0;
	uint32_t                    parameter_count      = 0;
	// Guest LDS allocation of the merged stage, in dwords.
	uint32_t                    lds_dwords           = 0;
	uint32_t                    waves                = 1;
	// The draw is indexed: slot vertices come from the guest index buffer
	// (address in row 1) whose elements are 32-bit unless index_format says 16.
	bool                        indexed              = false;
	uint32_t                    index_format         = 0;
	// Host subgroup width of each guest wave's parts (ShaderSplitWavePartLanes).
	uint32_t                    part_lanes           = 32;

	static constexpr uint32_t kLanes                    = 64;
	// An NGG subgroup holds at most 256 threads.
	static constexpr uint32_t kMaxWaves                 = 4;
	static constexpr uint32_t kVertexOffsetScale        = 4;
	static constexpr uint32_t kPrimitiveWordsOffset     = 1;
	[[nodiscard]] uint32_t Corners() const { return topology == ShaderGeometryInputTopology::PointList ? 1u : 3u; }
	[[nodiscard]] uint32_t GroupLanes() const { return kLanes * waves; }
	[[nodiscard]] uint32_t RecordsOffset() const { return kPrimitiveWordsOffset + GroupLanes(); }
	[[nodiscard]] uint32_t RecordDwords() const { return 8u + 4u * parameter_count; }
	[[nodiscard]] uint32_t GroupDwords() const { return RecordsOffset() + GroupLanes() * RecordDwords(); }
};

// Highest interpolant the program reads, or the register count when larger.
// Native, capture and shade generators must agree on it for one program.
uint32_t SpirvResolvePixelParameterCount(const ShaderCode& code, uint32_t register_count);
String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info);
// Emits a guest pixel program as a split Wave64 compute module over the
// captured per-lane records described by ShaderFragmentComputeInfo.
String8 SpirvGenerateFragmentComputeSource(const ShaderCode& code, const ShaderPixelInputInfo& ps_input_info,
                                           const ShaderComputeInputInfo& cs_input_info,
                                           const ShaderFragmentComputeInfo& fragment_compute_info);
// Highest vertex parameter the program exports, or the declared count when larger.
uint32_t SpirvResolveVertexParameterCount(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info);
// `code` is the linked ES+GS program; `vs_input_info` its fused-front input with
// the geometry push block and guest device addressing enabled.
String8 SpirvGenerateGeometryComputeSource(const ShaderCode& code, const ShaderVertexInputInfo& vs_input_info,
                                           const ShaderGeometryComputeInfo& geometry);
// The compute-stage contract that pass runs under: `waves` split Wave64s per
// workgroup over the guest LDS allocation.
ShaderComputeInputInfo SpirvGeometryComputeHostInfo(const ShaderCode& code, const ShaderVertexInputInfo& vs_input_info,
                                                    const ShaderGeometryComputeInfo& geometry);
// Host vertex stage drawing that pass's output as groups * GroupLanes() triangles:
// vertex v is corner v % 3 of output primitive (v / 3) % lanes of group v / (3 * lanes).
// Unallocated, null or out-of-range primitives collapse to the origin. Its
// push block holds the output address in the first two words.
String8 SpirvGenerateGeometryPullSource(const ShaderGeometryComputeInfo& geometry, bool layer);
String8 SpirvGetEmbeddedVs(uint32_t id);
String8 SpirvGetEmbeddedPs(uint32_t id);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_ */
