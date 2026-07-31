#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVTEMPLATES_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVTEMPLATES_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

extern const char FUNC_FETCH_4[];
extern const char FUNC_FETCH_3[];
extern const char FUNC_FETCH_2[];
extern const char FUNC_FETCH_1[];
extern const char FUNC_ABS_DIFF[];
extern const char FUNC_WQM[];
extern const char FUNC_ADDC[];
extern const char FUNC_LSHL_ADD[];
extern const char FUNC_MIPMAP[];
extern const char FUNC_ORDERED[];
extern const char FUNC_MUL_EXTENDED[];
extern const char FUNC_SHIFT_RIGHT[];
extern const char FUNC_SHIFT_LEFT[];
extern const char BUFFER_LOAD_UBYTE[];
extern const char BUFFER_RAW_ADDRESS[];
extern const char BUFFER_LOAD_FLOAT1[];
extern const char BUFFER_LOAD_FLOAT4[];
extern const char BUFFER_STORE_FLOAT1[];
extern const char BUFFER_STORE_FLOAT2[];
extern const char BUFFER_STORE_FLOAT4[];
extern const char TBUFFER_LOAD_FORMAT_XYZW[];
extern const char TBUFFER_FORMAT_SCALAR32[];
extern const char TBUFFER_LOAD_FORMAT_X[];
extern const char TBUFFER_LOAD_FORMAT_XY[];
extern const char TBUFFER_STORE_FORMAT_X[];
extern const char TBUFFER_STORE_FORMAT_XY[];
extern const char TBUFFER_STORE_FORMAT_XYZW[];
extern const char SBUFFER_LOAD_DWORD[];
extern const char SBUFFER_LOAD_DWORD_2[];
extern const char SBUFFER_LOAD_DWORD_4[];
extern const char SBUFFER_LOAD_DWORD_8[];
extern const char SBUFFER_LOAD_DWORD_16[];
extern const char EMBEDDED_SHADER_VS_0[];
extern const char EMBEDDED_SHADER_PS_0[];
extern const char EXECZ[];
extern const char SCC_NZ_1[];
extern const char SCC_NZ_2[];
extern const char SCC_EXEC_NZ_2[];
extern const char SCC_OVERFLOW_ADD_1[];
extern const char SCC_OVERFLOW_SUB_1[];
extern const char SCC_CARRY_1[];
extern const char CLAMP[];
extern const char MULTIPLY[];

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVTEMPLATES_H_ */
