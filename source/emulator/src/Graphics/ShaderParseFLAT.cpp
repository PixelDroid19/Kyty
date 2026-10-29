#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// RDNA2 FLAT encoding SADDR values that disable the scalar base (NULL and the
// legacy "off" value); the VGPR address is then a full 64-bit pointer.
constexpr uint32_t kFlatSaddrNull = 0x7du;
constexpr uint32_t kFlatSaddrOff  = 0x7fu;

constexpr uint32_t kFlatSegmentFlat     = 0u;
constexpr uint32_t kFlatSegmentGlobal   = 2u;
constexpr uint32_t kFlatSegmentReserved = 3u;

// GLOBAL_LOAD_SHORT_D16_HI is refused: at an odd address its result depends on
// SH_MEM_CONFIG.alignment_mode (RDNA2 Shader ISA, section 8.1.7), and the ISA does
// not define that result.
constexpr uint32_t kGlobalLoadShortD16HiOpcode = 37u;

struct GlobalLoadOpcode
{
	uint32_t              opcode;
	ShaderInstructionType type;
	int                   dwords;
};

// The D16_HI byte loads write only D0[31:16]; the destination VGPR keeps D0[15:0].
constexpr GlobalLoadOpcode kGlobalLoads[] = {
    {12u, ShaderInstructionType::GlobalLoadDword, 1},
    {13u, ShaderInstructionType::GlobalLoadDwordx2, 2},
    {14u, ShaderInstructionType::GlobalLoadDwordx4, 4},
    {15u, ShaderInstructionType::GlobalLoadDwordx3, 3},
    {33u, ShaderInstructionType::GlobalLoadUbyteD16Hi, 1},
    {35u, ShaderInstructionType::GlobalLoadSbyteD16Hi, 1},
};

const GlobalLoadOpcode* find_global_load(uint32_t opcode)
{
	for (const auto& load: kGlobalLoads)
	{
		if (load.opcode == opcode)
		{
			return &load;
		}
	}
	return nullptr;
}

} // namespace

// FLAT family (RDNA2 table "FLAT, SCRATCH and GLOBAL"). Only GLOBAL loads
// without LDS are modelled: dword loads and the D16_HI byte loads. FLAT and
// SCRATCH segments need aperture translation and are rejected as not
// implemented. GLC/SLC/DLC are cache policy hints with no effect on the loaded
// value.
KYTY_SHADER_PARSER(shader_parse_flat)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("flat");

	const uint32_t opcode  = (buffer[0] >> 18u) & 0x7fu;
	const uint32_t offset  = buffer[0] & 0xfffu;
	const uint32_t lds     = (buffer[0] >> 13u) & 0x1u;
	const uint32_t segment = (buffer[0] >> 14u) & 0x3u;
	const uint32_t vaddr   = buffer[1] & 0xffu;
	const uint32_t saddr   = (buffer[1] >> 16u) & 0x7fu;
	const uint32_t vdst    = (buffer[1] >> 24u) & 0xffu;

	// LDS is zero for every FLAT access, and segment 3 is reserved (RDNA2 Shader ISA, Table 51).
	if (!next_gen || segment == kFlatSegmentReserved || (lds != 0u && segment == kFlatSegmentFlat))
	{
		KYTY_UNKNOWN_OP();
	}
	if (segment != kFlatSegmentGlobal)
	{
		KYTY_NI("FLAT or SCRATCH segment");
	}
	if (lds != 0u)
	{
		KYTY_NI("GLOBAL load into LDS");
	}
	if (opcode == kGlobalLoadShortD16HiOpcode)
	{
		KYTY_NI("GLOBAL_LOAD_SHORT_D16_HI (alignment at odd addresses is not defined)");
	}

	const auto* load = find_global_load(opcode);
	if (load == nullptr)
	{
		KYTY_UNKNOWN_OP();
	}

	ShaderInstruction inst;
	inst.pc       = pc;
	inst.type     = load->type;
	inst.raw_word = buffer[0];
	inst.dst      = operand_parse(vdst + 256);
	inst.dst.size = load->dwords;
	inst.src[0]   = operand_parse(vaddr + 256);
	// The 12-bit immediate is signed for the GLOBAL segment.
	inst.flat_offset = static_cast<int16_t>(static_cast<int32_t>(offset << 20u) >> 20);

	if (saddr == kFlatSaddrNull || saddr == kFlatSaddrOff)
	{
		inst.src[0].size = 2;
		inst.src_num     = 1;
		inst.format      = ShaderInstructionFormat::VdataVaddr2Off;
	} else
	{
		inst.src[0].size = 1;
		inst.src[1]      = operand_parse(saddr);
		inst.src[1].size = 2;
		inst.src_num     = 2;
		inst.format      = ShaderInstructionFormat::VdataVaddrSaddr2;
	}

	dst->GetInstructions().Add(inst);

	return 2;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
