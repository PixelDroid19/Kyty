#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_

#include "Kyty/Core/Common.h"
//#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/EventQueue.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
class CommandBuffer;
// struct VulkanSwapchain;
struct VideoOutVulkanImage;
} // namespace Kyty::Libs::Graphics

namespace Kyty::Libs::VideoOut {

using VideoOutQuiescedAction = bool (*)(void*);

// VideoOut owns its guest error contract. Keeping these values beside the
// VideoOut ABI prevents graphics code from depending on the aggregate Libs
// errno table merely to return a display error.
constexpr int VIDEO_OUT_ERROR_INVALID_VALUE                    = -2144796671; /* 0x80290001 */
constexpr int VIDEO_OUT_ERROR_INVALID_ADDRESS                  = -2144796670; /* 0x80290002 */
constexpr int VIDEO_OUT_ERROR_INVALID_PIXEL_FORMAT             = -2144796669; /* 0x80290003 */
constexpr int VIDEO_OUT_ERROR_INVALID_PITCH                    = -2144796668; /* 0x80290004 */
constexpr int VIDEO_OUT_ERROR_INVALID_RESOLUTION               = -2144796667; /* 0x80290005 */
constexpr int VIDEO_OUT_ERROR_INVALID_FLIP_MODE                = -2144796666; /* 0x80290006 */
constexpr int VIDEO_OUT_ERROR_INVALID_TILING_MODE              = -2144796665; /* 0x80290007 */
constexpr int VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO             = -2144796664; /* 0x80290008 */
constexpr int VIDEO_OUT_ERROR_RESOURCE_BUSY                    = -2144796663; /* 0x80290009 */
constexpr int VIDEO_OUT_ERROR_INVALID_INDEX                    = -2144796662; /* 0x8029000A */
constexpr int VIDEO_OUT_ERROR_INVALID_HANDLE                   = -2144796661; /* 0x8029000B */
constexpr int VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE              = -2144796660; /* 0x8029000C */
constexpr int VIDEO_OUT_ERROR_INVALID_EVENT                    = -2144796659; /* 0x8029000D */
constexpr int VIDEO_OUT_ERROR_NO_EMPTY_SLOT                    = -2144796657; /* 0x8029000F */
constexpr int VIDEO_OUT_ERROR_SLOT_OCCUPIED                    = -2144796656; /* 0x80290010 */
constexpr int VIDEO_OUT_ERROR_FLIP_QUEUE_FULL                  = -2144796654; /* 0x80290012 */
constexpr int VIDEO_OUT_ERROR_INVALID_MEMORY                   = -2144796653; /* 0x80290013 */
constexpr int VIDEO_OUT_ERROR_MEMORY_NOT_PHYSICALLY_CONTIGUOUS = -2144796652; /* 0x80290014 */
constexpr int VIDEO_OUT_ERROR_MEMORY_INVALID_ALIGNMENT         = -2144796651; /* 0x80290015 */
constexpr int VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE          = -2144796650; /* 0x80290016 */
constexpr int VIDEO_OUT_ERROR_OVERFLOW                         = -2144796649; /* 0x80290017 */
constexpr int VIDEO_OUT_ERROR_NO_DEVICE                        = -2144796648; /* 0x80290018 */
constexpr int VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE          = -2144796647; /* 0x80290019 */
constexpr int VIDEO_OUT_ERROR_INVALID_OPTION                   = -2144796646; /* 0x8029001A */
constexpr int VIDEO_OUT_ERROR_PORT_UNSUPPORTED_FUNCTION        = -2144796645; /* 0x8029001B */
constexpr int VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION            = -2144796644; /* 0x8029001C */
constexpr int VIDEO_OUT_ERROR_FATAL                            = -2144796417; /* 0x802900FF */
constexpr int VIDEO_OUT_ERROR_UNKNOWN                          = -2144796418; /* 0x802900FE */
constexpr int VIDEO_OUT_ERROR_ENOMEM                           = -2144792564; /* 0x8029100C */

struct VideoOutResolutionStatus;
struct VideoOutBufferAttribute;
struct VideoOutBufferAttribute2;
struct VideoOutFlipStatus;
struct VideoOutVblankStatus;
struct VideoOutBuffers;

enum class VideoOutRegisteredHostExtentStatus
{
	Uniform,
	Unselected,
	NonUniform,
	InvalidArgument,
	NoBuffers,
};

[[nodiscard]] const char* VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus status);

struct VideoOutBufferImageInfo
{
	Graphics::VideoOutVulkanImage* image        = nullptr;
	uint32_t                       index        = static_cast<uint32_t>(-1);
	uint64_t                       buffer_size  = 0;
	uint64_t                       buffer_pitch = 0;
};

void                               VideoOutInit(uint32_t width, uint32_t height);
VideoOutBufferImageInfo            VideoOutGetImageMetadataForSubmission(uint64_t addr, Graphics::CommandBuffer* buffer);
VideoOutBufferImageInfo            VideoOutGetImageForSubmission(uint64_t addr, Graphics::CommandBuffer* buffer);
VideoOutRegisteredHostExtentStatus VideoOutGetRegisteredHostExtent(Graphics::CommandBuffer* buffer, uint32_t guest_width,
                                                                   uint32_t guest_height, uint32_t* host_width,
                                                                   uint32_t* host_height);
VideoOutRegisteredHostExtentStatus VideoOutSelectRegisteredHostExtent(Graphics::CommandBuffer* buffer, uint32_t guest_width,
                                                                      uint32_t guest_height, uint32_t host_width,
                                                                      uint32_t host_height);
void                               VideoOutWaitFlipDone(int handle, int index);
bool                               VideoOutRunBufferUnmapTransaction(uint64_t vaddr, uint64_t size, VideoOutQuiescedAction action,
                                                                    void* data);

// Pure helper: map handle 0 to the sole opened slot when exactly one port is
// open (Gen5 WaitUntilSafe encodes handle 0 while Open returns 1). Non-zero
// handles pass through. Ambiguous (0 or 2+ open) returns 0 unchanged.
int VideoOutResolveHandle(int handle, const bool* opened, int num_slots);
bool VideoOutIsValidFlipMode(int flip_mode);

KYTY_SYSV_ABI int  VideoOutOpen(int user_id, int bus_type, int index, const void* param);
KYTY_SYSV_ABI int  VideoOutClose(int handle);
KYTY_SYSV_ABI int  VideoOutGetResolutionStatus(int handle, VideoOutResolutionStatus* status);
KYTY_SYSV_ABI void VideoOutSetBufferAttribute(VideoOutBufferAttribute* attribute, uint32_t pixel_format, uint32_t tiling_mode,
                                              uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch_in_pixel);
KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute, uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option, uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color);
KYTY_SYSV_ABI int  VideoOutSetFlipRate(int handle, int rate);
KYTY_SYSV_ABI int  VideoOutAddFlipEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata);
KYTY_SYSV_ABI int  VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata);
KYTY_SYSV_ABI int  VideoOutDeleteVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle);
KYTY_SYSV_ABI int  VideoOutDeleteFlipEvent(LibKernel::EventQueue::KernelEqueue eq, int handle);
KYTY_SYSV_ABI int  VideoOutGetEventId(const LibKernel::EventQueue::KernelEvent* ev);
KYTY_SYSV_ABI int  VideoOutGetEventData(const LibKernel::EventQueue::KernelEvent* ev, uint64_t* data);
KYTY_SYSV_ABI int  VideoOutConfigureOutput(int handle);
KYTY_SYSV_ABI int  VideoOutInitializeOutputOptions(void* options);
KYTY_SYSV_ABI int  VideoOutIsOutputSupported(int handle, uint64_t mode, const void* options, const void* reserved_pointer,
                                               uint64_t reserved);
KYTY_SYSV_ABI int  VideoOutUnregisterBuffers(int handle, int attribute_index);
KYTY_SYSV_ABI int  VideoOutWaitVblank(int handle);
KYTY_SYSV_ABI int  VideoOutRegisterBuffers(int handle, int start_index, void* const* addresses, int buffer_num,
                                           const VideoOutBufferAttribute* attribute);
KYTY_SYSV_ABI int  VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start, const VideoOutBuffers* buffers,
                                            int buffer_num, const VideoOutBufferAttribute2* attribute, int category, void* option);
KYTY_SYSV_ABI int  VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
void               VideoOutSubmitFlipInternal(int handle, int index, int flip_mode, int64_t flip_arg);
KYTY_SYSV_ABI int  VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status);
// Returns flipPendingNum for Gen5 waiters (NID zgXifHT9ErY).
KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle);
KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status);
KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom);
// Gen5 color / output status (success HLE; no fabricated display modes).
struct VideoOutOutputStatus;
struct VideoOutColorSettings;
KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status);
KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma);
KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings);
// Accept after buffers registered; attribute change is host-side no-op.
KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index, const VideoOutBufferAttribute2* attribute);

void VideoOutBeginVblank();
void VideoOutEndVblank();
bool VideoOutFlipWindow(uint32_t micros);

} // namespace Kyty::Libs::VideoOut

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_ */
