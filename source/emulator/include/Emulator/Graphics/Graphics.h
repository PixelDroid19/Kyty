#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICS_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/EventQueue.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct Shader;
struct ShaderRegister;
struct ShaderSemantic;

// Gen5 sce::Agc size/alignment result for fused shader scratch.
struct SizeAlign
{
	uint64_t m_size  = 0;
	size_t   m_align = 0;
};

class GraphicsSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<GraphicsSubsystem>::Instance(); }
	const char*       Id() override { return "Graphics"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, uint32_t* cmd_buffer);

namespace Gen4 {

int KYTY_SYSV_ABI      GraphicsSetVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier);
int KYTY_SYSV_ABI      GraphicsUpdateVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier);
int KYTY_SYSV_ABI      GraphicsSetPsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs);
int KYTY_SYSV_ABI      GraphicsSetPsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs);
int KYTY_SYSV_ABI      GraphicsUpdatePsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs);
int KYTY_SYSV_ABI      GraphicsUpdatePsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs);
int KYTY_SYSV_ABI      GraphicsSetCsShaderWithModifier(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs, uint32_t shader_modifier);
int KYTY_SYSV_ABI      GraphicsSetEmbeddedVsShader(uint32_t* cmd, uint64_t size, uint32_t id, uint32_t shader_modifier);
int KYTY_SYSV_ABI      GraphicsDrawIndex(uint32_t* cmd, uint64_t size, uint32_t index_count, const void* index_addr, uint32_t flags,
                                         uint32_t type);
int KYTY_SYSV_ABI      GraphicsDrawIndexAuto(uint32_t* cmd, uint64_t size, uint32_t index_count, uint32_t flags);
int KYTY_SYSV_ABI      GraphicsDrawIndexOffset(uint32_t* cmd, uint64_t size, uint32_t index_offset, uint32_t index_count,
                                               uint32_t flags);
int KYTY_SYSV_ABI      GraphicsSubmitCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                                    void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes);
int KYTY_SYSV_ABI      GraphicsSubmitAndFlipCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                                           void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes, int handle, int index,
                                                           int flip_mode, int64_t flip_arg);
int KYTY_SYSV_ABI      GraphicsSubmitDone();
int KYTY_SYSV_ABI      GraphicsAreSubmitsAllowed();
void KYTY_SYSV_ABI     GraphicsFlushMemory();
int KYTY_SYSV_ABI      GraphicsAddEqEvent(::Kyty::Kernel::EventQueue::KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI      GraphicsDeleteEqEvent(::Kyty::Kernel::EventQueue::KernelEqueue eq, int id);
uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState(uint32_t* cmd, uint64_t size);
uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState175(uint32_t* cmd, uint64_t size);
uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState200(uint32_t* cmd, uint64_t size);
uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState350(uint32_t* cmd, uint64_t size);
uint32_t KYTY_SYSV_ABI GraphicsDispatchInitDefaultHardwareState(uint32_t* cmd, uint64_t size);
int KYTY_SYSV_ABI      GraphicsInsertWaitFlipDone(uint32_t* cmd, uint64_t size, uint32_t video_out_handle, uint32_t display_buffer_index);
int KYTY_SYSV_ABI      GraphicsDispatchDirect(uint32_t* cmd, uint64_t size, uint32_t thread_group_x, uint32_t thread_group_y,
                                              uint32_t thread_group_z, uint32_t mode);
uint32_t KYTY_SYSV_ABI GraphicsMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                               uint32_t* read_ptr_addr);
int KYTY_SYSV_ABI      GraphicsComputeWaitOnAddress(uint32_t* cmd, uint64_t size, uint32_t* gpu_addr, uint32_t mask, uint32_t func,
                                                    uint32_t ref);
void KYTY_SYSV_ABI     GraphicsDingDong(uint32_t ring_id, uint32_t offset_dw);
void KYTY_SYSV_ABI     GraphicsUnmapComputeQueue(uint32_t id);
int KYTY_SYSV_ABI      GraphicsInsertPushMarker(uint32_t* cmd, uint64_t size, const char* str);
int KYTY_SYSV_ABI      GraphicsInsertPopMarker(uint32_t* cmd, uint64_t size);
uint64_t KYTY_SYSV_ABI GraphicsGetGpuCoreClockFrequency();
int KYTY_SYSV_ABI      GraphicsIsUserPaEnabled();
void* KYTY_SYSV_ABI    GraphicsGetTheTessellationFactorRingBufferBaseAddress();

int KYTY_SYSV_ABI GraphicsRegisterOwner(uint32_t* owner_handle, const char* name);
int KYTY_SYSV_ABI GraphicsRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size,
                                           const char* name, uint32_t type, uint64_t user_data);
int KYTY_SYSV_ABI GraphicsUnregisterAllResourcesForOwner(uint32_t owner_handle);
int KYTY_SYSV_ABI GraphicsUnregisterOwnerAndResources(uint32_t owner_handle);
int KYTY_SYSV_ABI GraphicsUnregisterResource(uint32_t resource_handle);

} // namespace Gen4

namespace Gen5 {

struct CommandBuffer;
struct Label;

// Guest AGC state published by GraphicsInit: [0]=requested API version,
// [1]=feature flags (0 = no optional feature bits). Leaving both unset lets
// titles take a null-deref path after CreateShader / register-default use.
[[nodiscard]] inline bool GraphicsInitWriteGuestState(uint32_t* state, uint32_t ver)
{
	if (state == nullptr)
	{
		return false;
	}
	state[0] = ver;
	state[1] = 0u;
	return true;
}

int KYTY_SYSV_ABI   GraphicsInit(uint32_t* state, uint32_t ver);
// NID dbOlWdppb4o has multiple SDK-visible argument shapes. Dispatch the call
// by validated argument layout, then execute the matching Graphics5 contract.
int KYTY_SYSV_ABI   GraphicsBuildDescriptorTableOrInterpolantMapping(uint64_t output, uint64_t descriptor, uint64_t source,
                                                                     uint64_t arg3, uint64_t arg4, uint64_t arg5);
// Build the fixed 32-entry descriptor table consumed by the Graphics5 resource
// setup path.
int KYTY_SYSV_ABI   GraphicsBuildDescriptorTable(uint64_t output, uint64_t descriptor, uint64_t source, uint64_t arg3,
                                                 uint64_t arg4, uint64_t arg5);
// k0E7vkgqAuE: write one packed state entry per guest descriptor, matching it
// against the shader output-semantic masks.
uint64_t KYTY_SYSV_ABI GraphicsAgcBuildDescriptorState(uint64_t output, uint64_t descriptor, uint64_t source);
// sceAgcGetGsOversubscription (NKIzURsgV7I): fills a Uc + a Sh register
// descriptor that control geometry-shader oversubscription.
int KYTY_SYSV_ABI GraphicsAgcGetGsOversubscription(ShaderRegister* result, const Shader* gs, int32_t work_items,
                                                   float scaling);
void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2(uint32_t ver);
void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2Internal(uint32_t ver);
int KYTY_SYSV_ABI   GraphicsCreateShader(Shader** dst, void* header, const volatile void* code);
// Fuse GS/HS front+back halves (scratch holds copied SH registers for the result).
int KYTY_SYSV_ABI   GraphicsUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front, const Shader* back);
int KYTY_SYSV_ABI   GraphicsUnknownFuseShaderHalves(Shader* fused_result, const Shader* front, const Shader* back,
                                                    void* scratch_mem);
int KYTY_SYSV_ABI   GraphicsSetCxRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetShRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetUcRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetCxRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs, const Shader* hs, const Shader* gs,
                                            uint32_t prim_type);
int KYTY_SYSV_ABI   GraphicsCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs, const Shader* ps);
bool GraphicsBuildInterpolantMapping(ShaderRegister* regs, const ShaderSemantic* outputs, uint32_t output_count,
                                     const ShaderSemantic* inputs, uint32_t input_count);
int KYTY_SYSV_ABI   GraphicsGetDataPacketPayloadAddress(uint32_t** addr, uint32_t* cmd, int type);
int KYTY_SYSV_ABI   GraphicsSuspendPoint();
// Patches the address field of a Gen5 ReleaseMem end-of-pipe packet.
int KYTY_SYSV_ABI   GraphicsAgcQueueEndOfPipeActionPatchAddress(uint32_t* cmd, uint64_t address);
// sceAgcCbNop (NID LtTouSCZjHM): reserve num_dw and encode a type-3 NOP (R_ZERO).
// num_dw is the full PM4 length (header included); must be in [2, 0x4001].
uint32_t* KYTY_SYSV_ABI GraphicsCbNop(CommandBuffer* buf, uint32_t num_dw);
// Legacy name used by older call sites; same entry as GraphicsCbNop.
uint32_t* KYTY_SYSV_ABI GraphicsCbAllocateDwords(CommandBuffer* buf, uint32_t num_dw);
// sceAgcDcbSetIndexSize_0600 (NID -KRzWekV120): writes VGT_INDEX_TYPE through
// SET_UCONFIG_REG_INDEX. index_size: 0 = 16-bit, 1 = 32-bit, 2 = 8-bit.
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexSize0600(CommandBuffer* buf, uint8_t index_size, uint8_t cache_policy,
                                                    uint8_t per_instance_object_id);
// Decodes the indexed UCONFIG form that writes VGT_INDEX_TYPE. available_dwords
// includes the selector and value, and may include later PM4 packets.
bool GraphicsDecodeIndexedUconfigVgtIndexType(uint32_t header, const uint32_t* body, uint32_t available_dwords,
                                              uint32_t* index_type);
// Byte size of a CbNop of the given dword length (header included).
uint32_t KYTY_SYSV_ABI GraphicsCbNopGetSize(uint32_t size_in_dwords);
// Byte size of GraphicsCbDispatch (fixed 5-dword packet).
uint32_t KYTY_SYSV_ABI GraphicsCbDispatchGetSize();
// Byte size of GraphicsCbSetShRegisterRangeDirect for num_values registers.
uint32_t KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirectGetSize(uint32_t num_values);
// sceAgcGetPacketSize (NID Lkf86B98qPc): type-3 PM4 length in dwords from header.
uint32_t KYTY_SYSV_ABI GraphicsGetDataPacketSizeDw(const uint32_t* cmd);
// sceAgcDmaDataPatchSetDstAddressOrOffset (NID IxYiarKlXxM): patch R_DMA_DATA dst.
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd, uint64_t destination_address);
// sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate (NID cdDRpqcFGbU).
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(uint32_t* cmd, uint64_t source_value);
// sceAgcWaitRegMemPatchAddress (NID 3KDcnM3lrcU): patch wait-mem address field.
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchAddress(uint32_t* cmd, uint64_t address);
// sceAgcWaitRegMemPatchCompareFunction (NID n485EBnIWmk).
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchCompareFunction(uint32_t* cmd, uint32_t compare_function);
// sceAgcWaitRegMemPatchReference (NID 7nOoijNPvEU).
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchReference(uint32_t* cmd, uint64_t reference);
// sceAgcWaitRegMemPatchMask (NID hXAnLgDHCoI).
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchMask(uint32_t* cmd, uint64_t mask);
// sceAgcQueueEndOfPipeActionPatchGcrCntl (NID J8YCgfKAMQs).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchGcrCntl(uint32_t* cmd, uint32_t gcr_cntl);
// sceAgcQueueEndOfPipeActionPatchData (NID MlEw1feXcjg).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchData(uint32_t* cmd, uint64_t data);
// sceAgcQueueEndOfPipeActionPatchType (NID T9fjQIINoeE).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchType(uint32_t* cmd, uint32_t data_selection);
// sceAgcWriteDataPatchSetCachePolicy (NID eAy8eGNsCuU).
int KYTY_SYSV_ABI GraphicsWriteDataPatchSetCachePolicy(uint32_t* cmd, uintptr_t arg1);
// sceAgcWriteDataPatchSetDst (NID tmy-+rBpspY).
int KYTY_SYSV_ABI GraphicsWriteDataPatchSetDst(uint32_t* cmd, uintptr_t arg1);
// Patch IT_WRITE_DATA destination address dwords (NID fPSCdQxgpSw).
int KYTY_SYSV_ABI GraphicsWriteDataPatchSetAddressOrOffset(uint32_t* cmd, uint64_t address_or_offset);
// sceAgcDcbStallCommandBufferParserGetSize (NID +u6dKSLWM2o): fixed 2-dword packet.
uint32_t KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParserGetSize();
// sceAgcDcbWaitOnAddressGetSize (43WJ08sSugE) / sceAgcAcbWaitOnAddressGetSize (idlaArvdXEs).
uint32_t KYTY_SYSV_ABI GraphicsCbWaitOnAddressGetSize(uint32_t label_size);
// sceAgcDcbDmaDataGetSize (NID 2ccJz9LQI+w): fixed 7-dword packet.
uint32_t KYTY_SYSV_ABI GraphicsDcbDmaDataGetSize();
// AGC GetSize helpers (bytes). Values come from the shipped libSceAgc bodies.
// Where Kyty's writer reserves a larger packet, the larger value wins so the
// guest buffer cannot be under-allocated.
uint32_t KYTY_SYSV_ABI GraphicsAcbAcquireMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbCopyDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbDispatchIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbDmaDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbEventWriteGetSize();
uint32_t KYTY_SYSV_ABI GraphicsCbSetShRegistersDirectGetSize(uint32_t num_regs);
uint32_t KYTY_SYSV_ABI GraphicsDcbAcquireMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbCopyDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDispatchIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexOffsetGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbGetLodStatsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexBufferGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexCountGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexSizeGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetNumInstancesGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetUcRegisterDirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbWriteDataGetSize(uint32_t num_dwords);
// sceAgcDcbCondExecGetSize / sceAgcAcbCondExecGetSize: fixed 5-dword packet.
uint32_t KYTY_SYSV_ABI GraphicsDcbCondExecGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbCondExecGetSize();
// Remaining AGC GetSize helpers (bytes) from the Astro 01.018 bodies.
uint32_t KYTY_SYSV_ABI GraphicsAcbAtomicGdsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbAtomicMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbPrimeUtcl2GetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbQueueEndOfShaderActionGetSize();
uint32_t KYTY_SYSV_ABI GraphicsCbCondWriteGetSize();
uint32_t KYTY_SYSV_ABI GraphicsCbQueueEndOfPipeActionGetSize();
uint32_t KYTY_SYSV_ABI GraphicsCbSetUcRegisterRangeDirectGetSize(uint32_t num_registers);
uint32_t KYTY_SYSV_ABI GraphicsCbSetUcRegistersDirectGetSize(uint32_t num_registers);
uint32_t KYTY_SYSV_ABI GraphicsDcbAtomicGdsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbAtomicMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbBeginOcclusionQueryGetSize(uint32_t mode);
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectMultiGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexMultiInstancedGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndirectMultiGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbEndOcclusionQueryGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbPrimeUtcl2GetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbQueueEndOfShaderActionGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetBaseDispatchIndirectArgsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetBaseDrawIndirectArgsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetBoolPredicationEnableGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexIndirectArgsGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetPredicationDisableGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetZPassPredicationEnableGetSize();
// AGC patch/helper family from the Astro 01.018 libSceAgc bodies. Error
// 0x8a6c000c means the packet opcode is not the one the patch expects.
int KYTY_SYSV_ABI GraphicsSetNop(uint32_t* cmd);
int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI GraphicsAgcAsyncWriteDataPatchSetAddressOrOffset(uint32_t* cmd, uint64_t address_or_offset);
int KYTY_SYSV_ABI GraphicsAgcAsyncWriteDataPatchSetCachePolicy(uint32_t* cmd, uint32_t cache_policy);
int KYTY_SYSV_ABI GraphicsAgcAsyncWriteDataPatchSetDst(uint32_t* cmd, uint32_t dst);
// sceAgcRewindPatchSetRewindState / sceAgcAsyncRewindPatchSetRewindState.
int KYTY_SYSV_ABI GraphicsAgcRewindPatchSetRewindState(uint32_t* cmd, uint32_t state);
// sceAgcCondExecPatchSetCommandAddress / ...Async... and ...PatchSetEnd. `end`
// is the first dword past the conditional block.
int KYTY_SYSV_ABI GraphicsAgcCondExecPatchSetCommandAddress(uint32_t* cmd, uint64_t address);
int KYTY_SYSV_ABI GraphicsAgcCondExecPatchSetEnd(uint32_t* cmd, const uint32_t* end);
// sceAgcBranchPatchSetCompareAddress (NID GXBlM-ekzrI).
int KYTY_SYSV_ABI GraphicsAgcBranchPatchSetCompareAddress(uint32_t* cmd, uint64_t address);
// sceAgcAcquireMemSetEngine (NID W0WEyog0f74).
int KYTY_SYSV_ABI GraphicsAgcAcquireMemSetEngine(uint32_t* cmd, uint32_t engine);
// sceAgcGetDataPacketPayloadRange (NID s+VGAMDQ0AQ).
struct GraphicsAgcPayloadRange
{
	const volatile void* payload;
	uint64_t             size;
};
int KYTY_SYSV_ABI GraphicsGetDataPacketPayloadRange(GraphicsAgcPayloadRange* range, const uint32_t* cmd, uint32_t mode);
// sceAgcSetRangePredication (NID n8vgpaQg6dA).
int KYTY_SYSV_ABI GraphicsSetPacketPredicationRange(uint32_t* begin, uint32_t* end, uint32_t predication);
// sceAgcUpdatePrimState (NID Y3ymLfZ1384): patches VGT_GS_OUT_PRIM_TYPE and
// VGT_PRIMITIVE_TYPE inside the ShaderRegister arrays returned by CreatePrimState.
int KYTY_SYSV_ABI GraphicsUpdatePrimState(volatile uint32_t* cx_regs, volatile uint32_t* uc_regs, uint32_t prim_type);
// sceAgcDcbCondExec / sceAgcAcbCondExec (NIDs BIPexNBSGog, qyM2bxYFPAk).
uint32_t* KYTY_SYSV_ABI GraphicsDcbCondExec(CommandBuffer* buf, uint64_t address, uint32_t block_dwords);
uint32_t* KYTY_SYSV_ABI GraphicsAcbCondExec(CommandBuffer* buf, uint64_t address, uint32_t block_dwords);
// PS5 Pro / Trinity GPU mode query (NID BfBDZGbti7A). Base Prospero reports 0.
uint64_t KYTY_SYSV_ABI GraphicsGetIsTrinityMode();
// Debug exception raise used by titles as a soft breakpoint; returns OK.
int KYTY_SYSV_ABI GraphicsDebugRaiseException(uint32_t exception_id);
// sceAgcDriverRegisterWorkloadStream (NID 3AyTaWcF-H8): register a Gen5 workload stream descriptor.
int KYTY_SYSV_ABI GraphicsDriverRegisterWorkloadStream(uint32_t stream_id, const void* stream);
// sceAgcDriverUnregisterWorkloadStream (NID n5ElQVYsU1A).
int KYTY_SYSV_ABI GraphicsDriverUnregisterWorkloadStream(uint32_t stream_id);

uint32_t GraphicsGetShRegistersPacketSize(const ShaderRegister* regs, uint32_t num_regs);
uint32_t GraphicsEncodeShRegisters(uint32_t* cmd, uint32_t capacity_dw, const ShaderRegister* regs, uint32_t num_regs);
uint32_t GraphicsEncodeDispatch(uint32_t* cmd, uint32_t capacity_dw, uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                uint32_t modifier);

// Post-Play loading: WaitRegMem is often encoded with address=0 while the
// immediately preceding ReleaseMem receives the real Label* via EopPatch.
// If wait body address is null and the previous packet is R_RELEASE_MEM with a
// non-null address, return that address; otherwise nullptr.
uint64_t* GraphicsResolveWaitMemAddressFromPrecedingRelease(const uint32_t* wait_body, const uint32_t* stream_begin,
                                                            const uint32_t* stream_end);

uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirect(CommandBuffer* buf, uint32_t offset, const uint32_t* values,
                                                           uint32_t num_values);
uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegistersDirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs);
// sceAgcCbSetUcRegisterRangeDirect (NID MDLD5Ly94Xk) and
// sceAgcCbSetUcRegistersDirect (NID 03RZmELWWzw): SET_UCONFIG_REG packets.
uint32_t* KYTY_SYSV_ABI GraphicsCbSetUcRegisterRangeDirect(CommandBuffer* buf, uint32_t offset, const uint32_t* values,
                                                           uint32_t num_values);
uint32_t* KYTY_SYSV_ABI GraphicsCbSetUcRegistersDirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs);
// sceAgcDcbSetIndexIndirectArgs (NID 0o3VDdtA6nM): 4-dword packet, address at
// dwords 1-2 and count at dword 3.
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexIndirectArgs(CommandBuffer* buf, uint64_t args_address, uint32_t count);
// sceAgcDcbPrimeUtcl2 / sceAgcAcbPrimeUtcl2 (NIDs jt3pl7EN17o, szG7hz2yEhA).
// 5-dword packet: engine/mode, 16KB-aligned address and size.
uint32_t* KYTY_SYSV_ABI GraphicsDcbPrimeUtcl2(CommandBuffer* buf, uint32_t engine, uint32_t mode, uint64_t address, uint32_t size);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPrimeUtcl2(CommandBuffer* buf, uint32_t engine, uint32_t mode, uint64_t address, uint32_t size);
// sceAgcCbCondWrite (NID 7toV+elXqNM): 9-dword COND_WRITE packet.
uint32_t* KYTY_SYSV_ABI GraphicsCbCondWrite(CommandBuffer* buf, uint32_t function, uint32_t mode, uint64_t value_a,
                                            uint32_t value_b, uint64_t value_c, uint32_t value_d, uint32_t value_e);
// AGC 0x3F jump/branch field patches exported without a catalog name:
// 7Wa3aeJgeVU patches the then-target, rP5xLdOf26k the else-target and
// Ikfdt-rIqCE the 4-dword jump. All take (cache_policy, target, size_dw).
int KYTY_SYSV_ABI GraphicsAgcJumpPatchTarget(uint32_t* cmd, uint32_t cache_policy, uint64_t target, uint32_t size_dw);
int KYTY_SYSV_ABI GraphicsAgcBranchPatchSetThenTarget(uint32_t* cmd, uint32_t cache_policy, uint64_t target, uint32_t size_dw);
int KYTY_SYSV_ABI GraphicsAgcBranchPatchSetElseTarget(uint32_t* cmd, uint32_t cache_policy, uint64_t target, uint32_t size_dw);
uint32_t* KYTY_SYSV_ABI GraphicsCbDispatch(CommandBuffer* buf, uint32_t group_x, uint32_t group_y, uint32_t group_z, uint32_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl, uint8_t dst, uint8_t cache_policy,
                                             const volatile Label* address, uint8_t data_sel, uint64_t data, uint16_t gds_offset,
                                             uint16_t gds_size, uint8_t interrupt, uint32_t interrupt_ctx_id);
uint32_t* KYTY_SYSV_ABI GraphicsDcbResetQueue(CommandBuffer* buf, uint32_t op, uint32_t state);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitUntilSafeForRendering(CommandBuffer* buf, uint32_t video_out_handle, uint32_t display_buffer_index);
// sceAgcDcbSetWorkloadsActive / Complete: encode workload tracking NOP payloads.
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id, const uint32_t* workload_ids,
                                                      uint32_t workload_count);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id, uint32_t workload_id);
// ACB variants share the DCB packet; the real builders only flip a packet bit
// that Kyty's software command processor does not distinguish.
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id, const uint32_t* workload_ids,
                                                      uint32_t workload_count);
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id, uint32_t workload_id);
uint32_t* KYTY_SYSV_ABI GraphicsAcbWaitUntilSafeForRendering(CommandBuffer* buf, uint32_t video_out_handle,
                                                             uint32_t display_buffer_index);
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle, int32_t display_buffer_index,
                                           uint32_t flip_mode, int64_t flip_arg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexSize(CommandBuffer* buf, uint8_t index_size, uint8_t cache_policy);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count, uint64_t modifier);
// sceAgcDcbDrawIndexOffset (NID B+aG9DUnTKA): IT_DRAW_INDEX_OFFSET_2.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset, uint32_t index_count, uint32_t flags);
// Optional base-vertex auto-draw helper. Not bound to B+aG9DUnTKA (that NID is Offset).
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoWithBase(CommandBuffer* buf, uint32_t base_vertex, uint32_t index_count,
                                                         uint64_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndex(CommandBuffer* buf, uint32_t index_count, const void* index_addr, uint64_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsDcbEventWrite(CommandBuffer* buf, uint8_t event_type, const volatile void* address);
// sceAgcDcbStallCommandBufferParser: fixed EVENT_WRITE with CS partial flush (0x07).
uint32_t* KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParser(CommandBuffer* buf);
// Graphics and async compute entry points share IT_DMA_DATA encoding but have distinct ABIs.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t destination,
                                           uint8_t destination_cache_policy, uint64_t destination_address, uint8_t source,
                                           uint8_t source_cache_policy, uint64_t source_address, uint32_t byte_count,
                                           uint8_t wait_for_previous, uint8_t write_confirm, uint8_t block_engine);
uint32_t* KYTY_SYSV_ABI GraphicsAcbDmaData(CommandBuffer* buf, uint8_t destination, uint8_t destination_cache_policy,
                                         uint64_t destination_address, uint8_t source, uint8_t source_cache_policy,
                                         uint64_t source_address, uint32_t byte_count, uint8_t wait_for_previous,
                                         uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsDcbAcquireMem(CommandBuffer* buf, uint8_t engine, uint32_t cb_db_op, uint32_t gcr_cntl,
                                              const volatile void* base, uint64_t size_bytes, uint32_t poll_cycles);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy, uint64_t address_or_offset,
                                             const void* data, uint32_t num_dwords, uint8_t increment, uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsDcbContextStateOp(CommandBuffer* buf, uint32_t operation);
uint64_t KYTY_SYSV_ABI GraphicsDcbContextStateOpGetSize(uint32_t operation);
// sceAgcDcbSetBaseIndirectArgs (NID RmaJwLtc8rY).
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetBaseIndirectArgs(CommandBuffer* buf, uint32_t base_index, uint64_t address);
// sceAgcDcbDispatchIndirect (NID CtB+A9-VxO0).
uint32_t* KYTY_SYSV_ABI GraphicsDcbDispatchIndirect(CommandBuffer* buf, uint32_t data_offset, uint32_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsAcbDispatchIndirect(CommandBuffer* buf, const volatile void* indirect_args, uint32_t modifier);
// sceAgcDcbDrawIndexIndirect (NID t1vNu082-jM).
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes, uint64_t modifier);
// sceAgcDcbDrawIndirect (NID 1q1titRBL6o).
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes, uint64_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function, uint8_t op, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference, uint64_t mask, uint32_t poll_cycles);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle, int32_t display_buffer_index, uint32_t flip_mode,
                                           int64_t flip_arg);

// Async CB forms: same packet encodings as the DCB builders with ABI differences
// observed on Gen5 libSceAgc (no op argument on WaitRegMem; slim AcquireMem).
uint32_t* KYTY_SYSV_ABI GraphicsAcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference, uint64_t mask, uint32_t poll_cycles);
uint32_t* KYTY_SYSV_ABI GraphicsAcbEventWrite(CommandBuffer* buf, uint8_t event_type);
uint32_t* KYTY_SYSV_ABI GraphicsAcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy, uint64_t address_or_offset,
                                             const void* data, uint32_t num_dwords, uint8_t increment, uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsAcbAcquireMem(CommandBuffer* buf, uint32_t gcr_cntl, const volatile void* base, uint64_t size_bytes,
                                              uint32_t poll_cycles);
uint32_t* KYTY_SYSV_ABI GraphicsAcbResetQueue(CommandBuffer* buf, uint32_t op);
uint32_t* KYTY_SYSV_ABI GraphicsAcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address, uint8_t src,
                                            uint8_t src_cache_policy, uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPopMarker(CommandBuffer* buf);

uint32_t* KYTY_SYSV_ABI GraphicsDcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address, uint8_t src,
                                            uint8_t src_cache_policy, uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsDcbPopMarker(CommandBuffer* buf);
// sceAgcAcbSetMarker (NID xAeBOa0A3kk) and the span forms (NIDs pxx-GoOSdw4,
// 8Kly1JrJUlw, BYcSvEsINWU, N7M3+N-sBCw): AgcDriver user-data marker packets.
// The span builders take an explicit byte length instead of a NUL-terminated
// string and map color bits into the user-data register value.
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetMarker(CommandBuffer* buf, const char* label, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetMarkerSpan(CommandBuffer* buf, const char* data, uint32_t size, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPushMarkerSpan(CommandBuffer* buf, const char* data, uint32_t size, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetMarkerSpan(CommandBuffer* buf, const char* data, uint32_t size, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsDcbPushMarkerSpan(CommandBuffer* buf, const char* data, uint32_t size, uint32_t color);
// sceAgcAcbAtomicMem / sceAgcDcbAtomicMem (NIDs XKKuA6VkSRc, 1-gUn1PI4Sw):
// 9-dword ATOMIC_MEM packet. Header bits 30-31 exist only on the DCB form;
// bits 0-6 carry the atomic op, 8-11 the cache policy and 25-26 the memory type.
// Size is encoded in 64-byte units and saturates at 0xfff.
uint32_t* KYTY_SYSV_ABI GraphicsAcbAtomicMem(CommandBuffer* buf, uint32_t op, uint32_t cache_policy, uint64_t address,
                                             uint32_t mem_type, uint64_t src, uint64_t src2, uint32_t size);
uint32_t* KYTY_SYSV_ABI GraphicsDcbAtomicMem(CommandBuffer* buf, uint32_t op_hi, uint32_t op, uint32_t cache_policy,
                                             uint32_t mem_type, uint64_t address, uint64_t src, uint64_t src2, uint32_t size);
// sceAgcAcbMemSemaphore / sceAgcDcbMemSemaphore (NIDs q4VuU-QsLOE,
// G0jrLdvEqDw): user-data address packet, 4-dword MEM_SEMAPHORE and optional
// bracket packets for modes 6 and 7.
uint32_t* KYTY_SYSV_ABI GraphicsAcbMemSemaphore(CommandBuffer* buf, uint64_t address, uint8_t acquire, uint8_t release,
                                                uint32_t mode);
uint32_t* KYTY_SYSV_ABI GraphicsDcbMemSemaphore(CommandBuffer* buf, uint64_t address, uint8_t acquire, uint8_t release,
                                                uint32_t mode);
// Astro imports two unnamed AtomicGds writers (NIDs gQkqkLttcpw, zARR5aCmkoY)
// that share the 11-dword 0xC0091D00 layout but pack the flag word differently
// from the older named exports: bits 5/6 are duplicated into 18/20, bit 9 moves
// to 16, and the ACB form has no queue bits.
uint32_t* KYTY_SYSV_ABI GraphicsAcbAtomicGds(CommandBuffer* buf, uint32_t flags, int32_t engine_flag,
                                             uint32_t cache_policy, uint32_t op, uint64_t address_low,
                                             uint16_t address_high, uint32_t mask, uint32_t data, uint64_t src,
                                             uint64_t src2);
uint32_t* KYTY_SYSV_ABI GraphicsDcbAtomicGds(CommandBuffer* buf, uint32_t queue, uint32_t flags, int32_t engine_flag,
                                             uint32_t cache_policy, uint32_t op, uint16_t address_low,
                                             uint16_t address_high, uint32_t mask, uint32_t data, uint64_t src,
                                             uint64_t src2);
// sceAgcSetAmmSemaphoreMemory / sceAgcGetSemaphoreLabel (NIDs OQTgEXyihvA,
// hFQ9pUxoLQ4): AMM semaphore labels are 0x20-byte slots in the registered
// range. Returns the 0x8a6c00xx Agc status codes.
uint64_t KYTY_SYSV_ABI GraphicsAgcSetAmmSemaphoreMemory(uint64_t base, uint64_t size);
uint64_t KYTY_SYSV_ABI GraphicsAgcGetSemaphoreLabel(uint64_t oh, uint64_t* label);
// sceAgcGetDefaultCxStateFlat (NID AAeX-U5-P3M): expands the default Cx
// register state for [start, end) in the flat 0x400-entry context register
// space; registers without a modeled default read back as zero.
uint64_t KYTY_SYSV_ABI GraphicsAgcGetDefaultCxStateFlat(uint32_t* out, uint32_t start, uint32_t end,
                                                        uint32_t state_index);
// sceAgcAcb/DcbSetWorkloadStreamInactive (NIDs FcgdDM3MB+k, FneFypEDRgY):
// user-data write of the stream's inactive mask; invalid ids, unregistered
// streams and allocation failures return nullptr.
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetWorkloadStreamInactive(CommandBuffer* buf, uint32_t stream_id);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadStreamInactive(CommandBuffer* buf, uint32_t stream_id);
// sceAgcDcbDrawIndirectMulti / DrawIndexIndirectMulti / DrawIndexMultiInstanced
// (NIDs kUlvghKs-mA, ypVBz4uPKcQ, Rlx+bykm0r0): user-data brackets around the
// hardware multi-draw packet. `flags` selects the count/index register fields.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndirectMulti(CommandBuffer* buf, uint32_t data_offset, uint32_t index_size,
                                                     uint32_t max_index_count, uint64_t indirect_args, uint32_t draw_count,
                                                     uint64_t flags);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectMulti(CommandBuffer* buf, uint32_t data_offset, uint32_t index_size,
                                                          uint32_t max_index_count, uint64_t indirect_args, uint32_t draw_count,
                                                          uint64_t flags);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexMultiInstanced(CommandBuffer* buf, uint32_t index_count, uint64_t index_address,
                                                           uint64_t instance_address, int32_t instance_offset, uint64_t flags);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexBuffer(CommandBuffer* buf, uint64_t index_addr);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexCount(CommandBuffer* buf, uint32_t index_count);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetNumInstances(CommandBuffer* buf, uint32_t num_instances);
uint32_t* KYTY_SYSV_ABI GraphicsCbJump(CommandBuffer* buf, uint32_t mode, uint32_t cache_policy, const void* target, uint32_t size_dw);
uint32_t* KYTY_SYSV_ABI GraphicsAcbCondExec(CommandBuffer* buf, uint64_t address, uint32_t count);
uint32_t* KYTY_SYSV_ABI GraphicsDcbCondExec(CommandBuffer* buf, uint64_t address, uint32_t count);
uint32_t* KYTY_SYSV_ABI GraphicsAcbRewind(CommandBuffer* buf, int32_t flags, uint8_t queue);
uint32_t* KYTY_SYSV_ABI GraphicsDcbRewind(CommandBuffer* buf, int32_t flags);
uint32_t KYTY_SYSV_ABI  GraphicsCbJumpGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbCondExecGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbCondExecGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbRewindGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbRewindGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbDmaDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbCopyDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbAcquireMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsAcbEventWriteGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbEventWriteGetSize(uint8_t event_type);
uint32_t KYTY_SYSV_ABI GraphicsAcbDispatchIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbAcquireMemGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbCopyDataGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDispatchIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexOffsetGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexBufferGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexCountGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetIndexSizeGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetNumInstancesGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirectGetSize();
uint32_t KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirectGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsCbBranch(CommandBuffer* buf, uint32_t mode, uint32_t function, const volatile uint64_t* compare_address,
                                         uint64_t mask, uint64_t reference, uint32_t then_cache_policy, const void* then_target,
                                         uint32_t then_size_dw, uint32_t else_cache_policy, const void* else_target, uint32_t else_size_dw);
uint32_t KYTY_SYSV_ABI  GraphicsCbBranchGetSize();
int KYTY_SYSV_ABI       GraphicsSetPacketPredication(uint32_t* packet, uint32_t predication);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetPredication(CommandBuffer* buf, uint32_t condition, uint32_t operation, uint32_t wait,
                                                  const volatile void* address);
uint32_t* KYTY_SYSV_ABI GraphicsDcbGetLodStats(CommandBuffer* buf, uint8_t cache_policy, const volatile void* buffer,
                                               uint32_t buffer_size_in_bytes, uint32_t reset_count, uint8_t force_reset,
                                               uint8_t report_and_reset, uint32_t reporting_interval_in_100k_clocks);

} // namespace Gen5

namespace Gen5Driver {

struct Packet;

int KYTY_SYSV_ABI GraphicsDriverSubmitDcb(const Packet* packet);
int KYTY_SYSV_ABI GraphicsDriverSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs, const uint32_t* dcb_sizes_in_dwords,
                                                 uint32_t count);
int KYTY_SYSV_ABI GraphicsDriverAgrSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs,
                                                    const uint32_t* dcb_sizes_in_dwords, uint32_t count);
int KYTY_SYSV_ABI GraphicsDriverSubmitAcb(uint32_t queue, const Packet* packet);
// sceAgcDriverSubmitMultiAcbs (NID HF3YllT3mXU): the wrapper passes a packet
// pointer and a separate dword-size array; every entry is an async ACB.
int KYTY_SYSV_ABI GraphicsDriverSubmitMultiAcbs(uint32_t queue, const Packet* const* packets,
                                                 const uint32_t* sizes_in_dwords, uint32_t count);
int KYTY_SYSV_ABI GraphicsDriverAddEqEvent(Kernel::EventQueue::KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI GraphicsDriverDeleteEqEvent(Kernel::EventQueue::KernelEqueue eq, int id);
uint32_t KYTY_SYSV_ABI GraphicsDriverGetEqContextId(const Kernel::EventQueue::KernelEvent* ev);
// sceAgcDriverGetEqEventType (NID 5CdQTZIQPxM) reads the current graphics
// event's data (sceKernelGetEventData) and returns its upper half.
uint32_t KYTY_SYSV_ABI GraphicsDriverGetEqEventType(const Kernel::EventQueue::KernelEvent* ev);
// Astro 01.018 AgcDriver fakelib stubs: these NIDs return a constant without
// inspecting their arguments. 0x8a6c9018 covers the resource/GDS inspection
// group, 0x8a6c1000 the capture/validation group, 0 the status queries.
int KYTY_SYSV_ABI GraphicsDriverResourceApiUnavailable();
int KYTY_SYSV_ABI GraphicsDriverDebugApiUnavailable();
int KYTY_SYSV_ABI GraphicsDriverStatusDisabled();
int KYTY_SYSV_ABI GraphicsDriverShaderDebuggingStatus();
int KYTY_SYSV_ABI GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(size_t* size, uint32_t max_resources,
                                                                                 uint32_t max_owners);
int KYTY_SYSV_ABI GraphicsDriverInitResourceRegistration(void* memory, size_t size, uint32_t max_owners);
int KYTY_SYSV_ABI GraphicsDriverRegisterDefaultOwner(uint32_t options);
int KYTY_SYSV_ABI GraphicsDriverGetDefaultOwner(uint32_t* owner);
int KYTY_SYSV_ABI GraphicsDriverGetResourceRegistrationMaxNameLength(uint32_t* max_length);
int KYTY_SYSV_ABI GraphicsDriverRegisterOwner(uint32_t* owner, const char* name);
int KYTY_SYSV_ABI GraphicsDriverRegisterResource(uint32_t* resource, uint32_t owner, const void* base, uint64_t size, const char* name,
                                                 uint32_t type, uint64_t user_data);
int KYTY_SYSV_ABI GraphicsDriverUnregisterResource(uint32_t resource);
int KYTY_SYSV_ABI GraphicsDriverSetTFRing(const volatile void* base, uint32_t size);
int KYTY_SYSV_ABI GraphicsDriverSetHsOffchipParam(uint64_t value0, uint64_t value1, uint64_t value2);

} // namespace Gen5Driver

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICS_H_ */
