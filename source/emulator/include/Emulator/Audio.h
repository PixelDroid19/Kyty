#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

KYTY_SUBSYSTEM_DEFINE(Audio);

namespace AudioOut {

struct AudioOutOutputParam;
struct AudioOutPortState;

int KYTY_SYSV_ABI AudioOutInit();
int KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol);
int KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num);
int KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr);
int KYTY_SYSV_ABI AudioOutClose(int handle);
int KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state);

} // namespace AudioOut

// Gen5 AudioOut2 library (same module AudioOut_v1.1, distinct library AudioOut2_v1).
namespace AudioOut2 {

int KYTY_SYSV_ABI AudioOut2Initialize();
int KYTY_SYSV_ABI AudioOut2ContextResetParam(void* param);
int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const void* param, uint64_t* size_out);
int KYTY_SYSV_ABI AudioOut2ContextCreate(const void* param, void* buffer, uint64_t size, int32_t* handle_out);
int KYTY_SYSV_ABI AudioOut2ContextDestroy(int32_t handle);
int KYTY_SYSV_ABI AudioOut2ContextAdvance(int32_t handle);
int KYTY_SYSV_ABI AudioOut2ContextPush(int32_t handle, const void* data);
int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(int32_t handle, uint32_t* used, uint32_t* available);
int KYTY_SYSV_ABI AudioOut2PortCreate(int32_t context, const void* param, int32_t* port_out);
int KYTY_SYSV_ABI AudioOut2PortDestroy(int32_t port);
int KYTY_SYSV_ABI AudioOut2PortSetAttributes(int32_t port, const void* attr);
// sceAudioOut2PortGetState (NID gatEUKG+Ea4): 0x20-byte guest state blob.
int KYTY_SYSV_ABI AudioOut2PortGetState(int32_t port, void* state_out);
int KYTY_SYSV_ABI AudioOut2UserCreate(int user_id, const void* param, int32_t* user_out);
int KYTY_SYSV_ABI AudioOut2UserDestroy(int32_t user);
// Residual AudioOut2 NIDs imported by Gen5 titles whose names are not yet
// triangulated; log SysV args and return SCE_OK so boot can proceed.
int KYTY_SYSV_ABI AudioOut2LogAndOk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

} // namespace AudioOut2

namespace AudioIn {

int KYTY_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int KYTY_SYSV_ABI AudioInInput(int handle, void* dest);

} // namespace AudioIn

namespace VoiceQoS {

int KYTY_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type);

} // namespace VoiceQoS

namespace Ajm {

struct AjmBuffer
{
	void*  address;
	size_t size;
};

struct AjmBatchError
{
	int32_t     error_code;
	const void* job_address;
	uint32_t    command_offset;
	const void* job_return_address;
};

int KYTY_SYSV_ABI AjmInitialize(int64_t reserved, uint32_t* context);
int KYTY_SYSV_ABI AjmFinalize(uint32_t context);
int KYTY_SYSV_ABI AjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
int KYTY_SYSV_ABI AjmModuleUnregister(uint32_t context, uint32_t codec);
int KYTY_SYSV_ABI AjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance);
int KYTY_SYSV_ABI AjmInstanceDestroy(uint32_t context, uint32_t instance);
void* KYTY_SYSV_ABI AjmBatchJobControlBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* sideband_input,
                                               size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                                               void* return_address);
void* KYTY_SYSV_ABI AjmBatchJobInlineBuffer(void* buffer, const void* data_input, size_t data_input_size,
                                            const void** batch_address);
void* KYTY_SYSV_ABI AjmBatchJobRunBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* data_input,
                                           size_t data_input_size, void* data_output, size_t data_output_size,
                                           void* sideband_output, size_t sideband_output_size, void* return_address);
void* KYTY_SYSV_ABI AjmBatchJobRunSplitBufferRa(void* buffer, uint32_t instance, uint64_t flags,
                                                const AjmBuffer* data_input_buffers, size_t data_input_buffer_count,
                                                const AjmBuffer* data_output_buffers, size_t data_output_buffer_count,
                                                void* sideband_output, size_t sideband_output_size, void* return_address);
int KYTY_SYSV_ABI AjmBatchStartBuffer(uint32_t context, uint8_t* batch_buffer, uint32_t batch_size, int priority,
                                      AjmBatchError* error, uint32_t* batch_id);
int KYTY_SYSV_ABI AjmBatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, AjmBatchError* error);
int KYTY_SYSV_ABI AjmBatchCancel(uint32_t context, uint32_t batch_id);

} // namespace Ajm

namespace AvPlayer {

struct AvPlayerInitData;
struct AvPlayerFrameInfoEx;
struct AvPlayerFrameInfo;
struct AvPlayerInternal;

using Bool = uint8_t;

AvPlayerInternal* KYTY_SYSV_ABI AvPlayerInit(AvPlayerInitData* init);
int KYTY_SYSV_ABI               AvPlayerAddSource(AvPlayerInternal* h, const char* filename);
int KYTY_SYSV_ABI               AvPlayerSetLooping(AvPlayerInternal* h, Bool loop);
Bool KYTY_SYSV_ABI              AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info);
Bool KYTY_SYSV_ABI              AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info);
Bool KYTY_SYSV_ABI              AvPlayerIsActive(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerClose(AvPlayerInternal* h);

} // namespace AvPlayer

namespace Audio3d {

struct Audio3dOpenParameters;

int KYTY_SYSV_ABI  Audio3dInitialize(int64_t reserved);
void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p);
int KYTY_SYSV_ABI  Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id);
int KYTY_SYSV_ABI  Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size);
int KYTY_SYSV_ABI  Audio3dPortGetAttributesSupported(uint32_t port_id, uint32_t* capabilities, uint32_t* num_capabilities);
int KYTY_SYSV_ABI  Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available);
int KYTY_SYSV_ABI  Audio3dAudioOutOpen(uint32_t port_id, int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
int KYTY_SYSV_ABI  Audio3dAudioOutOutput(int handle, const void* data);
int KYTY_SYSV_ABI  Audio3dAudioOutClose(int handle);
int KYTY_SYSV_ABI  Audio3dPortAdvance(uint32_t port_id);
int KYTY_SYSV_ABI  Audio3dPortPush(uint32_t port_id, uint32_t blocking);

} // namespace Audio3d

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2RackOption;
struct Ngs2BufferAllocator;
struct Ngs2VoiceParamHeader;
struct Ngs2RenderBufferInfo;
struct Ngs2ContextBufferInfo;
struct Ngs2VoiceState;

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list);
int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands, uint32_t num_commands);
int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size);
int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);
int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info);
int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t grain_samples);
int KYTY_SYSV_ABI Ngs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate);
int KYTY_SYSV_ABI Ngs2PanInit(void* pan_param);
// 3D geometry helpers (positional audio). Observed NIDs: ResetSource/Listener, CalcListener, Apply.
int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(void* out_source_param);
int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(void* out_listener_param);
int KYTY_SYSV_ABI Ngs2GeomCalcListener(const void* listener_param, void* out_work, uint32_t flags);
int KYTY_SYSV_ABI Ngs2GeomApply(const void* listener_work, const void* source_param, void* out_attrib, uint32_t flags);

} // namespace Ngs2

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO_H_ */
