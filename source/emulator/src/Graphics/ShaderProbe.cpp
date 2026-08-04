#include "ShaderLogInternal.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "ShaderSpirvToolchain.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

uint64_t ShaderCodeId(const ShaderCode& code)
{
	return (static_cast<uint64_t>(code.GetHash0()) << 32u) | static_cast<uint64_t>(code.GetCrc32());
}

static bool ShaderProbeMatches(const ShaderCode& code)
{
	const char* value = std::getenv("KYTY_SHADER_PROBE_ID");
	if (value == nullptr || value[0] == '\0')
	{
		return false;
	}

	// Dump every matching stage when ID is "all" (bounded diagnostic only).
	if (std::strcmp(value, "all") == 0)
	{
		return true;
	}

	char*      end    = nullptr;
	const auto parsed = std::strtoull(value, &end, 16);
	if (end == value || *end != '\0')
	{
		KYTY_LOG_WARN("WARNING: invalid KYTY_SHADER_PROBE_ID value: %s\n", value);
		return false;
	}

	return parsed == ShaderCodeId(code) || parsed == code.GetCrc32();
}

static String ShaderProbeFolder()
{
	const char* value = std::getenv("KYTY_SHADER_PROBE_FOLDER");
	if (value != nullptr && value[0] != '\0')
	{
		return String::FromUtf8(value);
	}
	return Config::GetShaderLogFolder();
}

void ShaderProbeWrite(const char* stage, const ShaderCode& code, const String8* source, const Vector<uint32_t>* binary)
{
	if (!ShaderProbeMatches(code))
	{
		return;
	}

	const auto file_name = ShaderProbeFolder().FixDirectorySlash() +
	                       String::FromPrintf("shader_probe_%s_%08" PRIx32 "_%08" PRIx32 ".log", stage, code.GetHash0(),
	                                          code.GetCrc32());

	Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());

	Core::File file;
	file.Create(file_name);
	if (file.IsInvalid())
	{
		KYTY_LOG_WARN("WARNING: shader probe could not create %s\n", file_name.C_Str());
		return;
	}

	file.Printf("stage = %s\n", stage);
	file.Printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
	file.Printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
	file.Printf("id = %016" PRIx64 "\n", ShaderCodeId(code));
	file.Printf("--------- Original Shader ---------\n");
	const auto original = code.DbgDump();
	file.Printf("%s", original.c_str());

	if (source != nullptr)
	{
		file.Printf("--------- Recompiled Shader ---------\n");
		file.Printf("%s\n", source->c_str());
	}

	if (binary != nullptr && binary->Size() > 0)
	{
		String8 text;
		if (!ShaderToolchain::Disassemble(binary->GetDataConst(), binary->Size(), &text))
		{
			file.Printf("WARNING: SpirvDisassemble failed\n");
		} else
		{
			file.Printf("--------- Optimized Shader ---------\n");
			file.Printf("%s\n", Log::RemoveColors(String::FromUtf8(text.c_str())).C_Str());
		}
	}

	file.Close();
	KYTY_LOG_DEBUG( "KYTY_SHADER_PROBE wrote %s\n", file_name.C_Str());
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
