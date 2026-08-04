#include "ShaderLogInternal.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Shader.h"
#include "ShaderSpirvToolchain.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

ShaderLogHelper::ShaderLogHelper(const char* type)
{
	static std::atomic_int id = 0;

	switch (Config::GetShaderLogDirection())
	{
		case Config::ShaderLogDirection::Console:
			m_console = true;
			m_enabled = true;
			break;
		case Config::ShaderLogDirection::File:
		{
			m_file_name = Config::GetShaderLogFolder().FixDirectorySlash() +
			              String::FromPrintf("%04d_%04d_shader_%s.log", GraphicsRunGetFrameNum(), id++, type);
			Core::File::CreateDirectories(m_file_name.DirectoryWithoutFilename());
			m_file.Create(m_file_name);
			if (m_file.IsInvalid())
			{
				KYTY_LOG_DEBUG(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, m_file_name.C_Str());
				m_enabled = false;
			}
			m_enabled = true;
			m_console = false;
		}
		break;
		default: m_enabled = false; break;
	}
}

ShaderLogHelper::~ShaderLogHelper()
{
	if (m_enabled && !m_console && !m_file.IsInvalid())
	{
		m_file.Close();
	}
}

void ShaderLogHelper::DumpOriginalShader(const ShaderCode& code)
{
	if (m_enabled)
	{
		if (m_console)
		{
			KYTY_LOG_DEBUG("--------- Original Shader ---------\n");
			KYTY_LOG_DEBUG("crc32 = %08" PRIx32 "\n", code.GetCrc32());
			KYTY_LOG_DEBUG("hash0 = %08" PRIx32 "\n", code.GetHash0());
			KYTY_LOG_DEBUG("---------\n");
			KYTY_LOG_DEBUG("%s", code.DbgDump().c_str());
			KYTY_LOG_DEBUG("---------\n");
		} else if (!m_file.IsInvalid())
		{
			m_file.Printf("--------- Original Shader ---------\n");
			m_file.Printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
			m_file.Printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
			m_file.Printf("---------\n");
			m_file.Printf("%s", code.DbgDump().c_str());
			m_file.Printf("---------\n");
		}
	}
}

void ShaderLogHelper::DumpRecompiledShader(const String8& source)
{
	if (m_enabled)
	{
		if (m_console)
		{
			KYTY_LOG_DEBUG("--------- Recompiled Shader ---------\n");
			KYTY_LOG_DEBUG("%s\n", source.c_str());
			KYTY_LOG_DEBUG("---------\n");
		} else if (!m_file.IsInvalid())
		{
			m_file.Printf("--------- Recompiled Shader ---------\n");
			m_file.Printf("%s\n", source.c_str());
			m_file.Printf("---------\n");
		}
	}
}

void ShaderLogHelper::DumpOptimizedShader(const Vector<uint32_t>& bin)
{
	if (m_enabled)
	{
		String8 text;
		if (!ShaderToolchain::Disassemble(bin.GetDataConst(), bin.Size(), &text))
		{
			KYTY_LOG_DEBUG("WARNING: SpirvDisassemble failed (continuing)\n");
		}
		if (m_console)
		{
			KYTY_LOG_DEBUG("--------- Optimized Shader ---------\n");
			KYTY_LOG_DEBUG("%s\n", text.c_str());
			KYTY_LOG_DEBUG("---------\n");
		} else if (!m_file.IsInvalid())
		{
			m_file.Printf("--------- Optimized Shader ---------\n");
			m_file.Printf("%s\n", Log::RemoveColors(String::FromUtf8(text.c_str())).C_Str());
			m_file.Printf("---------\n");
		}
	}
}

void ShaderLogHelper::DumpGlslShader(const Vector<uint32_t>& bin)
{
	if (m_enabled)
	{
		String8 text;
		if (!ShaderToolchain::ToGlsl(bin.GetDataConst(), bin.Size(), &text))
		{
			KYTY_LOG_DEBUG("WARNING: SpirvToGlsl failed (continuing)\n");
		}
		if (m_console)
		{
			KYTY_LOG_DEBUG("--------- Glsl Shader ---------\n");
			KYTY_LOG_DEBUG("%s\n", text.c_str());
			KYTY_LOG_DEBUG("---------\n");
		} else if (!m_file.IsInvalid())
		{
			m_file.Printf("--------- Glsl Shader ---------\n");
			m_file.Printf("%s\n", Log::RemoveColors(String::FromUtf8(text.c_str())).C_Str());
			m_file.Printf("---------\n");
		}
	}
}

void ShaderLogHelper::DumpBinary(const Vector<uint32_t>& bin)
{
	if (m_enabled && !m_console && !m_file.IsInvalid())
	{
		Core::File file;
		String     file_name = m_file_name.FilenameWithoutExtension() + ".spv";
		file.Create(file_name);
		if (file.IsInvalid())
		{
			KYTY_LOG_DEBUG(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
		} else
		{
			file.Write(bin.GetDataConst(), bin.Size() * 4);
			file.Close();
		}
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
