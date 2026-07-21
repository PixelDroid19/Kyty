#include "Emulator/Graphics/VideoOut.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/InternalResolutionRuntime.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/VideoOutFlipLifecycleGate.h"
#include "Emulator/Graphics/VideoOutFlipQueueAdmissionGate.h"
#include "Emulator/Graphics/VideoOutHostAccessGate.h"
#include "Emulator/Graphics/VideoOutMaterializationGate.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Profiler.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
struct GraphicContext;
} // namespace Kyty::Libs::Graphics

namespace Kyty::Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

const char* VideoOutRegisteredHostExtentStatusName(VideoOutRegisteredHostExtentStatus status)
{
	switch (status)
	{
		case VideoOutRegisteredHostExtentStatus::Uniform: return "uniform";
		case VideoOutRegisteredHostExtentStatus::Unselected: return "unselected";
		case VideoOutRegisteredHostExtentStatus::NonUniform: return "non_uniform";
		case VideoOutRegisteredHostExtentStatus::InvalidArgument: return "invalid_argument";
		case VideoOutRegisteredHostExtentStatus::NoBuffers: return "no_buffers";
	}
	return "unknown";
}

namespace EventQueue = LibKernel::EventQueue;

namespace {

bool EopTraceEnabled()
{
	static const bool enabled = (std::getenv("KYTY_EOP_TRACE") != nullptr);
	return enabled;
}

bool VideoOutRangesOverlap(uint64_t lhs_address, uint64_t lhs_size, uint64_t rhs_address, uint64_t rhs_size)
{
	if (lhs_size == 0 || rhs_size == 0)
	{
		return false;
	}
	return lhs_address <= rhs_address ? rhs_address - lhs_address < lhs_size : lhs_address - rhs_address < rhs_size;
}

} // namespace

constexpr int VIDEO_OUT_EVENT_FLIP             = 0;
constexpr int VIDEO_OUT_EVENT_VBLANK           = 1;
constexpr int VIDEO_OUT_EVENT_PRE_VBLANK_START = 2;

struct VideoOutResolutionStatus
{
	uint32_t fullWidth        = 1280;
	uint32_t fullHeight       = 720;
	uint32_t paneWidth        = 1280;
	uint32_t paneHeight       = 720;
	uint64_t refreshRate      = 3;
	float    screenSizeInInch = 50;
	uint16_t flags            = 0;
	uint16_t reserved0        = 0;
	uint32_t reserved1[3]     = {0};
};

struct VideoOutBufferAttribute
{
	uint32_t pixel_format;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint32_t option;
	uint32_t reserved0;
	uint64_t reserved1;
};

struct VideoOutBufferAttribute2
{
	uint32_t reserved0;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint64_t option;
	uint64_t pixel_format;
	uint64_t dcc_cb_register_clear_color;
	uint32_t dcc_control;
	uint32_t pad0;
	uint64_t reserved1[3];
};

struct VideoOutFlipStatus
{
	uint64_t count          = 0;
	uint64_t processTime    = 0;
	uint64_t tsc            = 0;
	int64_t  flipArg        = 0;
	uint64_t submitTsc      = 0;
	uint64_t reserved0      = 0;
	int32_t  gcQueueNum     = 0;
	int32_t  flipPendingNum = 0;
	int32_t  currentBuffer  = 0;
	uint32_t reserved1      = 0;
};

struct VideoOutVblankStatus
{
	uint64_t count       = 0;
	uint64_t processTime = 0;
	uint64_t tsc         = 0;
	uint64_t reserved[1] = {0};
	uint8_t  flags       = 0;
	uint8_t  pad1[7]     = {};
};

struct VideoOutBuffers
{
	const void* data;
	const void* metadata;
	const void* reserved[2];
};

union VideoOutBufferAttributeUnion
{
	VideoOutBufferAttribute  gen4;
	VideoOutBufferAttribute2 gen5;
};

struct VideoOutBufferSet
{
	VideoOutBufferAttributeUnion attr;

	bool gen5        = false;
	int  start_index = 0;
	int  num         = 0;
	int  set_id      = 0;
};

struct VideoOutConfig;

struct VideoOutBufferInfo
{
	const void*                    buffer        = nullptr;
	Graphics::VideoOutVulkanImage* buffer_vulkan = nullptr;
	uint64_t                       buffer_size   = 0;
	uint64_t                       buffer_pitch  = 0;
	uint32_t                       guest_width   = 0;
	uint32_t                       guest_height  = 0;
	uint32_t                       host_width    = 0;
	uint32_t                       host_height   = 0;
	int                            set_id        = 0;
};

struct VideoOutRegisteredImageSnapshot
{
	VideoOutConfig* config       = nullptr;
	const void*     buffer       = nullptr;
	uint64_t        buffer_size  = 0;
	uint64_t        buffer_pitch = 0;
	uint32_t        guest_width  = 0;
	uint32_t        guest_height = 0;
	uint32_t        host_width   = 0;
	uint32_t        host_height  = 0;
	int             slot         = -1;
	int             set_id       = 0;
	int             relative_index = -1;
};

enum class VideoOutEventKind : uint8_t
{
	Flip,
	Vblank,
};

struct VideoOutEventBinding
{
	VideoOutConfig*                    config    = nullptr;
	EventQueue::KernelEqueueIdentity   identity {};
	VideoOutEventKind                  kind      = VideoOutEventKind::Flip;
	bool                               published = false;
	bool                               deleted   = false;
};

struct VideoOutConfig
{
	Core::Mutex                      mutex;
	VideoOutResolutionStatus         resolution;
	bool                             opened    = false;
	bool                             closing   = false;
	int                              flip_rate = 0;
	Vector<VideoOutEventBinding*>    flip_events;
	Vector<VideoOutEventBinding*>    vblank_events;
	Vector<VideoOutEventBinding*>    event_bindings;
	VideoOutFlipStatus               flip_status;
	VideoOutVblankStatus             pre_vblank_status;
	VideoOutVblankStatus             vblank_status;
	VideoOutBufferInfo               buffers[16];
	bool                             buffer_registration_reserved[16] {};
	Vector<VideoOutBufferSet>        buffers_sets;
	int                              buffers_sets_seq = 0;
};

class FlipQueue
{
public:
	FlipQueue(): m_admission_gate(2) { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FlipQueue() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FlipQueue);

	bool Submit(VideoOutConfig* cfg, int index, int64_t flip_arg);
	void ReserveInternal(VideoOutConfig* cfg);
	void SubmitReservedBlocking(VideoOutConfig* cfg, int index, int64_t flip_arg);
	bool Flip(uint32_t micros);
	void GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out);
	void Wait(VideoOutConfig* cfg, int index);
	void WaitForConfig(VideoOutConfig* cfg);

private:
	struct Request
	{
		VideoOutConfig* cfg;
		int             index;
		int64_t         flip_arg;
		uint64_t        submit_tsc;
	};

	void Enqueue(VideoOutConfig* cfg, int index, int64_t flip_arg, bool accept_lifecycle);

	Core::Mutex                              m_mutex;
	Core::CondVar                            m_submit_cond_var;
	Core::CondVar                            m_done_cond_var;
	Core::List<Request>                      m_requests;
	Graphics::VideoOutFlipQueueAdmissionGate m_admission_gate;
	Graphics::VideoOutFlipLifecycleGate      m_lifecycle_gate;
};

enum class SubmitFlipStatus
{
	Submitted,
	InvalidHandle,
	InvalidIndex,
	QueueFull,
};

[[nodiscard]] static const char* SubmitFlipStatusName(SubmitFlipStatus status)
{
	switch (status)
	{
		case SubmitFlipStatus::Submitted: return "submitted";
		case SubmitFlipStatus::InvalidHandle: return "invalid_handle";
		case SubmitFlipStatus::InvalidIndex: return "invalid_index";
		case SubmitFlipStatus::QueueFull: return "queue_full";
	}
	return "unknown";
}

class VideoOutContext
{
public:
	static constexpr int VIDEO_OUT_NUM_MAX = 2;

	class SessionAccess final
	{
	public:
		SessionAccess() = default;
		SessionAccess(SessionAccess&&) noexcept            = default;
		SessionAccess& operator=(SessionAccess&&) noexcept = default;
		~SessionAccess()                                   = default;

		SessionAccess(const SessionAccess&)            = delete;
		SessionAccess& operator=(const SessionAccess&) = delete;

		[[nodiscard]] VideoOutConfig* Get() const { return m_config; }
		[[nodiscard]] explicit operator bool() const { return m_config != nullptr; }

	private:
		friend class VideoOutContext;
		SessionAccess(Graphics::VideoOutHostAccessGate::AccessPin access, VideoOutConfig* config)
		    : m_access(std::move(access)), m_config(config)
		{
		}

		Graphics::VideoOutHostAccessGate::AccessPin m_access;
		VideoOutConfig*                             m_config = nullptr;
	};

	VideoOutContext() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~VideoOutContext() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(VideoOutContext);

	int             Open();
	void            Close(int handle);
	SessionAccess   AcquireSession(int handle);

	VideoOutBufferImageInfo            FindImageForSubmission(const void* buffer, Graphics::CommandBuffer* command_buffer,
	                                                          bool materialize);
	Graphics::VideoOutVulkanImage*     MaterializeRegisteredImage(VideoOutConfig* cfg, int index, Graphics::SubmissionId submission);
	VideoOutRegisteredHostExtentStatus GetRegisteredHostExtent(Graphics::CommandBuffer* buffer, uint32_t guest_width,
	                                                           uint32_t guest_height, uint32_t* host_width, uint32_t* host_height);
	int RegisterBuffers(int handle, int set_id, bool generate_set_id, int start_index, const void* const* addresses, int buffer_num,
	                    const VideoOutBufferAttribute* attribute, const VideoOutBufferAttribute2* attribute2);
	SubmitFlipStatus SubmitFlip(int handle, int index, int64_t flip_arg);
	SubmitFlipStatus SubmitFlipInternal(int handle, int index, int64_t flip_arg);
	bool             RunBufferUnmapTransaction(uint64_t vaddr, uint64_t size, VideoOutQuiescedAction action, void* data);

	void Init(uint32_t width, uint32_t height);

	Graphics::GraphicContext* GetGraphicCtx()
	{
		Core::LockGuard lock(m_mutex);

		if (m_graphic_ctx == nullptr)
		{
			m_graphic_ctx = Graphics::WindowGetGraphicContext();
		}

		return m_graphic_ctx;
	}

	FlipQueue& GetFlipQueue() { return m_flip_queue; }

	void VblankBegin();
	void VblankEnd();

private:
	[[nodiscard]] int ResolveHandleLocked(int handle) const;
	void              DetachRegisteredBuffersForUnmapLocked(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool CaptureImageLocked(const void* buffer, VideoOutRegisteredImageSnapshot* snapshot);
	[[nodiscard]] bool CaptureRegisteredImageLocked(VideoOutConfig* cfg, int index,
	                                                VideoOutRegisteredImageSnapshot* snapshot);
	[[nodiscard]] bool RegisteredImageMatchesLocked(const VideoOutRegisteredImageSnapshot& snapshot) const;
	[[nodiscard]] Graphics::VideoOutVulkanImage*
	ResolveRegisteredImageForSubmission(const VideoOutRegisteredImageSnapshot& snapshot, Graphics::SubmissionId submission);
	[[nodiscard]] VideoOutBufferImageInfo
	PinImageForSubmission(const void* buffer, Graphics::SubmissionId submission, Graphics::VideoOutMaterializationGate::Pin* pin);
	[[nodiscard]] Graphics::VideoOutVulkanImage*
	PinRegisteredImageForSubmission(VideoOutConfig* cfg, int index, Graphics::SubmissionId submission,
	                                Graphics::VideoOutMaterializationGate::Pin* pin);

	Core::Mutex                           m_mutex;
	Core::Mutex                           m_registration_mutex;
	Graphics::VideoOutHostAccessGate      m_host_access_gate;
	Graphics::VideoOutMaterializationGate m_registration_gate;
	Graphics::VideoOutMaterializationGate m_materialization_gate;
	VideoOutConfig                        m_video_out_ctx[VIDEO_OUT_NUM_MAX];
	Graphics::GraphicContext*             m_graphic_ctx = nullptr;
	FlipQueue                             m_flip_queue;
};

static VideoOutContext* g_video_out_context = nullptr;
static std::atomic_uint64_t g_present_submission_sequence {0};

[[nodiscard]] static Graphics::SubmissionId NextPresentSubmission()
{
	const auto sequence = g_present_submission_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	EXIT_IF(sequence == 0);
	return {Graphics::GpuQueueId(static_cast<uint32_t>(Graphics::GraphicContext::QUEUE_PRESENT)), sequence};
}

static void calc_buffer_size(const VideoOutBufferAttribute* attribute, const VideoOutBufferAttribute2* attribute2, uint64_t* out_size,
                             uint64_t* out_align, uint64_t* out_pitch)
{
	EXIT_IF(out_size == nullptr);
	EXIT_IF(out_pitch == nullptr);
	EXIT_IF((attribute == nullptr && attribute2 == nullptr) || (attribute != nullptr && attribute2 != nullptr));

	bool     tile   = (attribute2 != nullptr ? (attribute2->tiling_mode == 0) : (attribute->tiling_mode == 0));
	bool     neo    = (attribute2 != nullptr ? true : Config::IsNeo());
	uint32_t width  = (attribute2 != nullptr ? attribute2->width : attribute->width);
	uint32_t height = (attribute2 != nullptr ? attribute2->height : attribute->height);
	uint32_t pitch  = (attribute2 != nullptr ? attribute2->width : attribute->pitch_in_pixel);

	if (attribute2 != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(attribute2->option != 0 && attribute2->option != 8);
		EXIT_NOT_IMPLEMENTED(attribute2->aspect_ratio != 0);
		// Gen5 PIXEL_FORMAT2: 8:8:8:8 sRGB and 10:10:10:2 (B/R channel order variants).
		EXIT_NOT_IMPLEMENTED(attribute2->pixel_format != 0x8000000000000000ULL && attribute2->pixel_format != 0x8000000022000000ULL &&
		                     attribute2->pixel_format != 0x8100000000000000ULL && attribute2->pixel_format != 0x8100000022000000ULL);
	} else
	{
		EXIT_NOT_IMPLEMENTED(attribute->option != 0);
		EXIT_NOT_IMPLEMENTED(attribute->aspect_ratio != 0);
		EXIT_NOT_IMPLEMENTED(attribute->pixel_format != 0x80000000 && attribute->pixel_format != 0x80002200);
	}

	Graphics::TileSizeAlign size32 {};
	Graphics::TileGetVideoOutSize(width, height, pitch, tile, neo, &size32);

	*out_size  = size32.size;
	*out_align = size32.align;
	*out_pitch = pitch;
}

void VideoOutInit(uint32_t width, uint32_t height)
{
	EXIT_IF(g_video_out_context != nullptr);

	g_video_out_context = new VideoOutContext;

	g_video_out_context->Init(width, height);
}

void VideoOutContext::Init(uint32_t width, uint32_t height)
{
	for (auto& ctx: m_video_out_ctx)
	{
		ctx.resolution.fullWidth  = width;
		ctx.resolution.fullHeight = height;
		ctx.resolution.paneWidth  = width;
		ctx.resolution.paneHeight = height;
	}
}

int VideoOutContext::Open()
{
	auto            access = m_host_access_gate.Acquire();
	Core::LockGuard lock(m_mutex);

	int handle = -1;

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
	{
		if (!m_video_out_ctx[i].opened && !m_video_out_ctx[i].closing)
		{
			handle = i;
			break;
		}
	}

	if (handle < 0)
	{
		return -1;
	}
	EXIT_IF(m_video_out_ctx[handle].closing);
	EXIT_IF(!m_video_out_ctx[handle].flip_events.IsEmpty());
	EXIT_IF(!m_video_out_ctx[handle].vblank_events.IsEmpty());
	EXIT_IF(!m_video_out_ctx[handle].event_bindings.IsEmpty());
	EXIT_IF(m_video_out_ctx[handle].flip_rate != 0);

	m_video_out_ctx[handle].opened                    = true;
	m_video_out_ctx[handle].flip_status               = VideoOutFlipStatus();
	m_video_out_ctx[handle].flip_status.flipArg       = -1;
	m_video_out_ctx[handle].flip_status.currentBuffer = -1;
	m_video_out_ctx[handle].flip_status.count         = 0;
	m_video_out_ctx[handle].pre_vblank_status         = VideoOutVblankStatus();
	m_video_out_ctx[handle].vblank_status             = VideoOutVblankStatus();

	return handle;
}

void VideoOutContext::Close(int handle)
{
	auto quiesce = m_host_access_gate.Quiesce();

	m_mutex.Lock();

	EXIT_NOT_IMPLEMENTED(handle < 0 || handle >= VIDEO_OUT_NUM_MAX);
	auto* ctx = m_video_out_ctx + handle;
	EXIT_NOT_IMPLEMENTED(!ctx->opened || ctx->closing);

	ctx->closing = true;
	m_mutex.Unlock();

	m_flip_queue.WaitForConfig(ctx);
	m_registration_gate.WaitUntilIdle();

	m_mutex.Lock();
	EXIT_IF(!ctx->opened || !ctx->closing);
	ctx->opened = false;
	m_mutex.Unlock();

	m_materialization_gate.WaitUntilIdle();

	m_mutex.Lock();
	EXIT_IF(ctx->opened || !ctx->closing);
	m_mutex.Unlock();

	struct PendingEventDelete
	{
		EventQueue::KernelEqueuePin pin;
		uintptr_t                   ident = 0;
	};
	std::vector<PendingEventDelete>       event_deletes;
	std::vector<EventQueue::KernelEqueueIdentity> closing_queues;
	{
		Core::LockGuard config_lock(ctx->mutex);
		event_deletes.reserve(ctx->event_bindings.Size());
		closing_queues.reserve(ctx->event_bindings.Size());
		for (auto* binding: ctx->event_bindings)
		{
			EXIT_IF(binding == nullptr || binding->deleted || !binding->published);
			auto pin = EventQueue::KernelAcquireEqueue(binding->identity);
			if (pin)
			{
				const uintptr_t ident =
				    binding->kind == VideoOutEventKind::Flip ? VIDEO_OUT_EVENT_FLIP : VIDEO_OUT_EVENT_VBLANK;
				event_deletes.push_back(PendingEventDelete {std::move(pin), ident});
			} else
			{
				closing_queues.push_back(binding->identity);
			}
		}
	}

	for (auto& pending: event_deletes)
	{
		const auto result =
		    EventQueue::KernelDeleteEvent(pending.pin, pending.ident, EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
	}
	event_deletes.clear();
	for (const auto identity: closing_queues)
	{
		EventQueue::KernelWaitEqueueClosed(identity);
	}

	{
		Core::LockGuard config_lock(ctx->mutex);
		EXIT_IF(!ctx->event_bindings.IsEmpty());
		EXIT_IF(!ctx->flip_events.IsEmpty());
		EXIT_IF(!ctx->vblank_events.IsEmpty());
	}

	m_mutex.Lock();
	EXIT_IF(ctx->opened || !ctx->closing);
	ctx->flip_rate = 0;

	for (auto& buffer: ctx->buffers)
	{
		buffer = {};
	}
	for (bool reserved: ctx->buffer_registration_reserved)
	{
		EXIT_IF(reserved);
	}

	ctx->buffers_sets.Clear();

	ctx->buffers_sets_seq = 0;
	ctx->closing          = false;
	m_mutex.Unlock();
}

VideoOutContext::SessionAccess VideoOutContext::AcquireSession(int handle)
{
	auto            access = m_host_access_gate.Acquire();
	Core::LockGuard lock(m_mutex);
	handle = ResolveHandleLocked(handle);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX)
	{
		return {};
	}
	auto* ctx = m_video_out_ctx + handle;
	if (!ctx->opened || ctx->closing)
	{
		return {};
	}

	return {std::move(access), ctx};
}

SubmitFlipStatus VideoOutContext::SubmitFlip(int handle, int index, int64_t flip_arg)
{
	auto            access = m_host_access_gate.Acquire();
	Core::LockGuard lock(m_mutex);
	handle = ResolveHandleLocked(handle);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX)
	{
		return SubmitFlipStatus::InvalidHandle;
	}

	auto* ctx = m_video_out_ctx + handle;
	if (!ctx->opened || ctx->closing)
	{
		return SubmitFlipStatus::InvalidHandle;
	}
	if (index < 0 || index >= 16 || ctx->buffer_registration_reserved[index] || ctx->buffers[index].buffer == nullptr)
	{
		return SubmitFlipStatus::InvalidIndex;
	}
	return m_flip_queue.Submit(ctx, index, flip_arg) ? SubmitFlipStatus::Submitted : SubmitFlipStatus::QueueFull;
}

SubmitFlipStatus VideoOutContext::SubmitFlipInternal(int handle, int index, int64_t flip_arg)
{
	auto            access = m_host_access_gate.Acquire();
	VideoOutConfig* ctx = nullptr;
	{
		Core::LockGuard lock(m_mutex);
		handle = ResolveHandleLocked(handle);
		if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX)
		{
			return SubmitFlipStatus::InvalidHandle;
		}

		ctx = m_video_out_ctx + handle;
		if (!ctx->opened || ctx->closing)
		{
			return SubmitFlipStatus::InvalidHandle;
		}
		if (index < 0 || index >= 16 || ctx->buffer_registration_reserved[index] || ctx->buffers[index].buffer == nullptr)
		{
			return SubmitFlipStatus::InvalidIndex;
		}

		// Reserve the context lifetime while the internal producer waits for queue
		// capacity. Close marks the context as closing under m_mutex, then waits for
		// every reservation, so the pointer remains valid after this lock is released.
		m_flip_queue.ReserveInternal(ctx);
	}

	m_flip_queue.SubmitReservedBlocking(ctx, index, flip_arg);
	return SubmitFlipStatus::Submitted;
}

int VideoOutContext::ResolveHandleLocked(int handle) const
{
	bool opened[VIDEO_OUT_NUM_MAX] = {};
	for (int i = 0; i < VIDEO_OUT_NUM_MAX; i++)
	{
		opened[i] = m_video_out_ctx[i].opened;
	}
	return VideoOutResolveHandle(handle, opened, VIDEO_OUT_NUM_MAX);
}

int VideoOutResolveHandle(int handle, const bool* opened, int num_slots)
{
	if (handle != 0 || opened == nullptr || num_slots <= 0)
	{
		return handle;
	}
	int sole = -1;
	for (int i = 0; i < num_slots; i++)
	{
		if (opened[i])
		{
			if (sole >= 0)
			{
				return 0; // ambiguous
			}
			sole = i;
		}
	}
	return sole >= 0 ? sole : 0;
}

void VideoOutContext::VblankBegin()
{
	auto access = m_host_access_gate.Acquire();

	VideoOutConfig* opened[VIDEO_OUT_NUM_MAX] {};
	uint32_t        opened_num = 0;
	{
		Core::LockGuard lock(m_mutex);
		for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
		{
			auto& ctx = m_video_out_ctx[i];
			if (ctx.opened && !ctx.closing)
			{
				opened[opened_num++] = &ctx;
			}
		}
	}

	for (uint32_t index = 0; index < opened_num; index++)
	{
		auto*    ctx   = opened[index];
		uint64_t count = 0;
		{
			Core::LockGuard config_lock(ctx->mutex);
			ctx->pre_vblank_status.count++;
			ctx->pre_vblank_status.processTime = LibKernel::KernelGetProcessTime();
			ctx->pre_vblank_status.tsc         = LibKernel::KernelReadTsc();
			count                              = ctx->pre_vblank_status.count;
		}
		(void)count;
	}
}

void VideoOutContext::VblankEnd()
{
	auto access = m_host_access_gate.Acquire();

	VideoOutConfig* opened[VIDEO_OUT_NUM_MAX] {};
	uint32_t        opened_num = 0;
	{
		Core::LockGuard lock(m_mutex);
		for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
		{
			auto& ctx = m_video_out_ctx[i];
			if (ctx.opened && !ctx.closing)
			{
				opened[opened_num++] = &ctx;
			}
		}
	}

	for (uint32_t index = 0; index < opened_num; index++)
	{
		auto*                                  ctx = opened[index];
		std::vector<EventQueue::KernelEqueuePin> queues;
		uint64_t                               count = 0;
		{
			Core::LockGuard config_lock(ctx->mutex);
			ctx->vblank_status.count++;
			ctx->vblank_status.processTime = LibKernel::KernelGetProcessTime();
			ctx->vblank_status.tsc         = LibKernel::KernelReadTsc();
			count                          = ctx->vblank_status.count;
			queues.reserve(ctx->vblank_events.Size());
			for (auto* binding: ctx->vblank_events)
			{
				auto pin = EventQueue::KernelAcquireEqueue(binding->identity);
				if (pin)
				{
					queues.push_back(std::move(pin));
				}
			}
		}
		for (auto& pin: queues)
		{
			const auto result =
			    EventQueue::KernelTriggerEvent(pin, VIDEO_OUT_EVENT_VBLANK, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
			                                   reinterpret_cast<void*>(count));
			EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
		}
	}
}

bool VideoOutContext::CaptureRegisteredImageLocked(VideoOutConfig* cfg, int index,
                                                   VideoOutRegisteredImageSnapshot* snapshot)
{
	bool registered_config = false;
	for (auto& context: m_video_out_ctx)
	{
		registered_config = registered_config || &context == cfg;
	}
	if (!registered_config || snapshot == nullptr || index < 0 || index >= 16 || !cfg->opened || cfg->closing ||
	    cfg->buffer_registration_reserved[index])
	{
		return false;
	}

	const auto& registered = cfg->buffers[index];
	if (registered.buffer == nullptr || registered.buffer_size == 0 || registered.host_width == 0 || registered.host_height == 0)
	{
		return false;
	}

	int relative_index = -1;
	for (const auto& set: cfg->buffers_sets)
	{
		if (index >= set.start_index && index < set.start_index + set.num)
		{
			relative_index = index - set.start_index;
			break;
		}
	}
	if (relative_index < 0)
	{
		return false;
	}

	*snapshot = {
	    cfg,
	    registered.buffer,
	    registered.buffer_size,
	    registered.buffer_pitch,
	    registered.guest_width,
	    registered.guest_height,
	    registered.host_width,
	    registered.host_height,
	    index,
	    registered.set_id,
	    relative_index,
	};
	return true;
}

bool VideoOutContext::CaptureImageLocked(const void* buffer, VideoOutRegisteredImageSnapshot* snapshot)
{
	if (buffer == nullptr || snapshot == nullptr)
	{
		return false;
	}

	for (auto& ctx: m_video_out_ctx)
	{
		if (!ctx.opened || ctx.closing)
		{
			continue;
		}
		for (const auto& set: ctx.buffers_sets)
		{
			for (int slot = set.start_index; slot < set.start_index + set.num; slot++)
			{
				if (ctx.buffers[slot].buffer == buffer)
				{
					return CaptureRegisteredImageLocked(&ctx, slot, snapshot);
				}
			}
		}
	}
	return false;
}

bool VideoOutContext::RegisteredImageMatchesLocked(const VideoOutRegisteredImageSnapshot& snapshot) const
{
	if (snapshot.config == nullptr || snapshot.slot < 0 || snapshot.slot >= 16 || !snapshot.config->opened ||
	    snapshot.config->closing || snapshot.config->buffer_registration_reserved[snapshot.slot])
	{
		return false;
	}

	const auto& registered = snapshot.config->buffers[snapshot.slot];
	return registered.buffer == snapshot.buffer && registered.buffer_size == snapshot.buffer_size &&
	       registered.buffer_pitch == snapshot.buffer_pitch && registered.guest_width == snapshot.guest_width &&
	       registered.guest_height == snapshot.guest_height && registered.host_width == snapshot.host_width &&
	       registered.host_height == snapshot.host_height && registered.set_id == snapshot.set_id;
}

Graphics::VideoOutVulkanImage*
VideoOutContext::ResolveRegisteredImageForSubmission(const VideoOutRegisteredImageSnapshot& snapshot,
                                                     Graphics::SubmissionId submission)
{
	const auto objects = Graphics::GpuMemoryFindObjectsForSubmission(
	    submission, reinterpret_cast<uint64_t>(snapshot.buffer), snapshot.buffer_size,
	    Graphics::GpuMemoryObjectType::VideoOutBuffer, true, true);
	EXIT_NOT_IMPLEMENTED(objects.Size() != 1 || objects[0].obj == nullptr);

	auto* image = static_cast<Graphics::VideoOutVulkanImage*>(objects[0].obj);
	if (!image->MatchesGuestExtent(snapshot.guest_width, snapshot.guest_height))
	{
		EXIT("Resolved VideoOut backing has incompatible guest extent: expected=%ux%u actual=%ux%u\n", snapshot.guest_width,
		     snapshot.guest_height, image->guest_extent.width, image->guest_extent.height);
	}

	Graphics::VideoOutVulkanImage* refreshed_cache = nullptr;
	const auto refresh = Graphics::VideoOutBufferRefreshPublishedImage(image, snapshot.host_width, snapshot.host_height,
	                                                                   &refreshed_cache);
	switch (refresh)
	{
		case Graphics::VideoOutPublishedImageRefreshStatus::Published: break;
		case Graphics::VideoOutPublishedImageRefreshStatus::ExtentConflict:
			EXIT("Resolved VideoOut backing rejected registered host extent: expected=%ux%u\n", snapshot.host_width,
			     snapshot.host_height);
		case Graphics::VideoOutPublishedImageRefreshStatus::InvalidArgument:
			EXIT("Resolved VideoOut backing refresh received invalid state: expected=%ux%u\n", snapshot.host_width,
			     snapshot.host_height);
	}

	Core::LockGuard lock(m_mutex);
	if (!RegisteredImageMatchesLocked(snapshot))
	{
		return nullptr;
	}
	snapshot.config->buffers[snapshot.slot].buffer_vulkan = refreshed_cache;
	return refreshed_cache;
}

VideoOutBufferImageInfo VideoOutContext::FindImageForSubmission(const void* buffer, Graphics::CommandBuffer* command_buffer,
                                                                bool materialize)
{
	auto access = m_host_access_gate.Acquire();
	EXIT_IF(command_buffer == nullptr);
	auto* graphic_ctx = GetGraphicCtx();
	Graphics::SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!command_buffer->GetSubmissionId(&submission));

	Graphics::VideoOutMaterializationGate::Pin pin;
	auto                                       ret = PinImageForSubmission(buffer, submission, &pin);
	if (materialize && ret.image != nullptr)
	{
		Graphics::VideoOutBufferEnsureMaterialized(graphic_ctx, ret.image);
	}
	return ret;
}

Graphics::VideoOutVulkanImage* VideoOutContext::MaterializeRegisteredImage(VideoOutConfig* cfg, int index,
                                                                           Graphics::SubmissionId submission)
{
	EXIT_IF(cfg == nullptr);
	EXIT_IF(index < 0 || index >= 16);
	auto* graphic_ctx = GetGraphicCtx();

	Graphics::VideoOutMaterializationGate::Pin pin;
	auto*                                      image = PinRegisteredImageForSubmission(cfg, index, submission, &pin);
	Graphics::VideoOutBufferEnsureMaterialized(graphic_ctx, image);
	return image;
}

VideoOutBufferImageInfo VideoOutContext::PinImageForSubmission(const void* buffer, Graphics::SubmissionId submission,
                                                               Graphics::VideoOutMaterializationGate::Pin* pin)
{
	EXIT_IF(pin == nullptr);
	VideoOutRegisteredImageSnapshot snapshot;
	{
		Core::LockGuard lock(m_mutex);
		if (!CaptureImageLocked(buffer, &snapshot))
		{
			return {};
		}
		*pin = m_materialization_gate.Acquire();
	}

	auto* image = ResolveRegisteredImageForSubmission(snapshot, submission);
	EXIT_IF(image == nullptr);
	return {image, static_cast<uint32_t>(snapshot.relative_index), snapshot.buffer_size, snapshot.buffer_pitch};
}

Graphics::VideoOutVulkanImage*
VideoOutContext::PinRegisteredImageForSubmission(VideoOutConfig* cfg, int index, Graphics::SubmissionId submission,
                                                 Graphics::VideoOutMaterializationGate::Pin* pin)
{
	EXIT_IF(pin == nullptr);
	VideoOutRegisteredImageSnapshot snapshot;
	{
		Core::LockGuard lock(m_mutex);
		if (!CaptureRegisteredImageLocked(cfg, index, &snapshot))
		{
			EXIT("VideoOut registered image is no longer available: index=%d\n", index);
		}
		*pin = m_materialization_gate.Acquire();
	}

	auto* image = ResolveRegisteredImageForSubmission(snapshot, submission);
	EXIT_IF(image == nullptr);
	return image;
}

VideoOutRegisteredHostExtentStatus VideoOutContext::GetRegisteredHostExtent(Graphics::CommandBuffer* buffer, uint32_t guest_width,
                                                                            uint32_t guest_height, uint32_t* host_width,
                                                                            uint32_t* host_height)
{
	auto access = m_host_access_gate.Acquire();
	if (buffer == nullptr || guest_width == 0 || guest_height == 0 || host_width == nullptr || host_height == nullptr)
	{
		return VideoOutRegisteredHostExtentStatus::InvalidArgument;
	}
	Graphics::SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));

	VideoOutRegisteredImageSnapshot snapshots[VIDEO_OUT_NUM_MAX * 16] {};
	uint32_t                        snapshot_count = 0;
	Graphics::VideoOutMaterializationGate::Pin pin;
	{
		Core::LockGuard lock(m_mutex);
		pin = m_materialization_gate.Acquire();
		for (auto& ctx: m_video_out_ctx)
		{
			if (!ctx.opened || ctx.closing)
			{
				continue;
			}
			for (int slot = 0; slot < 16; slot++)
			{
				const auto& registered = ctx.buffers[slot];
				if (registered.buffer == nullptr || registered.guest_width != guest_width ||
				    registered.guest_height != guest_height)
				{
					continue;
				}
				EXIT_IF(snapshot_count >= VIDEO_OUT_NUM_MAX * 16);
				EXIT_IF(!CaptureRegisteredImageLocked(&ctx, slot, &snapshots[snapshot_count]));
				snapshot_count++;
			}
		}
	}

	Graphics::VideoOutVulkanImage* images[VIDEO_OUT_NUM_MAX * 16] {};
	uint32_t                       image_count = 0;
	for (uint32_t i = 0; i < snapshot_count; i++)
	{
		auto* image = ResolveRegisteredImageForSubmission(snapshots[i], submission);
		EXIT_IF(image == nullptr);
		images[image_count++] = image;
	}

	if (image_count == 0)
	{
		return VideoOutRegisteredHostExtentStatus::NoBuffers;
	}

	Graphics::VideoOutHostExtentSetState state;
	switch (Graphics::VideoOutBufferInspectHostExtentSet(images, image_count, &state))
	{
		case Graphics::VideoOutHostExtentSetInspectionStatus::Uniform:
			*host_width  = state.width;
			*host_height = state.height;
			return VideoOutRegisteredHostExtentStatus::Uniform;
		case Graphics::VideoOutHostExtentSetInspectionStatus::Unselected: return VideoOutRegisteredHostExtentStatus::Unselected;
		case Graphics::VideoOutHostExtentSetInspectionStatus::NonUniform:
			*host_width  = state.width;
			*host_height = state.height;
			return VideoOutRegisteredHostExtentStatus::NonUniform;
		case Graphics::VideoOutHostExtentSetInspectionStatus::InvalidArgument: return VideoOutRegisteredHostExtentStatus::InvalidArgument;
		case Graphics::VideoOutHostExtentSetInspectionStatus::Empty: return VideoOutRegisteredHostExtentStatus::NoBuffers;
	}
	return VideoOutRegisteredHostExtentStatus::InvalidArgument;
}

bool FlipQueue::Submit(VideoOutConfig* cfg, int index, int64_t flip_arg)
{
	if (!m_admission_gate.TryAcquire())
	{
		return false;
	}

	Enqueue(cfg, index, flip_arg, true);
	return true;
}

void FlipQueue::ReserveInternal(VideoOutConfig* cfg)
{
	EXIT_IF(cfg == nullptr);
	m_lifecycle_gate.Accept(cfg);
}

void FlipQueue::SubmitReservedBlocking(VideoOutConfig* cfg, int index, int64_t flip_arg)
{
	EXIT_IF(cfg == nullptr);
	m_admission_gate.AcquireBlocking();
	Enqueue(cfg, index, flip_arg, false);
}

void FlipQueue::Enqueue(VideoOutConfig* cfg, int index, int64_t flip_arg, bool accept_lifecycle)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(cfg == nullptr);
	EXIT_IF(m_requests.Size() >= 2);

	Request r {};
	r.cfg        = cfg;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.submit_tsc = LibKernel::KernelReadTsc();

	m_requests.Add(r);
	if (accept_lifecycle)
	{
		m_lifecycle_gate.Accept(cfg);
	}

	cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.Size());
	cfg->flip_status.gcQueueNum     = 0;

	m_submit_cond_var.Signal();
}

void FlipQueue::Wait(VideoOutConfig* cfg, int index)
{
	Core::LockGuard lock(m_mutex);

	while (
	    m_requests.IndexValid(m_requests.Find(cfg, index, [](auto r, auto cfg, auto index) { return r.cfg == cfg && r.index == index; })))
	{
		m_done_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::WaitForConfig(VideoOutConfig* cfg)
{
	EXIT_IF(cfg == nullptr);
	m_lifecycle_gate.WaitUntilIdle(cfg);
}

bool FlipQueue::Flip(uint32_t micros)
{
	KYTY_PROFILER_BLOCK("FlipQueue::Flip");

	m_mutex.Lock();
	if (m_requests.Size() == 0)
	{
		m_submit_cond_var.WaitFor(&m_mutex, micros);

		if (m_requests.Size() == 0)
		{
			m_mutex.Unlock();
			return false;
		}
	}
	auto first = m_requests.First();
	auto r     = m_requests.At(first);
	m_mutex.Unlock();

	const auto present_submission = NextPresentSubmission();
	auto*      buffer = g_video_out_context->MaterializeRegisteredImage(r.cfg, r.index, present_submission);

	Graphics::WindowDrawBuffer(buffer);
	Graphics::GpuMemoryCompleteSubmission(present_submission);

	std::vector<EventQueue::KernelEqueuePin> flip_queues;
	{
		Core::LockGuard config_lock(r.cfg->mutex);
		flip_queues.reserve(r.cfg->flip_events.Size());
		for (auto* binding: r.cfg->flip_events)
		{
			auto pin = EventQueue::KernelAcquireEqueue(binding->identity);
			if (pin)
			{
				flip_queues.push_back(std::move(pin));
			}
		}
	}

	uint32_t flip_triggered = 0;
	for (auto& pin: flip_queues)
	{
		const auto result =
		    EventQueue::KernelTriggerEvent(pin, VIDEO_OUT_EVENT_FLIP, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
		                                   reinterpret_cast<void*>(r.flip_arg));
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
		if (result == OK)
		{
			flip_triggered++;
		}
	}
	if (EopTraceEnabled())
	{
		static std::atomic<uint32_t> flip_logs {0};
		const uint32_t               n = flip_logs.fetch_add(1);
		if (n < 32u)
		{
			std::fprintf(stderr, "FLIP_TRIGGER index=%d arg=%" PRId64 " eqs=%u\n", r.index, r.flip_arg, flip_triggered);
		}
	}

	printf("Flip done: %d\n", r.index);

	m_mutex.Lock();

	r.cfg->flip_status.count++;
	r.cfg->flip_status.processTime    = LibKernel::KernelGetProcessTime();
	r.cfg->flip_status.tsc            = LibKernel::KernelReadTsc();
	r.cfg->flip_status.submitTsc      = r.submit_tsc;
	r.cfg->flip_status.flipArg        = r.flip_arg;
	r.cfg->flip_status.currentBuffer  = r.index;
	r.cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.Size() - 1);

	m_requests.Remove(first);
	m_done_cond_var.SignalAll();
	m_lifecycle_gate.Complete(r.cfg);

	m_mutex.Unlock();
	m_admission_gate.Release();

	Graphics::GpuMemoryFrameDone();
	Graphics::GpuMemoryDbgDump();

	return true;
}

void FlipQueue::GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out)
{
	EXIT_IF(cfg == nullptr);
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	*out = cfg->flip_status;
}

bool VideoOutFlipWindow(uint32_t micros)
{
	EXIT_IF(g_video_out_context == nullptr);

	return g_video_out_context->GetFlipQueue().Flip(micros);
}

void VideoOutBeginVblank()
{
	EXIT_IF(g_video_out_context == nullptr);

	// g_video_out_context->VblankBegin();
}

void VideoOutEndVblank()
{
	EXIT_IF(g_video_out_context == nullptr);

	g_video_out_context->VblankEnd();
}

KYTY_SYSV_ABI int VideoOutOpen(int user_id, int bus_type, int index, const void* param)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 0);
	EXIT_NOT_IMPLEMENTED(bus_type != 0);
	EXIT_NOT_IMPLEMENTED(index != 0);
	// Gen5 titles pass a non-null open-param block; attributes are applied later
	// via SetBufferAttribute*. Accept and ignore for Open().
	printf("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	int handle = g_video_out_context->Open();

	if (handle < 0)
	{
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}

	return handle;
}

KYTY_SYSV_ABI int VideoOutClose(int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	const bool closed = Graphics::GraphicsRunWithQuiescedSubmissions(
	    [](void* data)
	    {
		    EXIT_IF(data == nullptr);
		    g_video_out_context->Close(*static_cast<int*>(data));
		    return true;
	    },
	    &handle);
	EXIT_IF(!closed);

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetResolutionStatus(int handle, VideoOutResolutionStatus* status)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	EXIT_NOT_IMPLEMENTED(status == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	EXIT_NOT_IMPLEMENTED(!session);
	*status = session.Get()->resolution;

	return OK;
}

KYTY_SYSV_ABI void VideoOutSetBufferAttribute(VideoOutBufferAttribute* attribute, uint32_t pixel_format, uint32_t tiling_mode,
                                              uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch_in_pixel)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	printf("\t pixel_format   = %08" PRIx32 "\n", pixel_format);
	printf("\t tiling_mode    = %" PRIu32 "\n", tiling_mode);
	printf("\t aspect_ratio   = %" PRIu32 "\n", aspect_ratio);
	printf("\t width          = %" PRIu32 "\n", width);
	printf("\t height         = %" PRIu32 "\n", height);
	printf("\t pitch_in_pixel = %" PRIu32 "\n", pitch_in_pixel);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute));

	attribute->pixel_format   = pixel_format;
	attribute->tiling_mode    = tiling_mode;
	attribute->aspect_ratio   = aspect_ratio;
	attribute->width          = width;
	attribute->height         = height;
	attribute->pitch_in_pixel = pitch_in_pixel;
}

KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute, uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option, uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	printf("\t pixel_format                = %016" PRIx64 "\n", pixel_format);
	printf("\t tiling_mode                 = %" PRIu32 "\n", tiling_mode);
	printf("\t width                       = %" PRIu32 "\n", width);
	printf("\t height                      = %" PRIu32 "\n", height);
	printf("\t option                      = %016" PRIx64 "\n", option);
	printf("\t dcc_control                 = %08" PRIx32 "\n", dcc_control);
	printf("\t dcc_cb_register_clear_color = %016" PRIx64 "\n", dcc_cb_register_clear_color);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute2));

	attribute->tiling_mode                 = tiling_mode;
	attribute->aspect_ratio                = 0;
	attribute->width                       = width;
	attribute->height                      = height;
	attribute->pitch_in_pixel              = 0;
	attribute->option                      = option;
	attribute->pixel_format                = pixel_format;
	attribute->dcc_cb_register_clear_color = dcc_cb_register_clear_color;
	attribute->dcc_control                 = dcc_control;
}

KYTY_SYSV_ABI int VideoOutSetFlipRate(int handle, int rate)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	EXIT_NOT_IMPLEMENTED(rate < 0 || rate > 2);

	printf("\trate = %d\n", rate);

	auto session = g_video_out_context->AcquireSession(handle);
	EXIT_NOT_IMPLEMENTED(!session);
	session.Get()->flip_rate = rate;

	return OK;
}

static void flip_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void flip_event_delete_func(EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_FLIP);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	auto* binding = static_cast<VideoOutEventBinding*>(event->filter.data);
	EXIT_IF(binding->config == nullptr || binding->identity.eq != eq || binding->kind != VideoOutEventKind::Flip);
	auto* video_out = binding->config;
	VideoOutEventBinding* release = nullptr;
	{
		Core::LockGuard config_lock(video_out->mutex);
		EXIT_IF(binding->deleted);
		if (binding->published)
		{
			const auto active_index = video_out->flip_events.Find(binding);
			EXIT_IF(!video_out->flip_events.IndexValid(active_index));
			video_out->flip_events.RemoveAt(active_index);
			const auto binding_index = video_out->event_bindings.Find(binding);
			EXIT_IF(!video_out->event_bindings.IndexValid(binding_index));
			video_out->event_bindings.RemoveAt(binding_index);
			release = binding;
		}
		binding->published = false;
		binding->deleted   = true;
	}
	delete release;
}

static void flip_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

static void vblank_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void vblank_event_delete_func(EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_VBLANK);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	auto* binding = static_cast<VideoOutEventBinding*>(event->filter.data);
	EXIT_IF(binding->config == nullptr || binding->identity.eq != eq || binding->kind != VideoOutEventKind::Vblank);
	auto* video_out = binding->config;
	VideoOutEventBinding* release = nullptr;
	{
		Core::LockGuard config_lock(video_out->mutex);
		EXIT_IF(binding->deleted);
		if (binding->published)
		{
			const auto active_index = video_out->vblank_events.Find(binding);
			EXIT_IF(!video_out->vblank_events.IndexValid(active_index));
			video_out->vblank_events.RemoveAt(active_index);
			const auto binding_index = video_out->event_bindings.Find(binding);
			EXIT_IF(!video_out->event_bindings.IndexValid(binding_index));
			video_out->event_bindings.RemoveAt(binding_index);
			release = binding;
		}
		binding->published = false;
		binding->deleted   = true;
	}
	delete release;
}

static void vblank_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

KYTY_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();

	if (eq == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto eq_pin = EventQueue::KernelAcquireEqueue(eq);
	if (!eq_pin)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	auto* binding     = new VideoOutEventBinding;
	binding->config   = ctx;
	binding->identity = eq_pin.GetIdentity();
	binding->kind     = VideoOutEventKind::Flip;
	{
		Core::LockGuard config_lock(ctx->mutex);
		ctx->event_bindings.Add(binding);
	}

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_FLIP;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = flip_event_delete_func;
	event.filter.reset_func        = flip_event_reset_func;
	event.filter.trigger_func      = flip_event_trigger_func;
	event.filter.data              = binding;

	const int result = EventQueue::KernelAddEvent(eq_pin, event);

	unsigned flip_eq_count = 0;
	VideoOutEventBinding* release = nullptr;
	{
		Core::LockGuard config_lock(ctx->mutex);
		if (result == OK && !binding->deleted)
		{
			ctx->flip_events.Add(binding);
			binding->published = true;
		} else
		{
			const auto index = ctx->event_bindings.Find(binding);
			EXIT_IF(!ctx->event_bindings.IndexValid(index));
			ctx->event_bindings.RemoveAt(index);
			release = binding;
		}
		flip_eq_count = static_cast<unsigned>(ctx->flip_events.Size());
	}
	delete release;

	if (EopTraceEnabled())
	{
		std::fprintf(stderr, "FLIP_ADD handle=%d eq=%p count=%u\n", handle, static_cast<void*>(eq), flip_eq_count);
	}

	return result;
}

KYTY_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();

	if (eq == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto eq_pin = EventQueue::KernelAcquireEqueue(eq);
	if (!eq_pin)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	auto* binding     = new VideoOutEventBinding;
	binding->config   = ctx;
	binding->identity = eq_pin.GetIdentity();
	binding->kind     = VideoOutEventKind::Vblank;
	{
		Core::LockGuard config_lock(ctx->mutex);
		ctx->event_bindings.Add(binding);
	}

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_VBLANK;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = vblank_event_delete_func;
	event.filter.reset_func        = vblank_event_reset_func;
	event.filter.trigger_func      = vblank_event_trigger_func;
	event.filter.data              = binding;

	const int result = EventQueue::KernelAddEvent(eq_pin, event);

	VideoOutEventBinding* release = nullptr;
	{
		Core::LockGuard config_lock(ctx->mutex);
		if (result == OK && !binding->deleted)
		{
			ctx->vblank_events.Add(binding);
			binding->published = true;
		} else
		{
			const auto index = ctx->event_bindings.Find(binding);
			EXIT_IF(!ctx->event_bindings.IndexValid(index));
			ctx->event_bindings.RemoveAt(index);
			release = binding;
		}
	}
	delete release;

	return result;
}

int VideoOutContext::RegisterBuffers(int handle, int set_id, bool generate_set_id, int start_index, const void* const* addresses,
                                     int buffer_num, const VideoOutBufferAttribute* attribute, const VideoOutBufferAttribute2* attribute2)
{
	auto access = m_host_access_gate.Acquire();

	// Registration is a transaction, but its expensive GpuMemory/Vulkan work
	// must not block vblank, flip status, image lookup, or Close on m_mutex.
	// Reserve before any host wait so Close either observes this exact
	// transaction or wins the session linearization point.

	uint64_t buffer_size  = 0;
	uint64_t buffer_align = 0;
	uint64_t buffer_pitch = 0;
	calc_buffer_size(attribute, attribute2, &buffer_size, &buffer_align, &buffer_pitch);

	EXIT_NOT_IMPLEMENTED(buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(buffer_pitch == 0);

	bool     tile   = (attribute2 != nullptr ? (attribute2->tiling_mode == 0) : (attribute->tiling_mode == 0));
	bool     neo    = (attribute2 != nullptr ? true : Config::IsNeo());
	uint32_t width  = (attribute2 != nullptr ? attribute2->width : attribute->width);
	uint32_t height = (attribute2 != nullptr ? attribute2->height : attribute->height);

	Graphics::VideoOutBufferFormat format = Graphics::VideoOutBufferFormat::Unknown;

	if (attribute2 != nullptr)
	{
		if (attribute2->pixel_format == 0x8000000000000000ULL)
		{
			format = Graphics::VideoOutBufferFormat::B8G8R8A8Srgb;
		} else if (attribute2->pixel_format == 0x8000000022000000ULL)
		{
			format = Graphics::VideoOutBufferFormat::R8G8B8A8Srgb;
		} else if (attribute2->pixel_format == 0x8100000000000000ULL)
		{
			// SCE_VIDEO_OUT_PIXEL_FORMAT2_B10_G10_R10_A2
			format = Graphics::VideoOutBufferFormat::B10G10R10A2Unorm;
		} else if (attribute2->pixel_format == 0x8100000022000000ULL)
		{
			// SCE_VIDEO_OUT_PIXEL_FORMAT2_R10_G10_B10_A2
			format = Graphics::VideoOutBufferFormat::R10G10B10A2Unorm;
		}
	} else
	{
		if (attribute->pixel_format == 0x80000000)
		{
			format = Graphics::VideoOutBufferFormat::B8G8R8A8Srgb;
		} else if (attribute->pixel_format == 0x80002200)
		{
			format = Graphics::VideoOutBufferFormat::R8G8B8A8Srgb;
		}
	}

	Graphics::VideoOutBufferObject vulkan_buffer_info(format, width, height, tile, neo, buffer_pitch);

	for (int i = 0; i < buffer_num; i++)
	{
		EXIT_NOT_IMPLEMENTED((reinterpret_cast<uint64_t>(addresses[i]) & (buffer_align - 1u)) != 0);
		for (int j = i + 1; j < buffer_num; j++)
		{
			if (VideoOutRangesOverlap(reinterpret_cast<uint64_t>(addresses[i]), buffer_size,
			                         reinterpret_cast<uint64_t>(addresses[j]), buffer_size))
			{
				return VIDEO_OUT_ERROR_INVALID_ADDRESS;
			}
		}
	}

	auto overlaps_registered_video_out = [&]() {
		for (int i = 0; i < buffer_num; i++)
		{
			const auto requested_address = reinterpret_cast<uint64_t>(addresses[i]);
			for (const auto& registered_ctx: m_video_out_ctx)
			{
				for (const auto& registered: registered_ctx.buffers)
				{
					if (registered.buffer != nullptr &&
					    VideoOutRangesOverlap(requested_address, buffer_size, reinterpret_cast<uint64_t>(registered.buffer),
					                          registered.buffer_size))
					{
						return true;
					}
				}
			}
		}
		return false;
	};

	Graphics::VideoOutMaterializationGate::Pin registration_pin;
	VideoOutConfig*                          ctx = nullptr;
	{
		Core::LockGuard lock(m_mutex);
			handle = ResolveHandleLocked(handle);
		if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX)
		{
			return VIDEO_OUT_ERROR_INVALID_HANDLE;
		}
		ctx = m_video_out_ctx + handle;
		if (!ctx->opened || ctx->closing)
		{
			return VIDEO_OUT_ERROR_INVALID_HANDLE;
		}
		if (overlaps_registered_video_out())
		{
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}

		for (int i = 0; i < buffer_num; i++)
		{
			const int slot = i + start_index;
			if (ctx->buffers[slot].buffer != nullptr || ctx->buffer_registration_reserved[slot])
			{
				return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
			}
		}
		for (int i = 0; i < buffer_num; i++)
		{
			ctx->buffer_registration_reserved[i + start_index] = true;
		}
		registration_pin = m_registration_gate.Acquire();
	}

	// Staging may run concurrently with vblank, flips, image queries and Close,
	// but registrations themselves remain ordered so resolution epochs and
	// generated set IDs cannot overtake one another.
	Core::LockGuard registration_lock(m_registration_mutex);

	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(ctx == nullptr || !ctx->opened);
		if (overlaps_registered_video_out())
		{
			for (int i = 0; i < buffer_num; i++)
			{
				const int slot = i + start_index;
				EXIT_IF(!ctx->buffer_registration_reserved[slot]);
				ctx->buffer_registration_reserved[slot] = false;
			}
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}
	}

	Graphics::WindowWaitForGraphicInitialized();
	Graphics::GraphicsRenderCreateContext();
	auto* graphic_ctx = GetGraphicCtx();

	VideoOutBufferInfo             staged_buffers[16] {};
	Graphics::VideoOutVulkanImage* staged_images[16] {};
	for (int i = 0; i < buffer_num; i++)
	{
		auto& staged         = staged_buffers[i];
		staged.buffer        = addresses[i];
		staged.buffer_size   = buffer_size;
		staged.buffer_pitch  = buffer_pitch;
		staged.buffer_vulkan = static_cast<Graphics::VideoOutVulkanImage*>(Graphics::GpuMemoryCreateObject(
		    0, graphic_ctx, nullptr, reinterpret_cast<uint64_t>(addresses[i]), buffer_size, vulkan_buffer_info));
		EXIT_NOT_IMPLEMENTED(staged.buffer_vulkan == nullptr);
		staged_images[i] = staged.buffer_vulkan;
	}

	{
		Core::LockGuard lock(m_mutex);

		// Close marks the context as closing, then waits for this transaction
		// outside m_mutex. A registration that already reserved its slots
		// therefore linearizes completely before Close clears the session.
		EXIT_IF(ctx == nullptr || !ctx->opened);
		for (int i = 0; i < buffer_num; i++)
		{
			const int slot = i + start_index;
			EXIT_IF(!ctx->buffer_registration_reserved[slot] || ctx->buffers[slot].buffer != nullptr);
		}

		// Commit the runtime extent, every staged image selection and the
		// published set under one VideoOut observation boundary. Draw/image
		// lookup can therefore see either the previous set or this complete
		// cohort, never a registered candidate without its buffers.
		const auto resolution_status = Graphics::InternalResolutionRuntimeRegisterGuestDisplayExtent({width, height});
		EXIT_IF(resolution_status != Graphics::ResolutionPolicyStatus::Success);
		const auto resolution  = Graphics::InternalResolutionRuntimeGetSnapshot();
		const auto host_extent = resolution.candidate_decision.classification == Graphics::ResolutionClassification::Scaled
		                             ? resolution.candidate_decision.host_extent
		                             : Graphics::ResolutionExtent {width, height};

		Graphics::VideoOutHostExtentSetState selection_state {};
		const auto selection =
		    Graphics::VideoOutBufferSelectHostExtentSet(staged_images, static_cast<uint32_t>(buffer_num), host_extent.width,
		                                                host_extent.height, &selection_state);
		if (selection != Graphics::VideoOutHostExtentSetSelectionStatus::Selected &&
		    selection != Graphics::VideoOutHostExtentSetSelectionStatus::StickyMatch)
		{
			EXIT("VideoOut buffer-set host extent conflict: guest=%ux%u requested=%ux%u selected=%ux%u status=%s(%u)\n", width,
			     height, host_extent.width, host_extent.height, selection_state.width, selection_state.height,
			     Graphics::VideoOutHostExtentSetSelectionStatusName(selection), static_cast<unsigned>(selection));
		}

		// Generated set IDs are committed with publication. Validation failures
		// such as SLOT_OCCUPIED leave the sequence unchanged.
		const int         effective_set_id = generate_set_id ? ctx->buffers_sets_seq : set_id;
		VideoOutBufferSet new_set {};
		new_set.start_index = start_index;
		new_set.num         = buffer_num;
		new_set.set_id      = effective_set_id;
		if (attribute2 != nullptr)
		{
			new_set.attr.gen5 = *attribute2;
			new_set.gen5      = true;
		} else
		{
			new_set.attr.gen4 = *attribute;
			new_set.gen5      = false;
		}

		for (int i = 0; i < buffer_num; i++)
		{
			const int slot                              = i + start_index;
			staged_buffers[i].set_id                    = effective_set_id;
			staged_buffers[i].guest_width               = width;
			staged_buffers[i].guest_height              = height;
			staged_buffers[i].host_width                = host_extent.width;
			staged_buffers[i].host_height               = host_extent.height;
			ctx->buffers[slot]                          = staged_buffers[i];
			ctx->buffer_registration_reserved[slot]     = false;
			printf("\tbuffers[%d] = %016" PRIx64 "\n", slot, reinterpret_cast<uint64_t>(addresses[i]));
		}
		ctx->buffers_sets.Add(new_set);
		if (generate_set_id)
		{
			ctx->buffers_sets_seq++;
		}

		return generate_set_id ? effective_set_id : OK;
	}
}

void VideoOutContext::DetachRegisteredBuffersForUnmapLocked(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(size == 0);

	for (auto& ctx: m_video_out_ctx)
	{
		Vector<VideoOutBufferSet> remaining_sets;
		for (const auto& set: ctx.buffers_sets)
		{
			int segment_start = -1;
			auto flush_segment = [&](int segment_end)
			{
				if (segment_start < 0)
				{
					return;
				}
				auto remaining        = set;
				remaining.start_index = segment_start;
				remaining.num         = segment_end - segment_start;
				remaining_sets.Add(remaining);
				segment_start = -1;
			};

			for (int slot = set.start_index; slot < set.start_index + set.num; slot++)
			{
				auto& buffer = ctx.buffers[slot];
				const bool detach =
				    buffer.buffer != nullptr &&
				    VideoOutRangesOverlap(vaddr, size, reinterpret_cast<uint64_t>(buffer.buffer), buffer.buffer_size);
				if (detach)
				{
					flush_segment(slot);
					buffer = {};
				} else if (buffer.buffer != nullptr)
				{
					if (segment_start < 0)
					{
						segment_start = slot;
					}
				} else
				{
					flush_segment(slot);
				}
			}
			flush_segment(set.start_index + set.num);
		}
		ctx.buffers_sets = std::move(remaining_sets);
	}
}

bool VideoOutContext::RunBufferUnmapTransaction(uint64_t vaddr, uint64_t size, VideoOutQuiescedAction action, void* data)
{
	EXIT_IF(size == 0);
	EXIT_IF(action == nullptr);

	auto quiesce = m_host_access_gate.Quiesce();

	VideoOutConfig* opened[VIDEO_OUT_NUM_MAX] {};
	uint32_t        opened_num = 0;
	{
		Core::LockGuard lock(m_mutex);
		for (auto& ctx: m_video_out_ctx)
		{
			if (ctx.opened)
			{
				opened[opened_num++] = &ctx;
			}
		}
	}

	for (uint32_t index = 0; index < opened_num; index++)
	{
		m_flip_queue.WaitForConfig(opened[index]);
	}
	m_registration_gate.WaitUntilIdle();
	m_materialization_gate.WaitUntilIdle();

	{
		Core::LockGuard lock(m_mutex);
		DetachRegisteredBuffersForUnmapLocked(vaddr, size);
	}

	return action(data);
}

KYTY_SYSV_ABI int VideoOutRegisterBuffers(int handle, int start_index, void* const* addresses, int buffer_num,
                                          const VideoOutBufferAttribute* attribute)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (addresses == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (start_index < 0 || start_index > 15 || buffer_num < 1 || buffer_num > 16 || start_index + buffer_num > 15)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	printf("\t start_index    = %d\n", start_index);
	printf("\t buffer_num     = %d\n", buffer_num);
	printf("\t pixel_format   = 0x%08" PRIx32 "\n", attribute->pixel_format);
	printf("\t tiling_mode    = %" PRIu32 "\n", attribute->tiling_mode);
	printf("\t aspect_ratio   = %" PRIu32 "\n", attribute->aspect_ratio);
	printf("\t width          = %" PRIu32 "\n", attribute->width);
	printf("\t height         = %" PRIu32 "\n", attribute->height);
	printf("\t pitch_in_pixel = %" PRIu32 "\n", attribute->pitch_in_pixel);
	printf("\t option         = %" PRIu32 "\n", attribute->option);

	// EXIT_NOT_IMPLEMENTED(attribute->pixel_format != 0x80000000);
	EXIT_NOT_IMPLEMENTED(attribute->tiling_mode != 0);
	EXIT_NOT_IMPLEMENTED(attribute->aspect_ratio != 0);
	EXIT_NOT_IMPLEMENTED(attribute->pitch_in_pixel != attribute->width);
	EXIT_NOT_IMPLEMENTED(attribute->option != 0);

	return g_video_out_context->RegisterBuffers(handle, 0, true, start_index, addresses, buffer_num, attribute, nullptr);
}

KYTY_SYSV_ABI int VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start, const VideoOutBuffers* buffers,
                                           int buffer_num, const VideoOutBufferAttribute2* attribute, int category, void* option)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (buffers == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (buffer_index_start < 0 || buffer_index_start > 15 || buffer_num < 1 || buffer_num > 16 || buffer_index_start + buffer_num > 15)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	printf("\t start_index    = %d\n", buffer_index_start);
	printf("\t buffer_num     = %d\n", buffer_num);
	printf("\t set_index      = %d\n", set_index);
	printf("\t pixel_format   = 0x%016" PRIx64 "\n", attribute->pixel_format);
	printf("\t tiling_mode    = %" PRIu32 "\n", attribute->tiling_mode);
	printf("\t aspect_ratio   = %" PRIu32 "\n", attribute->aspect_ratio);
	printf("\t width          = %" PRIu32 "\n", attribute->width);
	printf("\t height         = %" PRIu32 "\n", attribute->height);
	printf("\t pitch_in_pixel = %" PRIu32 "\n", attribute->pitch_in_pixel);
	printf("\t option         = %" PRIu64 "\n", attribute->option);

	// EXIT_NOT_IMPLEMENTED(attribute->pixel_format != 0x80000000);
	EXIT_NOT_IMPLEMENTED(option != nullptr);
	EXIT_NOT_IMPLEMENTED(category != 0);
	EXIT_NOT_IMPLEMENTED(attribute->tiling_mode != 0);
	EXIT_NOT_IMPLEMENTED(attribute->aspect_ratio != 0);
	EXIT_NOT_IMPLEMENTED(attribute->pitch_in_pixel != 0);
	EXIT_NOT_IMPLEMENTED(attribute->option != 0 && attribute->option != 8);
	EXIT_NOT_IMPLEMENTED(attribute->dcc_cb_register_clear_color != 0);
	EXIT_NOT_IMPLEMENTED(attribute->dcc_control != 0);

	Vector<const void*> addresses(buffer_num);

	for (int i = 0; i < buffer_num; i++)
	{
		EXIT_NOT_IMPLEMENTED(buffers[i].metadata != nullptr);

		addresses[i] = buffers[i].data;
	}

	return g_video_out_context->RegisterBuffers(handle, set_index, false, buffer_index_start, addresses.GetDataConst(), buffer_num,
	                                            nullptr, attribute);
}

VideoOutBufferImageInfo VideoOutGetImageMetadataForSubmission(uint64_t addr, Graphics::CommandBuffer* buffer)
{
	EXIT_IF(g_video_out_context == nullptr);
	return g_video_out_context->FindImageForSubmission(reinterpret_cast<void*>(addr), buffer, false);
}

VideoOutBufferImageInfo VideoOutGetImageForSubmission(uint64_t addr, Graphics::CommandBuffer* buffer)
{
	EXIT_IF(g_video_out_context == nullptr);
	return g_video_out_context->FindImageForSubmission(reinterpret_cast<void*>(addr), buffer, true);
}

VideoOutRegisteredHostExtentStatus VideoOutGetRegisteredHostExtent(Graphics::CommandBuffer* buffer, uint32_t guest_width,
                                                                   uint32_t guest_height, uint32_t* host_width,
                                                                   uint32_t* host_height)
{
	EXIT_IF(g_video_out_context == nullptr);
	return g_video_out_context->GetRegisteredHostExtent(buffer, guest_width, guest_height, host_width, host_height);
}

bool VideoOutIsValidFlipMode(int flip_mode)
{
	return flip_mode >= 1 && flip_mode <= 4;
}

KYTY_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (!VideoOutIsValidFlipMode(flip_mode))
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (index < 0 || index > 15)
	{
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	switch (g_video_out_context->SubmitFlip(handle, index, flip_arg))
	{
		case SubmitFlipStatus::Submitted: return OK;
		case SubmitFlipStatus::InvalidHandle: return VIDEO_OUT_ERROR_INVALID_HANDLE;
		case SubmitFlipStatus::InvalidIndex: return VIDEO_OUT_ERROR_INVALID_INDEX;
		case SubmitFlipStatus::QueueFull: return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}
	return VIDEO_OUT_ERROR_INVALID_HANDLE;
}

void VideoOutSubmitFlipInternal(int handle, int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(g_video_out_context == nullptr);
	if (!VideoOutIsValidFlipMode(flip_mode))
	{
		EXIT("Internal VideoOut flip has unsupported mode: handle=%d index=%d mode=%d\n", handle, index, flip_mode);
	}

	const auto status = g_video_out_context->SubmitFlipInternal(handle, index, flip_arg);
	if (status != SubmitFlipStatus::Submitted)
	{
		EXIT("Internal VideoOut flip failed: status=%s(%d) handle=%d index=%d mode=%d\n", SubmitFlipStatusName(status),
		     static_cast<int>(status), handle, index, flip_mode);
	}
}

void VideoOutWaitFlipDone(int handle, int index)
{
	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	EXIT_NOT_IMPLEMENTED(!session);
	auto* ctx = session.Get();

	EXIT_NOT_IMPLEMENTED(index < 0 || index > 15);

	g_video_out_context->GetFlipQueue().Wait(ctx, index);
}

bool VideoOutRunBufferUnmapTransaction(uint64_t vaddr, uint64_t size, VideoOutQuiescedAction action, void* data)
{
	EXIT_IF(g_video_out_context == nullptr);
	return g_video_out_context->RunBufferUnmapTransaction(vaddr, size, action, data);
}

KYTY_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (status == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();

	g_video_out_context->GetFlipQueue().GetFlipStatus(ctx, status);

	printf("\t count = %" PRIu64 "\n", status->count);
	printf("\t processTime = %" PRIu64 "\n", status->processTime);
	printf("\t tsc = %" PRIu64 "\n", status->tsc);
	printf("\t submitTsc = %" PRIu64 "\n", status->submitTsc);
	printf("\t flipArg = %" PRId64 "\n", status->flipArg);
	printf("\t gcQueueNum = %d\n", status->gcQueueNum);
	printf("\t flipPendingNum = %d\n", status->flipPendingNum);
	printf("\t currentBuffer = %d\n", status->currentBuffer);

	return OK;
}

KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();

	VideoOutFlipStatus status {};
	g_video_out_context->GetFlipQueue().GetFlipStatus(ctx, &status);
	printf("\t flipPendingNum = %d\n", status.flipPendingNum);
	return status.flipPendingNum;
}

struct VideoOutOutputStatus
{
	uint32_t resolution   = 1;
	uint32_t dynamicRange = 1;
	uint32_t refreshRate  = 1;
	uint32_t reserved     = 0;
};

struct VideoOutColorSettings
{
	float    gamma     = 1.0f;
	uint32_t reserved0 = 0;
	uint32_t reserved1 = 0;
	uint32_t reserved2 = 0;
};

KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status)
{
	PRINT_NAME();
	EXIT_IF(g_video_out_context == nullptr);
	if (status == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();
	status->resolution   = (ctx->resolution.fullWidth >= 3840 || ctx->resolution.fullHeight >= 2160) ? 2u : 1u;
	status->dynamicRange = 1;
	status->refreshRate  = 1;
	status->reserved     = 0;
	return OK;
}

KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma)
{
	PRINT_NAME();
	printf("\t gamma = %g\n", static_cast<double>(gamma));
	if (settings == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	settings->gamma = gamma;
	return OK;
}

KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings)
{
	PRINT_NAME();
	EXIT_IF(g_video_out_context == nullptr);
	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	(void)settings;
	return OK;
}

KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index, const VideoOutBufferAttribute2* attribute)
{
	PRINT_NAME();
	EXIT_IF(g_video_out_context == nullptr);
	printf("\t handle = %d set_index = %d attr = 0x%016" PRIx64 "\n", handle, set_index, reinterpret_cast<uint64_t>(attribute));
	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return OK;
}

KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (status == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto* ctx = session.Get();

	ctx->mutex.Lock();
	*status = ctx->vblank_status;
	ctx->mutex.Unlock();

	printf("\t count = %" PRIu64 "\n", status->count);
	printf("\t processTime = %" PRIu64 "\n", status->processTime);
	printf("\t tsc = %" PRIu64 "\n", status->tsc);

	return OK;
}

KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	printf("\t top    = %d\n", top);
	printf("\t bottom = %d\n", bottom);

	return OK;
}

namespace {

constexpr uint64_t kVideoOutOutputModeDefault   = 1;
constexpr uint64_t kVideoOutOutputMode119_88Hz  = 0xF;
constexpr size_t   kVideoOutOutputOptionsSize   = 0x40;

bool IsValidVideoOutEvent(const LibKernel::EventQueue::KernelEvent* ev)
{
	return ev != nullptr && ev->filter == EventQueue::KERNEL_EVFILT_VIDEO_OUT &&
	       (ev->ident == VIDEO_OUT_EVENT_FLIP || ev->ident == VIDEO_OUT_EVENT_VBLANK);
}

} // namespace

KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	std::vector<EventQueue::KernelEqueuePin> pins;
	{
		auto* ctx = session.Get();
		Core::LockGuard config_lock(ctx->mutex);
		for (auto* binding: ctx->vblank_events)
		{
			if (binding != nullptr && binding->identity.eq == eq)
			{
				if (auto pin = EventQueue::KernelAcquireEqueue(binding->identity))
				{
					pins.push_back(std::move(pin));
				}
			}
		}
	}

	for (auto& pin: pins)
	{
		const auto result =
		    EventQueue::KernelDeleteEvent(pin, VIDEO_OUT_EVENT_VBLANK, EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetEventId(const LibKernel::EventQueue::KernelEvent* ev)
{
	PRINT_NAME();

	if (ev == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (!IsValidVideoOutEvent(ev))
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	return static_cast<int>(ev->ident);
}

KYTY_SYSV_ABI int VideoOutGetEventData(const LibKernel::EventQueue::KernelEvent* ev, uint64_t* data)
{
	PRINT_NAME();

	if (ev == nullptr || data == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (!IsValidVideoOutEvent(ev))
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	*data = static_cast<uint64_t>(ev->data);
	return OK;
}

KYTY_SYSV_ABI int VideoOutConfigureOutput(int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutInitializeOutputOptions(void* options)
{
	PRINT_NAME();

	if (options == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	std::memset(options, 0, kVideoOutOutputOptionsSize);
	return OK;
}

KYTY_SYSV_ABI int VideoOutIsOutputSupported(int handle, uint64_t mode, const void* options, const void* reserved_pointer,
                                            uint64_t reserved)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (reserved_pointer != nullptr || reserved != 0)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (options != nullptr)
	{
		const auto* bytes = static_cast<const uint8_t*>(options);
		for (size_t i = 0; i < kVideoOutOutputOptionsSize; i++)
		{
			if (bytes[i] != 0)
			{
				return VIDEO_OUT_ERROR_INVALID_OPTION;
			}
		}
	}

	if (mode != kVideoOutOutputModeDefault && mode != kVideoOutOutputMode119_88Hz)
	{
		return VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE;
	}

	return mode == kVideoOutOutputModeDefault ? 1 : 0;
}

KYTY_SYSV_ABI int VideoOutUnregisterBuffers(int handle, int attribute_index)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (attribute_index < 0)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* ctx = session.Get();
	Core::LockGuard config_lock(ctx->mutex);

	int set_index = -1;
	if (attribute_index < static_cast<int>(ctx->buffers_sets.Size()))
	{
		set_index = attribute_index;
	} else
	{
		for (int i = 0; i < static_cast<int>(ctx->buffers_sets.Size()); i++)
		{
			if (ctx->buffers_sets[i].set_id == attribute_index)
			{
				set_index = i;
				break;
			}
		}
	}

	if (set_index < 0)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	const auto& set = ctx->buffers_sets[set_index];
	for (int slot = set.start_index; slot < set.start_index + set.num; slot++)
	{
		if (slot >= 0 && slot < 16)
		{
			ctx->buffers[slot] = {};
		}
	}
	ctx->buffers_sets.RemoveAt(set_index);
	return OK;
}

KYTY_SYSV_ABI int VideoOutWaitVblank(int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto session = g_video_out_context->AcquireSession(handle);
	if (!session)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* ctx = session.Get();
	uint64_t start_count = 0;
	{
		Core::LockGuard config_lock(ctx->mutex);
		start_count = ctx->vblank_status.count;
	}

	constexpr LibKernel::KernelUseconds frame_us = 16667;
	for (LibKernel::KernelUseconds waited = 0; waited <= frame_us; waited += 1000)
	{
		{
			Core::LockGuard config_lock(ctx->mutex);
			if (ctx->vblank_status.count > start_count)
			{
				return OK;
			}
		}
		LibKernel::KernelUsleep(1000);
	}

	return OK;
}

} // namespace Kyty::Libs::VideoOut

#endif // KYTY_EMU_ENABLED
