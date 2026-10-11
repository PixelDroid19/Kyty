#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorShaderWideBufferOffset);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t kEndProgram = 0xbf810000u;
constexpr uint32_t kBufferBaseRegister = 16u;
constexpr uint32_t kOffsetRegister = 24u;
constexpr uint32_t kRegisterOffset = 0x1234u;
constexpr uint32_t kInstructionImmediate = 0x28u;

void InitializeShaderTest()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
}

std::vector<uint32_t> SBufferLoadWords(uint32_t components, uint32_t soffset, uint32_t immediate)
{
	const uint32_t opcode = components == 8u ? 0x0bu : 0x0cu;
	const uint32_t sdst   = 32u;
	const uint32_t sbase  = kBufferBaseRegister / 2u;
	const uint32_t word0  = (0x3du << 26u) | (opcode << 18u) | (sdst << 6u) | sbase;
	const uint32_t word1  = (soffset << 25u) | (immediate & 0x1fffffu);
	return {word0, word1, kEndProgram};
}

ShaderCode ParseWideLoad(uint32_t components, uint32_t soffset, uint32_t immediate)
{
	const auto words = SBufferLoadWords(components, soffset, immediate);
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code) ||
	    code.GetInstructions().Size() != 2u)
	{
		ADD_FAILURE() << "production parser rejected the synthetic wide scalar-buffer load";
		return code;
	}
	return code;
}

ShaderComputeInputInfo MakeInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1u;
	input.threads_num[1] = 1u;
	input.threads_num[2] = 1u;
	input.bind.push_constant_size = 16u;
	auto& storage = input.bind.storage_buffers;
	storage.buffers_num        = 1;
	storage.start_register[0]  = static_cast<int>(kBufferBaseRegister);
	storage.sources[0]         = ShaderStorageBindingSource::DirectResource;
	storage.usages[0]          = ShaderStorageUsage::ReadOnly;
	storage.accesses[0]        = ShaderStorageAccess::Typed;
	storage.buffers[0].fields[1] = 16u << 16u;
	storage.buffers[0].fields[2] = 4u;
	return input;
}

std::vector<std::vector<std::string>> Tokenize(const String8& source)
{
	std::vector<std::vector<std::string>> instructions;
	std::istringstream input(source.c_str());
	std::string line;
	while (std::getline(input, line))
	{
		line = line.substr(0, line.find(';'));
		std::istringstream words(line);
		std::vector<std::string> tokens;
		std::string token;
		while (words >> token) { tokens.push_back(token); }
		if (!tokens.empty()) { instructions.push_back(std::move(tokens)); }
	}
	return instructions;
}

bool EvaluateWideLoadAddress(const String8& source, uint32_t components, uint32_t register_offset, uint32_t* address)
{
	if (address == nullptr) { return false; }
	const auto helper = components == 8u ? "%sbuffer_load_dword_8" : "%sbuffer_load_dword_16";
	const auto instructions = Tokenize(source);
	std::unordered_map<std::string, uint32_t> values;
	std::unordered_map<std::string, uint32_t> memory;

	for (const auto& tokens: instructions)
	{
		if (tokens.size() >= 5u && tokens[1] == "=" && tokens[2] == "OpConstant" &&
		    (tokens[3] == "%uint" || tokens[3] == "%int"))
		{
			values[tokens[0]] = static_cast<uint32_t>(std::strtoll(tokens[4].c_str(), nullptr, 0));
		}
	}

	bool in_main = false;
	for (const auto& tokens: instructions)
	{
		if (tokens.size() >= 4u && tokens[0] == "%main" && tokens[1] == "=" && tokens[2] == "OpFunction")
		{
			in_main = true;
			continue;
		}
		if (!in_main) { continue; }
		if (tokens[0] == "OpFunctionEnd") { break; }

		if (tokens.size() >= 3u && tokens[0] == "OpStore")
		{
			const auto value = values.find(tokens[2]);
			if (value != values.end()) { memory[tokens[1]] = value->second; }
			continue;
		}
		if (tokens.size() < 5u || tokens[1] != "=") { continue; }

		const auto operation = tokens[2];
		if (operation == "OpFunctionCall" && tokens[4] == helper)
		{
			// The last two parameters of each production helper are byte offset and
			// descriptor index. Evaluate the offset value actually stored for the
			// pointer passed to that helper.
			if (tokens.size() < 7u) { return false; }
			const auto offset = memory.find(tokens[tokens.size() - 2u]);
			if (offset == memory.end()) { return false; }
			*address = offset->second;
			return true;
		}

		if (operation == "OpLoad")
		{
			const auto& pointer = tokens[4];
			if (pointer == "%s24")
			{
				values[tokens[0]] = register_offset;
			} else if (const auto loaded = memory.find(pointer); loaded != memory.end())
			{
				values[tokens[0]] = loaded->second;
			}
		} else if (operation == "OpBitcast" || operation == "OpCopyObject")
		{
			if (const auto value = values.find(tokens[4]); value != values.end()) { values[tokens[0]] = value->second; }
		} else if (operation == "OpIAdd")
		{
			const auto lhs = values.find(tokens[4]);
			const auto rhs = values.find(tokens[5]);
			if (lhs != values.end() && rhs != values.end()) { values[tokens[0]] = lhs->second + rhs->second; }
		}
	}
	return false;
}

bool ValidateGeneratedModule(const String8& source, String8* error)
{
	Vector<uint32_t> binary;
	return ShaderToolchain::Run(source, &binary, error) && !binary.IsEmpty();
}

String8 GenerateWideLoad(uint32_t components, uint32_t soffset, uint32_t immediate)
{
	auto code = ParseWideLoad(components, soffset, immediate);
	if (code.GetInstructions().Size() != 2u) { return {}; }
	auto input = MakeInput();
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

} // namespace

TEST(EmulatorShaderWideBufferOffset, RegisterOffsetIncludesInstructionImmediate)
{
	InitializeShaderTest();
	for (const uint32_t components: {8u, 16u})
	{
		const auto source = GenerateWideLoad(components, kOffsetRegister, kInstructionImmediate);
		ASSERT_FALSE(source.IsEmpty()) << "wide scalar-buffer load did not generate SPIR-V";
		uint32_t address = 0;
		ASSERT_TRUE(EvaluateWideLoadAddress(source, components, kRegisterOffset, &address))
		    << "could not evaluate the byte offset passed to the generated wide-load helper";
		EXPECT_EQ(address, kRegisterOffset + kInstructionImmediate);

		String8 error;
		EXPECT_TRUE(ValidateGeneratedModule(source, &error)) << error.c_str();
	}
}

TEST(EmulatorShaderWideBufferOffset, ZeroImmediatePreservesRegisterAndNullSoffsetUsesInlineZero)
{
	InitializeShaderTest();
	for (const uint32_t components: {8u, 16u})
	{
		const auto register_source = GenerateWideLoad(components, kOffsetRegister, 0u);
		ASSERT_FALSE(register_source.IsEmpty());
		uint32_t register_address = 0;
		ASSERT_TRUE(EvaluateWideLoadAddress(register_source, components, kRegisterOffset, &register_address));
		EXPECT_EQ(register_address, kRegisterOffset);

		const auto null_source = GenerateWideLoad(components, 125u, 0u);
		ASSERT_FALSE(null_source.IsEmpty());
		const auto code = ParseWideLoad(components, 125u, 0u);
		ASSERT_EQ(code.GetInstructions().Size(), 2u);
		EXPECT_EQ(code.GetInstructions().At(0).src[1].type, ShaderOperandType::IntegerInlineConstant);
		EXPECT_EQ(code.GetInstructions().At(0).src[1].size, 0);
		uint32_t null_address = UINT32_MAX;
		ASSERT_TRUE(EvaluateWideLoadAddress(null_source, components, kRegisterOffset, &null_address));
		EXPECT_EQ(null_address, 0u);
	}
}

UT_END();
