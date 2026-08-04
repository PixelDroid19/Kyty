#include "ShaderSpirvToolchain.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Log.h"

#include "spirv-tools/libspirv.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

static bool SpirvDisassemble(const uint32_t* src_binary, size_t src_binary_size, String8* dst_disassembly)
{
	if (dst_disassembly != nullptr)
	{
		spvtools::SpirvTools core(SPV_ENV_VULKAN_1_2);

		std::string disassembly;
		if (!core.Disassemble(src_binary, src_binary_size, &disassembly,
		                      static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR)))
		{
			*dst_disassembly = disassembly.c_str();

			printf("Disassemble failed\n");
			return false;
		}

		*dst_disassembly = disassembly.c_str();
	}
	return true;
}

static bool SpirvToGlsl(const uint32_t* /*src_binary*/, size_t /*src_binary_size*/, String8* /*dst_code*/)
{
	//	if (dst_code != nullptr)
	//	{
	//		spirv_cross::CompilerGLSL glsl(src_binary, src_binary_size);
	//
	//		std::string source = glsl.compile();
	//
	//		*dst_code = source.c_str();
	//	}
	return true;
}

static bool SpirvCompile(const String8& src, Vector<uint32_t>* dst, String8* err_msg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(err_msg == nullptr);

	spvtools::SpirvTools core(SPV_ENV_VULKAN_1_2);
	spvtools::Optimizer  opt(SPV_ENV_VULKAN_1_2);

	spv_position_t error_position {};
	String8        error_msg;

	auto print_msg_to_stderr = [&error_position, &error_msg](spv_message_level_t /* level */, const char* /*source*/,
	                                                         [[maybe_unused]] const spv_position_t& position, const char* m)
	{
		// printf("%s\n", source);
		error_msg = String8::FromPrintf("%d: %d (%d) %s", static_cast<int>(position.line), static_cast<int>(position.column),
		                                static_cast<int>(position.index), m);
		printf(FG_BRIGHT_RED "error: %s\n" FG_DEFAULT, error_msg.c_str());
		error_position = position;
	};
	core.SetMessageConsumer(print_msg_to_stderr);
	opt.SetMessageConsumer(print_msg_to_stderr);

	dst->Clear();

	std::vector<uint32_t> spirv;
	if (!core.Assemble(src.GetDataConst(), src.Size(), &spirv))
	{
		printf("Assemble failed at:\n%s\n", src.Mid(src.FindIndex('\n', error_position.index - 100), 200).c_str());
		*err_msg = String8::FromPrintf("Assemble failed: %s\n%s\n", error_msg.c_str(),
		                              src.Mid(src.FindIndex('\n', error_position.index - 100), 200).c_str());
		return false;
	}

	if (Config::ShaderValidationEnabled() && !core.Validate(spirv))
	{
		String8 disassembly;
		SpirvDisassemble(spirv.data(), spirv.size(), &disassembly);
		printf("%s\n", disassembly.c_str());
		printf("Validate failed\n");
		*err_msg = String8::FromPrintf("%s\n\nValidate failed:\n%s\n", Log::RemoveColors(String::FromUtf8(disassembly.c_str())).C_Str(),
		                               error_msg.c_str());
		return false;
	}

	bool optimize = true;
	switch (Config::GetShaderOptimizationType())
	{
		case Config::ShaderOptimizationType::Performance: opt.RegisterPerformancePasses(); break;
		case Config::ShaderOptimizationType::Size: opt.RegisterSizePasses(); break;
		default: optimize = false; break;
	}

	if (optimize && !opt.Run(spirv.data(), spirv.size(), &spirv))
	{
		// Keep the assembled module when the optimizer rejects structured CFG
		// that the driver may still accept. Prefer optimized output when Run
		// succeeds; never invent a replacement module.
		printf("WARNING: Optimize failed, using unoptimized SPIR-V: %s\n", error_msg.c_str());
		error_msg.Clear();
	}

	dst->Add(spirv.data(), spirv.size());

	return true;
}

static bool SpirvRun(const String8& src, Vector<uint32_t>* dst, String8* err_msg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(err_msg == nullptr);

	const auto optimization = static_cast<uint32_t>(Config::GetShaderOptimizationType());
	const bool validation   = Config::ShaderValidationEnabled();
	auto&      cache        = SpirvBinaryCacheDefaultStore();
	const auto lookup       = cache.Load(src, optimization, validation, dst);
	if (lookup == SpirvBinaryCacheLoadResult::Hit)
	{
		return true;
	}

	bool compiled = false;
	{
		DebugStatsScopedTimer timer(DebugStatsRecordSpirvCompile);
		compiled = SpirvCompile(src, dst, err_msg);
	}
	if (compiled)
	{
		(void)cache.QueueStore(src, optimization, validation, *dst);
	}
	return compiled;
}

} // namespace

namespace ShaderToolchain {

bool Disassemble(const uint32_t* src_binary, size_t src_binary_size, String8* dst_disassembly)
{
	return SpirvDisassemble(src_binary, src_binary_size, dst_disassembly);
}

bool ToGlsl(const uint32_t* src_binary, size_t src_binary_size, String8* dst_code)
{
	return SpirvToGlsl(src_binary, src_binary_size, dst_code);
}

bool Run(const String8& src, Vector<uint32_t>* dst, String8* err_msg)
{
	return SpirvRun(src, dst, err_msg);
}

} // namespace ShaderToolchain

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
