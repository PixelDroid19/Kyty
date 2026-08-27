#ifndef EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"

#include "ShaderDebugInternal.h"

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ShaderOperandOverlapsSgprRange(const ShaderOperand& operand, int start_register, int registers_num);
[[nodiscard]] bool ShaderInstructionHasStaticBranchTarget(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionReadsImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionWritesImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionUsesImageSampler(ShaderInstructionType type);
[[nodiscard]] State::ImageSampleOperation ShaderInstructionSamplerOperation(ShaderInstructionType type);

struct ShaderSamplerOperationEvidence
{
	State::ImageSampleOperation operation = State::ImageSampleOperation::Regular;
	bool                        found     = false;
};

[[nodiscard]] ShaderSamplerOperationEvidence AnalyzeShaderSamplerOperationEvidence(const ShaderCode& code, int start_register);

void ShaderGetTextureBuffer(ShaderTextureResources* info, bool* direct_sgprs, int start_index, int slot, ShaderTextureUsage usage,
                            const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer);
void ShaderGetSampler(ShaderSamplerResources* info, bool* direct_sgprs, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                      const uint32_t* extended_buffer);

// Gen5 EUD (extended user data) descriptor helpers, shared by the shader
// resource analysis and the usage parse paths.
constexpr uint16_t k_gen5_eud_direct_type = 5;
bool Gen5HasEudPointer(const ShaderUserData* user_data);
void ShaderReportMissingGen5EudPointer(const ShaderUserData* user_data, int reg, int user_sgpr_num);
bool Gen5SharpNeedsEud(int offset_dw, int dwords, int user_sgpr_num);
int Gen5EudApiIndex(int offset_dw, int user_sgpr_num);
bool Gen5SharpUseTextureDescriptor(bool size_flag, int offset_dw, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr,
                                   const uint32_t* extended_buffer);
bool Gen5CodeUnavailableDirectResourceLooksStorage(const HW::UserSgprInfo& user_sgpr, int reg);

// Dynamic scalar resource analysis, shared by the usage parse paths.
bool ShaderInstructionIsScalarBufferLoad(const ShaderInstruction& inst);
void ShaderCollectDynamicScalarResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                         ShaderParsedUsage* info, const uint32_t* extended_buffer, uint16_t eud_size_dw);
bool ShaderIsDynamicScalarStorageConsumer(const ShaderBindResources& bind, const ShaderInstruction& inst);
bool ShaderStorageResourceHasDynamicSLoad(const ShaderBindResources& bind, int storage_index);
void ShaderPruneUnusedMetadataStorage(const ShaderCode& code, ShaderStorageResources* resources, int user_sgpr_num,
                                      int user_data_register_base);

// Vertex input decode helpers, shared by the usage-parse paths.
const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code);
ShaderUsageInfo GetUsageSlots(const uint32_t* code);
void ShaderDetectBuffers(ShaderVertexInputInfo* info, bool ps5);
void ShaderParseFetch(ShaderVertexInputInfo* info, const uint32_t* fetch, const uint32_t* buffer, uint32_t user_sgpr_num);
void ShaderParseAttrib(ShaderVertexInputInfo* info, const ShaderSemantic* input_semantics, uint32_t num_input_semantics,
                       const uint32_t* attrib, const uint32_t* buffer);
bool ShaderGetStorageBuffer(ShaderStorageResources* info, bool* direct_sgprs, int start_index, int slot, ShaderStorageUsage usage,
                            const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer,
                            ShaderStorageBindingSource source = ShaderStorageBindingSource::DirectResource);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_ */
