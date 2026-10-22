#ifndef EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"

#include "ShaderDebugInternal.h"

#include <bitset>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ShaderOperandOverlapsSgprRange(const ShaderOperand& operand, int start_register, int registers_num);
[[nodiscard]] bool ShaderInstructionHasStaticBranchTarget(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionReadsImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionWritesImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionUsesImageSampler(ShaderInstructionType type);
[[nodiscard]] State::ImageSampleOperation ShaderInstructionSamplerOperation(ShaderInstructionType type);
// Fills bind->copied_image_descriptors for image reads whose T# or S# the
// direct lookups miss but the scalar copy flow resolves.
void ShaderCollectCopiedImageDescriptors(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base);

struct ShaderSamplerOperationEvidence
{
	State::ImageSampleOperation operation = State::ImageSampleOperation::Regular;
	bool                        found     = false;
};

[[nodiscard]] ShaderSamplerOperationEvidence AnalyzeShaderSamplerOperationEvidence(const ShaderCode& code, int start_register);

struct ShaderDynamicSLoadUse
{
	ShaderDynamicSLoadResourceKind kind              = ShaderDynamicSLoadResourceKind::StorageBuffer;
	ShaderStorageUsage             storage_usage     = ShaderStorageUsage::ReadOnly;
	ShaderTextureUsage             texture_usage     = ShaderTextureUsage::Unknown;
	State::ImageSampleOperation sampler_operation = State::ImageSampleOperation::Regular;
	ShaderGen5SampledTextureShape sampled_shape = ShaderGen5SampledTextureShape::TwoDimensional;
	uint32_t                    last_consumer_pc   = 0;
	bool                        raw_vmem_oob_guarded = false;
	bool                        sampled_shape_known = false;
	bool                        found              = false;
	bool                        valid              = true;
};

// Scalar bytes read through one dynamic S_LOAD mapping, as computed by
// ShaderDynamicSLoadScalarSpan from a zero byte count.
struct ShaderDynamicSLoadSpan
{
	uint64_t required_bytes = 0;
	bool     exact          = false;
};

// A whole-descriptor S_LOAD at a constant non-negative dword offset.
struct ShaderDescriptorSLoadCandidate
{
	uint32_t index     = 0;
	int      dwords    = 0;
	int      offset_dw = 0;
};

// An image consumer whose 8-dword T# is written, within its block, by two
// 4-dword S_LOADs: low at the T# register and high four registers above it.
struct ShaderSplitTextureCandidate
{
	uint32_t consumer = 0;
	uint32_t low      = 0;
	uint32_t high     = 0;
};

// Code-only instruction candidates of the EUD descriptor scan, in ascending
// instruction order. Per-draw checks (pointer register, entry values, EUD
// span) still apply to every entry.
struct ShaderEudDescriptorLoads
{
	std::vector<ShaderSplitTextureCandidate>    split_textures;
	std::vector<ShaderDescriptorSLoadCandidate> sloads;
};

[[nodiscard]] ShaderEudDescriptorLoads ShaderFindEudDescriptorLoads(const ShaderCode& code);

// Results of the resource analyses that depend only on the program code and one
// start register. Each memo belongs to one immutable code object and is shared
// by every draw of that program; a null memo analyzes the code directly. Entry
// values passed to DirectImageUse must come from this code's own scalar flow.
class ShaderCodeResourceMemo
{
public:
	[[nodiscard]] ShaderStorageUseEvidence       StorageUse(const ShaderCode& code, int start_register) const;
	[[nodiscard]] ShaderDirectImageUse           DirectImageUse(const ShaderCode& code, int start_register,
	                                                            const std::vector<std::bitset<106>>* entry_values) const;
	[[nodiscard]] ShaderSamplerOperationEvidence SamplerOperation(const ShaderCode& code, int start_register) const;
	// Consumers of every whole-descriptor S_LOAD up to the first branch, indexed
	// by instruction; other entries keep the default value.
	[[nodiscard]] const std::vector<ShaderDynamicSLoadUse>& DescriptorSLoadUses(const ShaderCode& code) const;
	[[nodiscard]] const ShaderEudDescriptorLoads&           EudDescriptorLoads(const ShaderCode& code) const;
	[[nodiscard]] ShaderDynamicSLoadSpan DynamicSLoadScalarSpan(const ShaderCode& code, const ShaderDynamicSLoadMapping& mapping) const;

private:
	mutable std::mutex                                              m_mutex;
	mutable std::unordered_map<int, ShaderStorageUseEvidence>       m_storage_use;
	mutable std::unordered_map<int, ShaderDirectImageUse>           m_direct_image_use;
	mutable std::unordered_map<int, ShaderDirectImageUse>           m_entry_image_use;
	mutable std::unordered_map<int, ShaderSamplerOperationEvidence> m_sampler_operation;
	mutable std::vector<ShaderDynamicSLoadUse>                      m_descriptor_sload_uses;
	mutable bool                                                    m_descriptor_sload_uses_ready = false;
	mutable ShaderEudDescriptorLoads                                m_eud_descriptor_loads;
	mutable bool                                                    m_eud_descriptor_loads_ready = false;
	// Keyed by load PC, last consumer PC, destination register and dword count.
	mutable std::map<std::tuple<uint32_t, uint32_t, int, int>, ShaderDynamicSLoadSpan> m_dynamic_sload_spans;
};

[[nodiscard]] ShaderStorageUseEvidence       AnalyzeShaderStorageUse(const ShaderCode& code, int start_register,
                                                                     const ShaderCodeResourceMemo* memo);
[[nodiscard]] ShaderDirectImageUse           AnalyzeShaderDirectImageUse(const ShaderCode& code, int start_register,
                                                                         const std::vector<std::bitset<106>>* entry_values,
                                                                         const ShaderCodeResourceMemo* memo);
[[nodiscard]] ShaderSamplerOperationEvidence AnalyzeShaderSamplerOperationEvidence(const ShaderCode& code, int start_register,
                                                                                   const ShaderCodeResourceMemo* memo);
[[nodiscard]] ShaderDynamicSLoadSpan AnalyzeShaderDynamicSLoadSpan(const ShaderCode& code, const ShaderDynamicSLoadMapping& mapping,
                                                                  const ShaderCodeResourceMemo* memo);

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
// Bound storage buffers use API user-data indices; the fused vertex front emits them at s8.
bool ShaderScalarBufferUsesRuntimeDescriptor(const ShaderBindResources& bind, const ShaderInstruction& inst,
                                             int user_data_register_base = 0);
void ShaderAccumulateScalarBufferLoadSpan(const ShaderInstruction& inst, uint64_t* required_bytes, bool* dynamic_offset);
void ShaderCollectDynamicScalarResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                         ShaderParsedUsage* info, const uint32_t* extended_buffer, uint16_t eud_size_dw,
                                         int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                         const ShaderCodeResourceMemo* memo);
// Records the buffer instructions whose bound V# register was redefined on
// every path before them (bind->redefined_descriptor_consumers).
void ShaderCollectRedefinedBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base,
                                             const ShaderScalarFlow& scalar_flow);
void ShaderCollectAssembledBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                             int user_sgpr_num, int user_data_register_base, const ShaderScalarFlow& scalar_flow);
void ShaderCollectPointerTableResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                        ShaderParsedUsage* info, uint16_t srt_size_dw, int user_data_register_base,
                                        const ShaderScalarFlow& scalar_flow);

// A T# or S# that a fused GS program loads, at a constant table dword, through
// the GS user-data address it receives in s[0:1] (block: its part of the load).
struct ShaderGsUserDataDescriptorLoad
{
	ShaderInstruction     block;
	int                   offset_dw = 0;
	ShaderDynamicSLoadUse use;
};
// Draw-independent analysis of the program code.
std::vector<ShaderGsUserDataDescriptorLoad> ShaderFindGsUserDataDescriptorLoads(const ShaderCode& code);
// Binds what the loads read from the draw's table; the loads keep reading
// guest memory and their mappings only name the bound resources.
void ShaderBindGsUserDataDescriptors(const ShaderCode& code, const std::vector<ShaderGsUserDataDescriptorLoad>& loads,
                                     uint64_t table_address, const HW::UserSgprInfo& user_sgpr, ShaderBindResources* bind,
                                     ShaderParsedUsage* info);
// Source of guest descriptor-table words for ShaderSnapshotGuestDescriptorTable.
// Without an active scope the production read is Core::VirtualMemory::CopyFromGuest.
class ShaderGuestDescriptorReader
{
public:
	ShaderGuestDescriptorReader()                                              = default;
	ShaderGuestDescriptorReader(const ShaderGuestDescriptorReader&)            = delete;
	ShaderGuestDescriptorReader& operator=(const ShaderGuestDescriptorReader&) = delete;
	virtual ~ShaderGuestDescriptorReader()                                     = default;

	// Fills exactly `dwords` words; false means the read was refused.
	[[nodiscard]] virtual bool Read(uint64_t guest_address, uint32_t dwords, uint32_t* words) = 0;
};

// Installs a reader for the current thread only and restores the previous one on destruction.
class ScopedShaderGuestDescriptorReader
{
public:
	explicit ScopedShaderGuestDescriptorReader(ShaderGuestDescriptorReader* reader);
	ScopedShaderGuestDescriptorReader(const ScopedShaderGuestDescriptorReader&)            = delete;
	ScopedShaderGuestDescriptorReader& operator=(const ScopedShaderGuestDescriptorReader&) = delete;
	~ScopedShaderGuestDescriptorReader();

private:
	ShaderGuestDescriptorReader* m_previous;
};

// Copies a descriptor table from guest memory, retrying until two reads agree.
bool ShaderSnapshotGuestDescriptorTable(uint64_t guest_address, uint32_t dwords, std::vector<uint32_t>* snapshot);
bool ShaderIsDynamicScalarStorageConsumer(const ShaderBindResources& bind, const ShaderInstruction& inst);
bool ShaderStorageResourceHasDynamicSLoad(const ShaderBindResources& bind, int storage_index);
void ShaderPruneUnusedMetadataStorage(const ShaderCode& code, ShaderStorageResources* resources, int user_sgpr_num,
                                      int user_data_register_base, const ShaderCodeResourceMemo* memo);

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
