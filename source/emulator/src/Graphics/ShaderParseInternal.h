#ifndef EMULATOR_SRC_GRAPHICS_SHADERPARSEINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADERPARSEINTERNAL_H_

#include "Emulator/Graphics/ShaderParse.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"

#include "ShaderParseOperands.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

#define KYTY_SHADER_PARSER_ARGS                                                                                                            \
	[[maybe_unused]] uint32_t pc, [[maybe_unused]] const uint32_t *src, [[maybe_unused]] const uint32_t *buffer,                           \
	    [[maybe_unused]] ShaderCode *dst, [[maybe_unused]] bool next_gen
#define KYTY_SHADER_PARSER(f) uint32_t f(KYTY_SHADER_PARSER_ARGS)
#define KYTY_CP_OP_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id, [[maybe_unused]] const uint32_t *buffer,                      \
	    [[maybe_unused]] uint32_t dw, [[maybe_unused]] uint32_t num_dw
#define KYTY_CP_OP_PARSER(f) static uint32_t f(KYTY_CP_OP_PARSER_ARGS)

#define KYTY_TYPE_STR(s) [[maybe_unused]] static const char* type_str = s;
#define KYTY_NI(i)                                                                                                                         \
	KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());                                                                                                  \
	EXIT("unknown %s instruction %s, opcode = 0x%" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n", \
	     type_str, i, opcode, pc, dst->GetHash0(), dst->GetCrc32());
#define KYTY_UNKNOWN_OP()                                                                                                                  \
	KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());                                                                                                  \
	EXIT("unknown %s opcode: 0x%" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n", type_str,        \
	     opcode, pc, dst->GetHash0(), dst->GetCrc32());

namespace Kyty::Libs::Graphics {

// Instruction-family parsers. Each lives in its own translation unit
// (ShaderParse<SOPC|SOPK|...|VINTRP>.cpp); shader_parse dispatches to them.
KYTY_SHADER_PARSER(shader_parse_sopc);
KYTY_SHADER_PARSER(shader_parse_sopk);
KYTY_SHADER_PARSER(shader_parse_sopp);
KYTY_SHADER_PARSER(shader_parse_sop1);
KYTY_SHADER_PARSER(shader_parse_sop2);
KYTY_SHADER_PARSER(shader_parse_vopc);
KYTY_SHADER_PARSER(shader_parse_vop1);
KYTY_SHADER_PARSER(shader_parse_vop2);
KYTY_SHADER_PARSER(shader_parse_vop3);
KYTY_SHADER_PARSER(shader_parse_exp);
KYTY_SHADER_PARSER(shader_parse_smem);
KYTY_SHADER_PARSER(shader_parse_smrd);
KYTY_SHADER_PARSER(shader_parse_mubuf);
KYTY_SHADER_PARSER(shader_parse_ds);
KYTY_SHADER_PARSER(shader_parse_mimg);
KYTY_SHADER_PARSER(shader_parse_mtbuf);
KYTY_SHADER_PARSER(shader_parse_vintrp);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERPARSEINTERNAL_H_ */
