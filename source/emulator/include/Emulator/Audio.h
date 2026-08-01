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

// Host lifecycle control. Pausing closes the producer gate before freezing
// devices; resuming starts devices before releasing waiting guest producers.
void AudioOutSetHostPaused(bool paused);

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

int KYTY_SYSV_ABI   AjmInitialize(int64_t reserved, uint32_t* context);
int KYTY_SYSV_ABI   AjmFinalize(uint32_t context);
int KYTY_SYSV_ABI   AjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
int KYTY_SYSV_ABI   AjmModuleUnregister(uint32_t context, uint32_t codec);
int KYTY_SYSV_ABI   AjmMemoryRegister(uint32_t context, const void* address, size_t num_pages);
int KYTY_SYSV_ABI   AjmBatchInitializeBuffer(void* buffer, size_t buffer_size, void* control);
int KYTY_SYSV_ABI   AjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance);
int KYTY_SYSV_ABI   AjmInstanceDestroy(uint32_t context, uint32_t instance);
void* KYTY_SYSV_ABI AjmBatchJobControlBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* sideband_input,
                                               size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                                               void* return_address);
void* KYTY_SYSV_ABI AjmBatchJobInlineBuffer(void* buffer, const void* data_input, size_t data_input_size, const void** batch_address);
void* KYTY_SYSV_ABI AjmBatchJobRunBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* data_input, size_t data_input_size,
                                           void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size,
                                           void* return_address);
void* KYTY_SYSV_ABI AjmBatchJobRunSplitBufferRa(void* buffer, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                                size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                                size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size,
                                                void* return_address);
int KYTY_SYSV_ABI   AjmBatchStartBuffer(uint32_t context, uint8_t* batch_buffer, uint32_t batch_size, int priority, AjmBatchError* error,
                                        uint32_t* batch_id);
int KYTY_SYSV_ABI   AjmBatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, AjmBatchError* error);
int KYTY_SYSV_ABI   AjmBatchCancel(uint32_t context, uint32_t batch_id);
int KYTY_SYSV_ABI   AjmBatchJobInitialize(void* batch, uint32_t instance, const void* config, size_t config_size, void* result);
int KYTY_SYSV_ABI   AjmBatchJobClearContext(void* batch, uint32_t instance, void* result);
int KYTY_SYSV_ABI   AjmBatchJobSetGaplessDecode(void* batch, uint32_t instance, const void* config, uint64_t enabled, void* result);
int KYTY_SYSV_ABI   AjmBatchJobGetGaplessDecode(void* batch, uint32_t instance, void* result);
int KYTY_SYSV_ABI   AjmBatchJobSetResampleParameters(void* batch, uint32_t instance, float ratio, uint32_t flags, void* result);
int KYTY_SYSV_ABI   AjmBatchJobSetResampleParametersEx(void* batch, uint32_t instance, float ratio_start,
                                                       float ratio_change_per_sample, uint32_t flags, void* result);
int KYTY_SYSV_ABI   AjmBatchJobGetResampleInfo(void* batch, uint32_t instance, void* result);
int KYTY_SYSV_ABI   AjmBatchJobDecode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                      size_t data_output_size, void* result, void* return_address, uint64_t reserved, void* result_alias);
int KYTY_SYSV_ABI   AjmBatchJobDecodeSingle(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                            size_t data_output_size, void* result);
int KYTY_SYSV_ABI   AjmBatchJobDecodeSplit(void* batch, uint32_t instance, const AjmBuffer* data_input_buffers,
                                           size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                           size_t data_output_buffer_count, void* result);
int KYTY_SYSV_ABI   AjmBatchJobEncode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                      size_t data_output_size, void* result);
int KYTY_SYSV_ABI   AjmBatchJobGetInfo(void* batch, uint32_t instance, void* result);
int KYTY_SYSV_ABI   AjmBatchJobGetCodecInfo(void* batch, uint32_t instance, void* result, size_t result_size);
int KYTY_SYSV_ABI   AjmBatchJobGetStatistics(void* batch, float interval, void* result);
int KYTY_SYSV_ABI   AjmBatchJobControl(void* batch, uint32_t instance, uint64_t flags, const void* sideband_input, size_t sideband_input_size,
                                       void* sideband_output, size_t sideband_output_size);
int KYTY_SYSV_ABI   AjmBatchJobRun(void* batch, uint32_t instance, uint64_t flags, const void* data_input, size_t data_input_size,
                                   void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size);
int KYTY_SYSV_ABI   AjmBatchJobRunSplit(void* batch, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                        size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                        size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size);
int KYTY_SYSV_ABI   AjmBatchStart(uint32_t context, void* batch, int priority, AjmBatchError* error, uint32_t* batch_id);
const char* KYTY_SYSV_ABI AjmStrError(int error);

} // namespace Ajm

namespace AvPlayer {

using Bool = uint8_t;

using AvPlayerAllocate          = KYTY_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocate        = KYTY_SYSV_ABI void (*)(void*, void*);
using AvPlayerAllocateTexture   = KYTY_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocateTexture = KYTY_SYSV_ABI void (*)(void*, void*);
using AvPlayerOpenFile          = KYTY_SYSV_ABI int (*)(void*, const char*);
using AvPlayerCloseFile         = KYTY_SYSV_ABI int (*)(void*);
using AvPlayerReadOffsetFile    = KYTY_SYSV_ABI int (*)(void*, uint8_t*, uint64_t, uint32_t);
using AvPlayerSizeFile          = KYTY_SYSV_ABI uint64_t (*)(void*);
using AvPlayerEventCallback     = KYTY_SYSV_ABI void (*)(void*, uint32_t, int32_t, void*);

enum AvPlayerUriType : uint32_t
{
	AvPlayerUriTypeSource = 0,
};

enum AvPlayerStreamType : uint32_t
{
	AvPlayerStreamUnknown   = 0,
	AvPlayerStreamVideo     = 1,
	AvPlayerStreamAudio     = 2,
	AvPlayerStreamTimedText = 3,
};

enum AvPlayerSourceType : uint32_t
{
	AvPlayerSourceUnknown  = 0,
	AvPlayerSourceFileMp4  = 1,
	AvPlayerSourceFileWebm = 2,
	AvPlayerSourceHls      = 8,
};

struct AvPlayerMemAllocator
{
	void*                     object_pointer     = nullptr;
	AvPlayerAllocate          allocate           = nullptr;
	AvPlayerDeallocate        deallocate         = nullptr;
	AvPlayerAllocateTexture   allocate_texture   = nullptr;
	AvPlayerDeallocateTexture deallocate_texture = nullptr;
};

struct AvPlayerFileReplacement
{
	void*                  object_pointer = nullptr;
	AvPlayerOpenFile       open           = nullptr;
	AvPlayerCloseFile      close          = nullptr;
	AvPlayerReadOffsetFile read_offset    = nullptr;
	AvPlayerSizeFile       size           = nullptr;
};

struct AvPlayerEventReplacement
{
	void*                 object_pointer = nullptr;
	AvPlayerEventCallback event_callback = nullptr;
};

struct AvPlayerInitData
{
	AvPlayerMemAllocator     memory_replacement;
	AvPlayerFileReplacement  file_replacement;
	AvPlayerEventReplacement event_replacement;
	uint32_t                 debug_level                   = 0;
	uint32_t                 base_priority                 = 0;
	int32_t                  num_output_video_framebuffers = 0;
	Bool                     auto_start                    = 0;
	uint8_t                  reserved[3]                   = {};
	const char*              default_language              = nullptr;
};

struct AvPlayerThreadInfo
{
	uint32_t priority     = 0;
	uint32_t stack_size   = 0;
	uint64_t affinity     = 0;
	uint8_t  reserved[32] = {};
};

struct AvPlayerInitDataEx
{
	size_t                   this_size = 0;
	AvPlayerMemAllocator     memory_replacement;
	AvPlayerFileReplacement  file_replacement;
	AvPlayerEventReplacement event_replacement;
	const char*              default_language = nullptr;
	uint32_t                 debug_level      = 0;
	Bool                     auto_start       = 0;
	uint8_t                  reserved[3]      = {};
	AvPlayerThreadInfo       audio_decoder;
	AvPlayerThreadInfo       video_decoder;
	AvPlayerThreadInfo       demuxer;
	AvPlayerThreadInfo       event;
	AvPlayerThreadInfo       call_queue;
	AvPlayerThreadInfo       http_command_processor;
	AvPlayerThreadInfo       http_segment_manager;
	AvPlayerThreadInfo       http_streamlist;
	AvPlayerThreadInfo       file_streaming;
	int32_t                  num_output_video_framebuffers = 0;
	uint8_t                  reserved2[4]                  = {};
};

struct AvPlayerUri
{
	const char* name   = nullptr;
	uint32_t    length = 0;
};

struct AvPlayerSourceDetails
{
	AvPlayerUri        uri;
	uint8_t            reserved1[64] = {};
	AvPlayerSourceType source_type   = AvPlayerSourceUnknown;
	uint8_t            reserved2[44] = {};
};

struct AvPlayerAudioEx
{
	uint16_t channel_count;
	uint8_t  reserved[2];
	uint32_t sample_rate;
	uint32_t size;
	uint8_t  language_code[4];
	uint8_t  reserved1[64];
};

struct AvPlayerAudio
{
	uint16_t channel_count;
	uint8_t  reserved[2];
	uint32_t sample_rate;
	uint32_t size;
	uint8_t  language_code[4];
};

struct AvPlayerVideoEx
{
	uint32_t width;
	uint32_t height;
	float    aspect_ratio;
	uint8_t  language_code[4];
	uint8_t  reserved[4];
	uint32_t crop_left_offset;
	uint32_t crop_right_offset;
	uint32_t crop_top_offset;
	uint32_t crop_bottom_offset;
	uint32_t pitch;
	uint8_t  luma_bit_depth;
	uint8_t  chroma_bit_depth;
	Bool     video_full_tange_flag;
	uint8_t  reserved1[5];
	double   framerate;
	uint32_t colour_primaries;
	uint32_t transfer_characteristics;
	uint8_t  reserved2[16];
};

struct AvPlayerVideo
{
	uint32_t width;
	uint32_t height;
	float    aspect_ratio;
	uint8_t  language_code[4];
};

struct AvPlayerTimedTextEx
{
	uint8_t language_code[4];
	uint8_t reserved[12];
	uint8_t reserved1[64];
};

struct AvPlayerTimedText
{
	uint8_t language_code[4];
	uint8_t reserved[12];
};

union AvPlayerStreamDetailsEx
{
	AvPlayerAudioEx     audio;
	AvPlayerVideoEx     video;
	AvPlayerTimedTextEx subs;
	uint8_t             reserved1[80];
};

union AvPlayerStreamDetails
{
	AvPlayerAudio     audio;
	AvPlayerVideo     video;
	AvPlayerTimedText subs;
	uint8_t           reserved[16];
};

struct AvPlayerFrameInfo
{
	void*                 data;
	uint8_t               reserved[4];
	uint64_t              time_stamp;
	AvPlayerStreamDetails details;
};

struct AvPlayerFrameInfoEx
{
	void*                   data;
	uint8_t                 reserved[4];
	uint64_t                time_stamp;
	AvPlayerStreamDetailsEx details;
};

struct AvPlayerStreamInfo
{
	AvPlayerStreamType    type;
	uint8_t               reserved[4];
	AvPlayerStreamDetails details;
	uint64_t              duration;
};

struct AvPlayerStreamInfoEx
{
	size_t                  this_size;
	AvPlayerStreamType      type;
	uint8_t                 reserved[4];
	AvPlayerStreamDetailsEx details;
	uint64_t                duration;
};

struct AvPlayerInternal;

AvPlayerInternal* KYTY_SYSV_ABI AvPlayerInit(AvPlayerInitData* init);
int KYTY_SYSV_ABI               AvPlayerInitEx(const AvPlayerInitDataEx* init, AvPlayerInternal** handle);
int KYTY_SYSV_ABI               AvPlayerPostInit(AvPlayerInternal* h, const void* post_init);
int KYTY_SYSV_ABI               AvPlayerAddSource(AvPlayerInternal* h, const char* filename);
int KYTY_SYSV_ABI               AvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type, const AvPlayerSourceDetails* source_details);
int KYTY_SYSV_ABI               AvPlayerStreamCount(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfo* info);
int KYTY_SYSV_ABI               AvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfoEx* info);
int KYTY_SYSV_ABI               AvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id);
int KYTY_SYSV_ABI               AvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id);
int KYTY_SYSV_ABI               AvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id, uint32_t new_stream_id);
int KYTY_SYSV_ABI               AvPlayerStart(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex);
int KYTY_SYSV_ABI               AvPlayerStop(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerPause(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerResume(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerSetLooping(AvPlayerInternal* h, Bool loop);
int KYTY_SYSV_ABI               AvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode);
int KYTY_SYSV_ABI               AvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth,
                                                              uint32_t minimum_bandwidth, uint32_t maximum_bandwidth);
int KYTY_SYSV_ABI               AvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed);
Bool KYTY_SYSV_ABI              AvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info);
Bool KYTY_SYSV_ABI              AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info);
Bool KYTY_SYSV_ABI              AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info);
Bool KYTY_SYSV_ABI              AvPlayerIsActive(AvPlayerInternal* h);
uint64_t KYTY_SYSV_ABI          AvPlayerCurrentTime(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms);
int KYTY_SYSV_ABI               AvPlayerClose(AvPlayerInternal* h);
int KYTY_SYSV_ABI               AvPlayerSetLogCallback(void* callback, void* user_data);
void                            ConvertNv12ToRgba32(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint8_t* rgba_dst);

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
