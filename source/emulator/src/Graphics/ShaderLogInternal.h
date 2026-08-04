#ifndef EMULATOR_SRC_GRAPHICS_SHADERLOGINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADERLOGINTERNAL_H_

#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Console/file dump helper for shader translations. Instantiated by the shader
// parse entry points; owned by its translation unit.
class ShaderLogHelper
{
public:
	explicit ShaderLogHelper(const char* type);
	virtual ~ShaderLogHelper();

	KYTY_CLASS_NO_COPY(ShaderLogHelper);

	void DumpOriginalShader(const ShaderCode& code);
	void DumpRecompiledShader(const String8& source);
	void DumpOptimizedShader(const Vector<uint32_t>& bin);
	void DumpGlslShader(const Vector<uint32_t>& bin);
	void DumpBinary(const Vector<uint32_t>& bin);

private:
	bool       m_console = false;
	bool       m_enabled = false;
	Core::File m_file;
	String     m_file_name;
};

// Optional shader source/binary probe dump, gated by the KYTY_SHADER_PROBE
// environment variable. Used by the shader parse entry points.
void ShaderProbeWrite(const char* stage, const ShaderCode& code, const String8* source, const Vector<uint32_t>* binary);

// Stable identity for a parsed shader (hash0<<32 | crc32). Used to key the
// pipeline cache and the disabled-shader set.
uint64_t ShaderCodeId(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERLOGINTERNAL_H_ */
