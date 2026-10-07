#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSGESTATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSGESTATE_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
class Shader;
class UserConfig;
struct GsShaderResource1;
struct GsShaderResource2;
struct GeControl;
struct GeUserVgprEn;
} // namespace HW

struct GraphicsGeRawRegister
{
	uint32_t value = 0;
	// An assignment since reset, including a partial typed assignment.
	bool written = false;
	// The complete current raw word is available, not reconstructed from fields.
	bool known = false;

	void SetRaw(uint32_t raw)
	{
		value   = raw;
		written = true;
		known   = true;
	}
	// Zero is only a placeholder when !known, never evidence of a zero write.
	void SetDecoded()
	{
		value   = 0;
		written = true;
		known   = false;
	}
};

enum class GraphicsGeGeneration
{
	Unknown,
	Gfx10,
	Gfx103
};

enum class GraphicsGeStageKind
{
	Unknown,
	LegacyVs,
	NggPassthrough,
	MergedEsGs,
	Other
};

// Stage shape only. Neither a recognized kind nor a known field is backend
// admission. Scheduling flags and unknown bits remain available to the caller.
struct GraphicsGeStageState
{
	GraphicsGeRawRegister raw;
	GraphicsGeGeneration  generation = GraphicsGeGeneration::Unknown;
	GraphicsGeStageKind   kind       = GraphicsGeStageKind::Unknown;

	uint32_t raw_unknown_bits        = 0;
	uint8_t  ls_en                   = 0;
	bool     hs_en                   = false;
	uint8_t  es_en                   = 0;
	bool     gs_en                   = false;
	uint8_t  vs_en                   = 0;
	bool     dynamic_hs              = false;
	bool     dispatch_draw_en        = false;
	bool     dis_dealloc_accum_0     = false;
	bool     dis_dealloc_accum_1     = false;
	bool     vs_wave_id_en           = false;
	bool     primgen_en              = false;
	bool     ordered_id_mode         = false;
	uint8_t  max_primgrp_in_wave     = 0;
	uint8_t  gs_fast_launch          = 0;
	bool     hs_w32_en               = false;
	bool     gs_w32_en               = false;
	bool     vs_w32_en               = false;
	bool     ngg_wave_id_en          = false;
	bool     primgen_passthru_en     = false;
	bool     primgen_passthru_no_msg = false; // GFX10.3 only
	uint32_t gs_wave_lanes           = 0;     // Zero means unknown, not an empty wave.
};

struct GraphicsGeNggSubgroupControl
{
	uint32_t raw                            = 0;
	uint32_t raw_unknown_bits               = 0;
	uint16_t primitive_amplification_factor = 0;
	uint16_t threads_per_subgroup           = 0; // Encoded field, not a launch thread count.
};

[[nodiscard]] GraphicsGeStageState         GraphicsDecodeGeStages(GraphicsGeRawRegister raw, GraphicsGeGeneration generation);
[[nodiscard]] GraphicsGeNggSubgroupControl GraphicsDecodeGeNggSubgroupControl(uint32_t raw);
[[nodiscard]] HW::GsShaderResource1        GraphicsDecodeGsShaderResource1(uint32_t raw);
[[nodiscard]] HW::GsShaderResource2        GraphicsDecodeGsShaderResource2(uint32_t raw);
[[nodiscard]] HW::GeControl                GraphicsDecodeGeControl(uint32_t raw);
[[nodiscard]] HW::GeUserVgprEn             GraphicsDecodeGeUserVgprEn(uint32_t raw);

// Single-register entry points used by indirect PM4 and by the bounded direct
// range adapters below. Unknown offsets return false without modifying state.
[[nodiscard]] bool GraphicsDecodeGeShaderRegister(HW::Shader& shader, uint32_t offset, uint32_t value);
[[nodiscard]] bool GraphicsDecodeGeUserConfigRegister(HW::UserConfig& ucfg, uint32_t offset, uint32_t value);
// Direct SET_SH_REG / SET_UCONFIG_REG adapters, implemented with the register
// parsers. Validate the entire range before the first assignment.
[[nodiscard]] bool GraphicsDecodeGeShaderRegisters(HW::Shader* shader, uint32_t offset, const uint32_t* values, uint32_t count);
[[nodiscard]] bool GraphicsDecodeGeUserConfigRegisters(HW::UserConfig* ucfg, uint32_t offset, const uint32_t* values, uint32_t count);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSGESTATE_H_ */
