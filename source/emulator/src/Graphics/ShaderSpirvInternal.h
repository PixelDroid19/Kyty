#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVINTERNAL_H_

// Shader SPIR-V recompiler module map (edit the smallest file that owns the
// contract you are changing):
//
//   ShaderSpirv.cpp           public entry: SpirvGenerateSource / embedded
//   ShaderSpirvInternal.h     Spirv class, operand helpers, recompiler macros
//   ShaderSpirvEmitters.h     KYTY_RECOMPILER_FUNC declarations (table symbols)
//   ShaderSpirvDispatch.cpp   recompiler function table (type+format → emit)
//   ShaderSpirvGenerator.cpp  Spirv::GenerateSource pipeline + Write* skeleton
//   ShaderSpirvControlFlow.cpp SBranch / SCbranch emission
//   ShaderSpirvWriteLabel.cpp label materialization (loop-merge → sc_join → guest)
//   ShaderSpirvScJoin.h/.cpp  multi-join ownership analysis (no SPIR-V text)
//   ShaderSpirvOperands.cpp   operand load/store helpers, interpolation
//   ShaderSpirvScalar.cpp     scalar ALU recompilers
//   ShaderSpirvVector.cpp     vector / export recompilers
//   ShaderSpirvBuffer.cpp     buffer / DS / SLoad recompilers
//   ShaderSpirvImage.cpp      image sample / load / store recompilers
//   ShaderSpirvTemplates.h/.cpp embedded SPIR-V text blobs (fetch, helpers)
//
// CFG / spirv-val work for PS multi-join cascades: ScJoin* then ControlFlow.

#include "Kyty/Core/ArrayWrapper.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include <set>
#include <string>

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class Spirv;

enum class SccCheck
{
	None,
	NonZero,
	ExecNonZero,
	OverflowAdd,
	OverflowSub,
	CarryOut,
};

#define KYTY_RECOMPILER_ARGS                                                                                                               \
	[[maybe_unused]] uint32_t index, [[maybe_unused]] const ShaderCode &code, [[maybe_unused]] String8 *dst_source,                        \
	    [[maybe_unused]] Spirv *spirv, [[maybe_unused]] const char *const *param, [[maybe_unused]] SccCheck scc_check
#define KYTY_RECOMPILER_FUNC(f) bool f(KYTY_RECOMPILER_ARGS)

using inst_recompile_func_t = bool (*)(KYTY_RECOMPILER_ARGS);

enum class SpirvType
{
	Unknown,
	Float,
	Int,
	Uint
};

struct SpirvValue
{
	SpirvType type = SpirvType::Unknown;
	String8   value;
};

enum class PixelInterpolationMode
{
	Unused,
	PerspectiveCenter,
	PerspectiveCentroid,
	LinearCenter,
	LinearCentroid,
	Unsupported
};

struct PixelSystemInputField
{
	uint32_t               bit;
	uint32_t               width;
	PixelInterpolationMode mode;
};

class Spirv
{
public:
	Spirv()          = default;
	virtual ~Spirv() = default;
	KYTY_CLASS_DEFAULT_COPY(Spirv);

	[[nodiscard]] const ShaderCode& GetCode() const { return m_code; }
	ShaderCode&                     GetCode() { return m_code; }
	void                            SetCode(const ShaderCode& m_code) { this->m_code = m_code; }

	void GenerateSource();

	[[nodiscard]] const String8& GetSource() const { return m_source; }
	[[nodiscard]] bool           CanLoadPackedHalfForExport(int export_index, ShaderOperand op) const;

	void                                       SetVsInputInfo(const ShaderVertexInputInfo* input_info) { m_vs_input_info = input_info; }
	[[nodiscard]] const ShaderVertexInputInfo* GetVsInputInfo() const { return m_vs_input_info; }

	void                                        SetCsInputInfo(const ShaderComputeInputInfo* input_info) { m_cs_input_info = input_info; }
	[[nodiscard]] const ShaderComputeInputInfo* GetCsInputInfo() const { return m_cs_input_info; }

	void                                      SetPsInputInfo(const ShaderPixelInputInfo* input_info) { m_ps_input_info = input_info; }
	[[nodiscard]] const ShaderPixelInputInfo* GetPsInputInfo() const { return m_ps_input_info; }

	[[nodiscard]] const ShaderBindResources* GetBindInfo() const { return m_bind; }
	//[[nodiscard]] const ShaderBindParameters& GetBindParams() const { return m_bind_params; }

	void                  AddConstantUint(uint32_t u);
	void                  AddConstantInt(int i);
	void                  AddConstantFloat(float f);
	void                  AddConstant(ShaderOperand op);
	[[nodiscard]] String8 GetConstantUint(uint32_t u) const;
	[[nodiscard]] String8 GetConstantInt(int i) const;
	[[nodiscard]] String8 GetConstantFloat(float f) const;
	[[nodiscard]] String8 GetConstant(ShaderOperand op) const;

	void GetMappedIndex(int offset, int* buffer, int* field) const
	{
		EXIT_NOT_IMPLEMENTED(offset >= m_extended_mapping.Size());
		*buffer = m_extended_mapping[offset][0];
		*field  = m_extended_mapping[offset][1];
	}

	[[nodiscard]] bool GetDynamicSLoadMappedIndex(uint32_t instruction_pc, int offset, int* buffer, int* field) const
	{
		EXIT_IF(buffer == nullptr || field == nullptr);
		if (m_bind == nullptr || offset < 0)
		{
			return false;
		}
		const auto& dynamic_sloads = m_bind->dynamic_sloads;
		for (int mapping = 0; mapping < dynamic_sloads.mappings_num; ++mapping)
		{
			if (dynamic_sloads.instruction_pc[mapping] != instruction_pc)
			{
				continue;
			}
			const int first_dword = dynamic_sloads.offset_dw[mapping];
			if (offset < first_dword || offset >= first_dword + dynamic_sloads.dword_count[mapping])
			{
				continue;
			}

			const int resource_index = dynamic_sloads.resource_index[mapping];
			const int resource_field = offset - first_dword;
			switch (dynamic_sloads.kind[mapping])
			{
				case ShaderDynamicSLoadResourceKind::StorageBuffer:
					EXIT_NOT_IMPLEMENTED(resource_index < 0 || resource_index >= m_bind->storage_buffers.buffers_num);
					*buffer = resource_index;
					*field  = resource_field;
					return true;
				case ShaderDynamicSLoadResourceKind::Texture:
					EXIT_NOT_IMPLEMENTED(resource_index < 0 || resource_index >= m_bind->textures2D.textures_num || resource_field >= 8);
					*buffer = m_bind->storage_buffers.buffers_num + resource_index * 2 + resource_field / 4;
					*field  = resource_field % 4;
					return true;
				case ShaderDynamicSLoadResourceKind::Sampler:
					EXIT_NOT_IMPLEMENTED(resource_index < 0 || resource_index >= m_bind->samplers.samplers_num);
					*buffer = m_bind->storage_buffers.buffers_num + m_bind->textures2D.textures_num * 2 + resource_index;
					*field  = resource_field;
					return true;
			}
		}
		return false;
	}

private:
	struct Variable
	{
		ShaderOperand op;
	};

	struct Constant
	{
		SpirvType      type     = SpirvType::Unknown;
		ShaderConstant constant = {0};
		String8        type_str;
		String8        value_str;
		String8        literal_str;
		String8        id;
	};

	void AddConstant(SpirvType type, ShaderConstant constant);
	void AddVariable(ShaderOperand op);
	void AddVariable(ShaderOperandType type, int register_id, int size);

	void WriteHeader();
	void WriteDebug();
	void WriteAnnotations();
	void WriteTypes();
	void WriteConstants();
	void WriteGlobalVariables();
	void WriteMainProlog();
	void WriteLocalVariables();
	void WriteInstructions();
	void WriteMainEpilog();
	void WriteFunctions();
	void WriteLabel(int index);

	void FindConstants();
	void FindVariables();
	bool ResolvePixelInterpolationModes();
	[[nodiscard]] PixelInterpolationMode GetPixelInterpolationMode(uint32_t input) const;

	void ModifyCode();

	void DetectFetch();

	String8                       m_source;
	ShaderCode                    m_code;
	// A guest join PC shared by several instructions materializes the same
	// sc_join block once per WriteLabel call. Emitting the label a second time
	// creates a duplicate SPIR-V Id that drivers reject during module creation.
	// Emit each sc_join name once; later
	// writers keep their guest body labels, which are reachable only through
	// the already-emitted join (discard tails are dead code after the first
	// OpKill anyway).
	std::set<std::string>         m_emitted_sc_joins;
	Vector<Constant>              m_constants;
	Vector<Variable>              m_variables;
	const ShaderVertexInputInfo*  m_vs_input_info = nullptr;
	const ShaderComputeInputInfo* m_cs_input_info = nullptr;
	const ShaderPixelInputInfo*   m_ps_input_info = nullptr;
	const ShaderBindResources*    m_bind          = nullptr;
	PixelInterpolationMode        m_pixel_interpolation[32] {};
	// ShaderBindParameters          m_bind_params;

	// Extended user data is addressed relative to SGPR 16. Keep the emitted
	// mapping aligned with the shared EUD span policy rather than the old 64-dw
	// local assumption; Gen5 descriptor tables may legitimately address later
	// entries through the EUD pointer.
	Core::Array2<int, SHADER_GEN5_EUD_MAX_DWORDS, 2> m_extended_mapping {};
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct RecompilerFunc
{
	inst_recompile_func_t           func      = nullptr;
	ShaderInstructionType           type      = ShaderInstructionType::Unknown;
	ShaderInstructionFormat::Format format    = ShaderInstructionFormat::Unknown;
	const char*                     param[4]  = {nullptr, nullptr, nullptr, nullptr};
	SccCheck                        scc_check = SccCheck::None;
};

bool operand_is_constant(ShaderOperand op);
bool operand_is_variable(ShaderOperand op);
bool operand_covers_vgpr(ShaderOperand op, int reg);
bool instruction_writes_vgpr(const ShaderInstruction& inst, int reg);
bool FragmentTapSelection(const ShaderCode& code, uint32_t* pc, int* first_register);
String8 packed_half_shadow_to_str(ShaderOperand op);
SpirvValue operand_variable_to_str(ShaderOperand op);
SpirvValue operand_variable_to_str(ShaderOperand op, int shift);
SpirvValue buffer_index_variable_to_str(const ShaderInstruction& inst);
SpirvValue mimg_address_to_str(const ShaderInstruction& inst, int address);
bool operand_is_exec(ShaderOperand op);
bool operand_load_int(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load);
bool operand_load_uint(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load, int shift = -1);
bool operand_load_float(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load);
String8 get_scc_check(SccCheck scc_check, int dst_num);
bool UsesArrayed2dImages(const ShaderBindResources* bind, ShaderTextureUsage usage);
bool UsesUnsignedIntegerImages(const ShaderBindResources* bind);
bool UsesMixedSampledImageNumericTypes(const ShaderBindResources* bind);
bool UsesFormatlessStorageImages(const ShaderBindResources* bind);
bool IsImageInstruction(const ShaderInstruction& inst);
bool IsSampledImageInstruction(const ShaderInstruction& inst);
bool IsStorageImageInstruction(const ShaderInstruction& inst);
bool SupportsArrayed2dImageInstruction(const ShaderInstruction& inst);
extern const uint32_t SPIRV_DEVICE_MEMORY_ACQ_REL;
extern const uint32_t SPIRV_WORKGROUP_MEMORY_ACQ_REL;
const RecompilerFunc* RecompFunc(ShaderInstructionType type, ShaderInstructionFormat::Format format);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVINTERNAL_H_ */
