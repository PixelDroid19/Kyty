#ifndef EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"

#include "ShaderDebugInternal.h"

#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ShaderOperandOverlapsSgprRange(const ShaderOperand& operand, int start_register, int registers_num);
[[nodiscard]] bool ShaderInstructionHasStaticBranchTarget(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionReadsImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionWritesImageResource(ShaderInstructionType type);
// Storage buffer or image write: buffer stores and buffer atomics plus the image writes above.
[[nodiscard]] bool ShaderInstructionWritesStorage(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionUsesImageSampler(ShaderInstructionType type);
[[nodiscard]] State::ImageSampleOperation ShaderInstructionSamplerOperation(ShaderInstructionType type);
class ShaderCodeResourceMemo;
// Fills bind->copied_image_descriptors for image reads whose T# or S# the
// direct lookups miss but the scalar copy flow resolves.
void ShaderCollectCopiedImageDescriptors(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base,
                                         const ShaderCodeResourceMemo* memo);

// An image read the copy flow resolves: the bound T# (before usage selection)
// and the S# whose words reach it, or -1.
struct ShaderImageCopyRead
{
	uint32_t consumer_pc        = 0;
	int      texture_descriptor = -1;
	int      sampler_index      = -1;
};

[[nodiscard]] std::vector<int32_t>             ShaderImageCopyLayout(const ShaderBindResources& bind, int user_data_register_base);
[[nodiscard]] std::vector<ShaderImageCopyRead> ShaderAnalyzeImageCopyReads(const ShaderCode& code, const ShaderBindResources& bind,
                                                                           int user_data_register_base);

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
	// Every read consumer fetches texels without a sampler, so a storage view can serve it.
	bool                        storage_readable   = false;
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

// Ascending instruction indices by class, so per-draw scans visit only the
// instructions their own checks can accept.
struct ShaderInstructionIndices
{
	std::vector<uint32_t> buffer_accesses;    // scalar buffer loads and vector buffer accesses
	std::vector<uint32_t> image_samplers;     // instructions that read an S#
	std::vector<uint32_t> image_reads;        // image reads that return dmask components
	std::vector<uint32_t> descriptor_loads;   // S_LOAD_DWORDX4 and S_LOAD_DWORDX8
	std::vector<uint32_t> gds_and_m0_writes;  // GDS data-share instructions and M0 writes
	bool                  gds_memory = false; // a GDS memory instruction exists
};

[[nodiscard]] ShaderInstructionIndices ShaderIndexInstructions(const ShaderCode& code);

// An image read whose S# SGPRs hold the same constant words on every path to
// it (an inline sampler the program assembles with scalar moves).
struct ShaderConstantSamplerRead
{
	uint32_t              instruction = 0;
	ShaderSamplerResource resource;
};

[[nodiscard]] std::vector<ShaderConstantSamplerRead> ShaderFindConstantSamplerReads(const ShaderCode& code);

// Results of the resource analyses that depend only on the program code and a
// start register or binding layout. Each memo belongs to one immutable code object and is shared
// by every draw of that program; a null memo analyzes the code directly. Entry
// values passed to DirectImageUse must come from this code's own scalar flow.
// A T# or S# that a fused GS program loads, at a constant table dword, through
// the GS user-data address it receives in s[0:1] (block: its part of the load).
struct ShaderGsUserDataDescriptorLoad
{
	ShaderInstruction     block;
	int                   offset_dw = 0;
	ShaderDynamicSLoadUse use;
};

// Part of a T# that a load writes: block is its part of the load, offset_dw
// its first table dword and field_offset its first T# dword.
struct ShaderNestedTexturePiece
{
	ShaderInstruction block;
	int               offset_dw    = 0;
	int               field_offset = 0;
};

// An image consumer whose T# scalar moves assemble, within its CFG block, from
// parts of loads of eight consecutive dwords of one table.
struct ShaderNestedCopiedTexture
{
	uint32_t                              consumer  = 0;
	int                                   offset_dw = 0;
	std::vector<ShaderNestedTexturePiece> pieces;
};

// A buffer consumer whose V# dwords come, on every path and through at least
// one scalar move, from loads of four consecutive dwords of one table.
struct ShaderNestedCopiedBuffer
{
	uint32_t consumer  = 0;
	int      offset_dw = 0;
};

// Descriptors a program loads, at constant table dwords, through a table
// pointer that a scalar load at pointer_index reads, at pointer_offset_dw, out
// of an SRT pointer table; that load is the only write of the base reaching
// each descriptor load.
struct ShaderNestedTableLoad
{
	uint32_t                                    pointer_index     = 0;
	int                                         pointer_offset_dw = 0;
	std::vector<ShaderGsUserDataDescriptorLoad> loads;
	std::vector<ShaderNestedCopiedTexture>      copied_textures;
	std::vector<ShaderNestedCopiedBuffer>       copied_buffers;
};
[[nodiscard]] std::vector<ShaderNestedTableLoad> ShaderFindNestedTableLoads(const ShaderCode& code);

// A descriptor-sized part of a pointer-table load and its consumers.
struct ShaderDescriptorBlock
{
	ShaderInstruction     load;
	int                   first_dword = 0;
	ShaderDynamicSLoadUse use;
};

struct ShaderControlFlowGraph;

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
	[[nodiscard]] const ShaderInstructionIndices&           InstructionIndices(const ShaderCode& code) const;
	[[nodiscard]] const std::vector<ShaderNestedTableLoad>& NestedTableLoads(const ShaderCode& code) const;
	[[nodiscard]] const std::vector<ShaderConstantSamplerRead>& ConstantSamplerReads(const ShaderCode& code) const;
	// Descriptor blocks of the pointer-table load at index; empty when a part
	// of it has no consumer it alone defines.
	[[nodiscard]] const std::vector<ShaderDescriptorBlock>& TableLoadBlocks(const ShaderCode& code, uint32_t index) const;
	[[nodiscard]] ShaderDynamicSLoadSpan DynamicSLoadScalarSpan(const ShaderCode& code, const ShaderDynamicSLoadMapping& mapping) const;
	// Copy-flow reads for the binding layout of `bind` (ShaderImageCopyLayout).
	[[nodiscard]] std::vector<ShaderImageCopyRead> ImageCopyReads(const ShaderCode& code, const ShaderBindResources& bind,
	                                                              int user_data_register_base) const;

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
	mutable ShaderInstructionIndices                                m_instruction_indices;
	mutable bool                                                    m_instruction_indices_ready = false;
	mutable std::vector<ShaderNestedTableLoad>                      m_nested_table_loads;
	mutable bool                                                    m_nested_table_loads_ready = false;
	mutable std::vector<ShaderConstantSamplerRead>                  m_constant_sampler_reads;
	mutable bool                                                    m_constant_sampler_reads_ready = false;
	mutable std::shared_ptr<const ShaderControlFlowGraph>           m_cfg;
	mutable std::map<uint32_t, std::vector<ShaderDescriptorBlock>>  m_table_load_blocks;
	// Keyed by load PC, last consumer PC, destination register and dword count.
	mutable std::map<std::tuple<uint32_t, uint32_t, int, int>, ShaderDynamicSLoadSpan> m_dynamic_sload_spans;
	mutable std::map<std::vector<int32_t>, std::vector<ShaderImageCopyRead>>           m_image_copy_reads;
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
                                             const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo = nullptr);
void ShaderCollectAssembledBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                             int user_sgpr_num, int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                             const ShaderCodeResourceMemo* memo);
// Binds, like assembled descriptors, each V# scalar moves copy out of a table
// that an SRT pointer table selects (ShaderNestedCopiedBuffer).
void ShaderCollectNestedCopiedBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                                uint16_t srt_size_dw, int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                                const ShaderCodeResourceMemo* memo);
void ShaderCollectPointerTableResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                        ShaderParsedUsage* info, uint16_t srt_size_dw, int user_data_register_base,
                                        const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo);
// Binds each S# the shader assembles from constants right before its image read.
void ShaderCollectConstantSamplers(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info,
                                   int user_data_register_base, const ShaderCodeResourceMemo* memo);

// Draw-independent analysis of the program code.
std::vector<ShaderGsUserDataDescriptorLoad> ShaderFindGsUserDataDescriptorLoads(const ShaderCode& code);
// Binds what the loads read from the draw's table; the loads keep reading
// guest memory and their mappings only name the bound resources.
void ShaderBindGsUserDataDescriptors(const ShaderCode& code, const std::vector<ShaderGsUserDataDescriptorLoad>& loads,
                                     uint64_t table_address, const HW::UserSgprInfo& user_sgpr, ShaderBindResources* bind,
                                     ShaderParsedUsage* info, const ShaderCodeResourceMemo* memo);
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
