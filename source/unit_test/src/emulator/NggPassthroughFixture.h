#ifndef UNIT_TEST_EMULATOR_NGG_PASSTHROUGH_FIXTURE_H_
#define UNIT_TEST_EMULATOR_NGG_PASSTHROUGH_FIXTURE_H_

// Independently authored generic RDNA2 encodings of a NGG passthrough front, shared
// by the proof and the admission suites. They are synthetic, not a captured shader.

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Kyty::Libs::Graphics::NggFixture {

using Words = std::vector<uint32_t>;

inline void Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "NGG passthrough proof fixture: %s\n", message);
		std::fflush(stderr);
		std::_Exit(7);
	}
}

inline void Initialize()
{
	// Parser configuration is process-global; every fixture runs in its own
	// death-test child, including on same-process suite repetitions.
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

inline uint64_t Mask(uint32_t count)
{
	return count == 64 ? UINT64_MAX : (uint64_t {1} << count) - 1u;
}

inline uint32_t Inline(int value)
{
	Check(value >= -16 && value <= 64, "encodable inline integer");
	return static_cast<uint32_t>(value < 0 ? 192 - value : 128 + value);
}

inline void Sop2(Words& words, uint32_t opcode, uint32_t dst, uint32_t a, uint32_t b, uint32_t literal = 0)
{
	Check(a != 255u || b != 255u, "fixture uses at most one literal");
	words.push_back(0x80000000u | (opcode << 23u) | (dst << 16u) | (b << 8u) | a);
	if (a == 255u || b == 255u) { words.push_back(literal); }
}

inline void Sop1(Words& words, uint32_t opcode, uint32_t dst, uint32_t src, uint32_t literal = 0)
{
	words.push_back(0xbe800000u | (dst << 16u) | (opcode << 8u) | src);
	if (src == 255u) { words.push_back(literal); }
}

inline void Sopp(Words& words, uint32_t opcode, uint32_t immediate = 0)
{
	words.push_back(0xbf800000u | (opcode << 16u) | immediate);
}

inline void Vop1(Words& words, uint32_t opcode, uint32_t dst, uint32_t src, uint32_t literal = 0)
{
	words.push_back(0x7e000000u | (dst << 17u) | (opcode << 9u) | src);
	if (src == 255u) { words.push_back(literal); }
}

inline void Vop2(Words& words, uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t src1)
{
	words.push_back((opcode << 25u) | (dst << 17u) | (src1 << 9u) | src0);
}

inline void Exp(Words& words, uint32_t target, uint32_t enable, bool done, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	words.push_back(0xf8000000u | (done ? 0x800u : 0u) | (target << 4u) | enable);
	words.push_back(a | (b << 8u) | (c << 16u) | (d << 24u));
}

inline Words Vopc(uint32_t opcode, uint32_t src0, uint32_t vsrc1)
{
	return {0x7c000000u | (opcode << 17u) | (vsrc1 << 9u) | src0};
}

inline Words Sopc(uint32_t opcode, uint32_t a, uint32_t b)
{
	return {0xbf000000u | (opcode << 16u) | (b << 8u) | a};
}

inline void Append(Words& words, const Words& more)
{
	words.insert(words.end(), more.begin(), more.end());
}

inline Words Fixture(uint32_t vertices = 3, uint32_t primitives = 1, bool low_word_masks = false)
{
	// Independently authored generic RDNA2 encodings, deliberately different
	// register allocation and vertex arithmetic from any captured shader. The
	// parameters are a caller's planned launch, not a host physical mapping.
	Words words;
	Sop1(words, 3, 20, 3);                          // s_mov_b32 s20, s3
	Sop2(words, 39, 22, 20, 255, (8u << 16u) | 8u); // primitive byte
	Sop2(words, 14, 21, 20, 255, 255);              // vertex byte
	Sop2(words, 30, 24, 22, Inline(12));
	Sop2(words, 16, 124, 21, 24);                   // m0 allocation payload
	Sopp(words, 32, 3);                            // three-line prefetch hint
	Sopp(words, 0, 2);                             // actual NOP, not PREFETCH mode 0
	Sopp(words, 16, 9);
	Sop2(words, 32, 26, 20, Inline(8));             // deliberately keeps unknown high bits
	Sop2(words, 3, 26, Inline(0), 26);
	if (low_word_masks)
	{
		Check(vertices <= 32 && primitives <= 32, "low-word mask fixture count");
		Sop1(words, 3, 126, 255, static_cast<uint32_t>(Mask(primitives)));
	} else
	{
		Sop2(words, 33, 126, Inline(-1), 26);
	}
	Exp(words, 20, 1, true, 0, 111, 173, 239); // disabled physical fields must be cleared by parsing
	Sop2(words, 3, 28, Inline(0), 20);
	Sopp(words, 12, 0xff0f);                  // only expcnt(0) is needed here
	if (low_word_masks)
	{
		Sop1(words, 3, 126, 255, static_cast<uint32_t>(Mask(vertices)));
	} else
	{
		Sop2(words, 33, 126, Inline(-1), 28);
	}
	Vop1(words, 1, 17, 256 + 5);
	Vop1(words, 1, 19, 256 + 8);
	Vop1(words, 1, 23, 244);                // float inline 2.0, copied bitwise
	Vop1(words, 1, 25, 255, 0x3e800000u);   // float literal 0.25, copied bitwise
	Vop2(words, 37, 17, Inline(7), 17);     // no-carry integer add
	Vop2(words, 26, 19, Inline(2), 19);
	Vop2(words, 27, 19, Inline(15), 19);
	Vop1(words, 5, 17, 256 + 17);
	Vop1(words, 6, 19, 256 + 19);
	Vop2(words, 3, 17, 240, 17);            // add float 0.5
	Vop2(words, 8, 19, 244, 19);            // multiply float 2.0
	Exp(words, 13, 4, false, 91, 101, 8, 201);
	Exp(words, 12, 15, true, 17, 19, 23, 25);
	Exp(words, 32, 15, false, 25, 23, 19, 17);
	Sopp(words, 1);
	return words;
}

inline ShaderCode Parse(const Words& words)
{
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	Check(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code), "bounded complete parse");
	Check(!code.GetInstructions().IsEmpty(), "nonempty parsed instructions");
	const auto& end = code.GetInstructions().At(code.GetInstructions().Size() - 1u);
	Check(end.type == ShaderInstructionType::SEndpgm && end.pc == (words.size() - 1u) * 4u, "real final terminator with all source words consumed");
	for (const auto& inst: code.GetInstructions())
	{
		Check(ShaderInstructionLoweringPreconditions(inst), "every parsed instruction preserves the shared full-module preconditions");
	}
	return code;
}

} // namespace Kyty::Libs::Graphics::NggFixture

#endif // UNIT_TEST_EMULATOR_NGG_PASSTHROUGH_FIXTURE_H_
