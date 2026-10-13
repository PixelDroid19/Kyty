#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSCALARLIVENESS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSCALARLIVENESS_H_

#include "Emulator/Graphics/Shader.h"

#include <array>
#include <bitset>
#include <memory>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr int kShaderScalarLivenessSgprs = 106;

// SGPRs whose entry value some path reads before redefining it. Uses and
// definitions come from each instruction's operands over the decoded CFG
// (branches, conditional fallthrough, s_endpgm). When the CFG cannot be
// resolved, every SGPR is reported live so callers stay conservative.
[[nodiscard]] std::bitset<kShaderScalarLivenessSgprs> ShaderSgprsLiveAtEntry(const ShaderCode& code);

// Per instruction index: SGPRs that still hold their entry (user data) value
// on every path reaching that instruction. Empty sets when the CFG cannot be
// resolved, so callers never treat a redefined register as user data.
[[nodiscard]] std::vector<std::bitset<kShaderScalarLivenessSgprs>> ShaderSgprsHoldingEntryValue(const ShaderCode& code);

// Per instruction index and SGPR: the SGPR whose entry value it holds on every
// path reaching that instruction (itself while untouched, another one after
// S_MOV copies), or kShaderSgprNoEntrySource. All unknown when the CFG cannot
// be resolved.
constexpr int8_t kShaderSgprNoEntrySource = -1;
using ShaderSgprEntrySources              = std::array<int8_t, kShaderScalarLivenessSgprs>;
[[nodiscard]] std::vector<ShaderSgprEntrySources> ShaderSgprEntrySourcesAt(const ShaderCode& code);

// The three results above use wave64 lane masks. A V_CMP or carry-out names an
// SGPR pair; a wave32 program writes only its low dword, so the wave64 reading
// overstates its definitions and stays conservative.
//
// The results for one program and guest wave size, computed once per distinct
// pair and shared: per-draw callers read them without copying. Any size other
// than 32 reads as wave64.
struct ShaderScalarFlow
{
	std::bitset<kShaderScalarLivenessSgprs>              live_at_entry;
	std::vector<ShaderSgprEntrySources>                  entry_sources;
	std::vector<std::bitset<kShaderScalarLivenessSgprs>> holding_entry_value;
};
[[nodiscard]] std::shared_ptr<const ShaderScalarFlow> ShaderScalarFlowOf(const ShaderCode& code, uint32_t guest_wave_size = 64u);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSCALARLIVENESS_H_
