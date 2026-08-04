#ifndef EMULATOR_SRC_GRAPHICS_SHADERDEBUGINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADERDEBUGINTERNAL_H_

#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct ShaderBinaryInfo
{
	uint8_t  signature[7];
	uint8_t  version;
	uint32_t pssl_or_cg  : 1;
	uint32_t cached      : 1;
	uint32_t type        : 4;
	uint32_t source_type : 2;
	uint32_t length      : 24;
	uint8_t  chunk_usage_base_offset_dw;
	uint8_t  num_input_usage_slots;
	uint8_t  is_srt                 : 1;
	uint8_t  is_srt_used_info_valid : 1;
	uint8_t  is_extended_usage_info : 1;
	uint8_t  reserved2              : 5;
	uint8_t  reserved3;
	uint32_t hash0;
	uint32_t hash1;
	uint32_t crc32;
};

struct ShaderUsageSlot
{
	uint8_t type;
	uint8_t slot;
	uint8_t start_register;
	uint8_t flags;
};

struct ShaderUsageInfo
{
	const uint32_t*        usage_masks = nullptr;
	const ShaderUsageSlot* slots       = nullptr;
	int                    slots_num   = 0;
	bool                   valid       = false;
};

struct ShaderDebugPrintfCmds
{
	uint64_t                  id = 0;
	Vector<ShaderDebugPrintf> cmds;
};

// Debug dump helpers shared between the shader analysis unit and the debug
// unit. Used by ShaderParseVS/PS/CS for diagnostic output.
void vs_print(const char* func, const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh);
void ps_print(const char* func, const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh);
void cs_print(const char* func, const HW::CsStageRegisters& cs, const HW::ShaderRegisters& sh);
void bi_print(const char* func, const ShaderBinaryInfo& bi);
void vs_check(const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh);
void ps_check(const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh);
void cs_check(const HW::CsStageRegisters& cs, const HW::ShaderRegisters& sh);

// Diagnostic counters recorded by the shader analysis paths.
void RecordShaderInputAnalysis(uint64_t elapsed_ns);
void RecordShaderPipelineMissParse(uint64_t elapsed_ns);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERDEBUGINTERNAL_H_ */
