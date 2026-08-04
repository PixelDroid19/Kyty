#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRUNINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRUNINTERNAL_H_

#include "Emulator/Graphics/GraphicsRun.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/AsyncJob.h"
#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GpuSubmissionCoordinator.h"
#include "Emulator/Graphics/GpuSubmissionPublicationGate.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/HardwareContext.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

#define KYTY_HW_CTX_PARSER_ARGS                                                                                                            \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define KYTY_HW_UC_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define KYTY_HW_SH_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define KYTY_HW_CTX_INDIRECT_ARGS CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define KYTY_HW_UC_INDIRECT_ARGS  CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define KYTY_HW_SH_INDIRECT_ARGS  CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value

#define KYTY_HW_CTX_PARSER(f) uint32_t f(KYTY_HW_CTX_PARSER_ARGS)
#define KYTY_HW_UC_PARSER(f)  uint32_t f(KYTY_HW_UC_PARSER_ARGS)
#define KYTY_HW_SH_PARSER(f)  uint32_t f(KYTY_HW_SH_PARSER_ARGS)

#define KYTY_CP_OP_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id, [[maybe_unused]] const uint32_t *buffer,                      \
	    [[maybe_unused]] uint32_t dw, [[maybe_unused]] uint32_t num_dw
#define KYTY_CP_OP_PARSER(f) uint32_t f(KYTY_CP_OP_PARSER_ARGS)

namespace Kyty::Libs::Graphics {

using hw_ctx_parser_func_t   = uint32_t (*)(KYTY_HW_CTX_PARSER_ARGS);
using hw_uc_parser_func_t    = uint32_t (*)(KYTY_HW_UC_PARSER_ARGS);
using hw_sh_parser_func_t    = uint32_t (*)(KYTY_HW_SH_PARSER_ARGS);
using cp_op_parser_func_t    = uint32_t (*)(KYTY_CP_OP_PARSER_ARGS);
using hw_ctx_indirect_func_t = void (*)(KYTY_HW_CTX_INDIRECT_ARGS);
using hw_uc_indirect_func_t  = void (*)(KYTY_HW_UC_INDIRECT_ARGS);
using hw_sh_indirect_func_t  = void (*)(KYTY_HW_SH_INDIRECT_ARGS);

// Jump tables populated once by graphics_init_jmp_tables_*(). The per-domain
// parser units register their functions here at startup.
extern hw_ctx_parser_func_t   g_hw_ctx_func[Pm4::CX_NUM];
extern hw_ctx_indirect_func_t g_hw_ctx_indirect_func[Pm4::CX_NUM];
extern hw_sh_parser_func_t    g_hw_sh_func[Pm4::SH_NUM];
extern hw_sh_indirect_func_t  g_hw_sh_indirect_func[Pm4::SH_NUM];
extern hw_uc_parser_func_t    g_hw_uc_func[Pm4::UC_NUM];
extern hw_uc_indirect_func_t  g_hw_uc_indirect_func[Pm4::UC_NUM];
extern hw_sh_parser_func_t    g_hw_sh_custom_func[Pm4::R_NUM];
extern cp_op_parser_func_t    g_cp_op_func[256];
extern cp_op_parser_func_t    g_cp_op_custom_func[Pm4::R_NUM];

[[nodiscard]] const char* gpu_submission_result_name(GpuSubmissionResult result);
void require_submission_success(GpuSubmissionResult result, const char* operation, int queue, uint32_t slot);
[[nodiscard]] const char* gpu_submission_publication_result_name(GpuSubmissionPublicationResult result);
void require_publication_success(GpuSubmissionPublicationResult result, const char* operation, int queue, SubmissionId submission);

void GraphicsRunTraceAaRegisterWrite(const char* path, const char* name, uint32_t value);
void GraphicsRunTraceWait(const char* stage, int queue, uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                          uint64_t sequence, uint64_t elapsed_ns);

inline void trace_aa_register_write(const char* path, const char* name, uint32_t value)
{
	GraphicsRunTraceAaRegisterWrite(path, name, value);
}

class Gpu;

extern Gpu* g_gpu;

void graphics_init_jmp_tables();

// Hardware register parsers. Each lives in GraphicsRunRegisterParsers.cpp and
// is registered into the jump tables at startup.
KYTY_HW_CTX_PARSER(hw_ctx_ignore);
KYTY_HW_CTX_PARSER(hw_ctx_set_aa_config);
KYTY_HW_CTX_PARSER(hw_ctx_set_aa_sample_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_blend_color);
KYTY_HW_CTX_PARSER(hw_ctx_set_blend_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_clip_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_color_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_color_info);
KYTY_HW_CTX_PARSER(hw_ctx_set_depth_clear);
KYTY_HW_CTX_PARSER(hw_ctx_set_depth_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_depth_render_target);
KYTY_HW_CTX_PARSER(hw_ctx_set_eqaa_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_generic_scissor);
KYTY_HW_CTX_PARSER(hw_ctx_set_guard_bands);
KYTY_HW_CTX_PARSER(hw_ctx_set_hardware_screen_offset);
KYTY_HW_CTX_PARSER(hw_ctx_set_line_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_mode_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_polygon_offset);
KYTY_HW_CTX_PARSER(hw_ctx_set_ps_input);
KYTY_HW_CTX_PARSER(hw_ctx_set_render_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_render_target);
KYTY_HW_CTX_PARSER(hw_ctx_set_render_target_mask);
KYTY_HW_CTX_PARSER(hw_ctx_set_scan_mode_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_screen_scissor);
KYTY_HW_CTX_PARSER(hw_ctx_set_shader_stages);
KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_clear);
KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_info);
KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_mask);
KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_scale_offset);
KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_transform_control);
KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_z);
KYTY_HW_CTX_PARSER(hw_ctx_set_window_offset);
KYTY_HW_SH_PARSER(hw_sh_set_cs_num_thread);
KYTY_HW_SH_PARSER(hw_sh_set_cs_pgm);
KYTY_HW_SH_PARSER(hw_sh_set_cs_resource_limits);
KYTY_HW_SH_PARSER(hw_sh_set_cs_rsrc);
KYTY_HW_SH_PARSER(hw_sh_set_cs_shader);
KYTY_HW_SH_PARSER(hw_sh_set_cs_user_sgpr);
KYTY_HW_SH_PARSER(hw_sh_set_gs_user_sgpr);
KYTY_HW_SH_PARSER(hw_sh_set_ps_embedded);
KYTY_HW_SH_PARSER(hw_sh_set_ps_shader);
KYTY_HW_SH_PARSER(hw_sh_set_ps_user_sgpr);
KYTY_HW_SH_PARSER(hw_sh_set_vs_embedded);
KYTY_HW_SH_PARSER(hw_sh_set_vs_shader);
KYTY_HW_SH_PARSER(hw_sh_set_vs_user_sgpr);
KYTY_HW_SH_PARSER(hw_sh_update_ps_shader);
KYTY_HW_SH_PARSER(hw_sh_update_vs_shader);
KYTY_HW_UC_PARSER(hw_uc_set_primitive_type);

// Command processor opcode parsers. Each lives in GraphicsRunOpParsers.cpp
// and is registered into the jump tables at startup.
KYTY_CP_OP_PARSER(cp_op_acquire_mem);
KYTY_CP_OP_PARSER(cp_op_clear_state);
KYTY_CP_OP_PARSER(cp_op_custom_dma_data);
KYTY_CP_OP_PARSER(cp_op_dispatch_direct);
KYTY_CP_OP_PARSER(cp_op_dispatch_indirect);
KYTY_CP_OP_PARSER(cp_op_dispatch_reset);
KYTY_CP_OP_PARSER(cp_op_dma_data);
KYTY_CP_OP_PARSER(cp_op_draw_index);
KYTY_CP_OP_PARSER(cp_op_draw_index_auto);
KYTY_CP_OP_PARSER(cp_op_draw_index_indirect);
KYTY_CP_OP_PARSER(cp_op_draw_index_offset);
KYTY_CP_OP_PARSER(cp_op_draw_reset);
KYTY_CP_OP_PARSER(cp_op_dump_const_ram);
KYTY_CP_OP_PARSER(cp_op_event_write);
KYTY_CP_OP_PARSER(cp_op_event_write_eop);
KYTY_CP_OP_PARSER(cp_op_event_write_eos);
KYTY_CP_OP_PARSER(cp_op_flip);
KYTY_CP_OP_PARSER(cp_op_get_lod_stats);
KYTY_CP_OP_PARSER(cp_op_increment_ce_counter);
KYTY_CP_OP_PARSER(cp_op_increment_de_counter);
KYTY_CP_OP_PARSER(cp_op_index_base);
KYTY_CP_OP_PARSER(cp_op_index_buffer_size);
KYTY_CP_OP_PARSER(cp_op_index_type);
KYTY_CP_OP_PARSER(cp_op_indirect_buffer);
KYTY_CP_OP_PARSER(cp_op_indirect_buffer_end);
KYTY_CP_OP_PARSER(cp_op_indirect_cx_regs);
KYTY_CP_OP_PARSER(cp_op_indirect_sh_regs);
KYTY_CP_OP_PARSER(cp_op_indirect_uc_regs);
KYTY_CP_OP_PARSER(cp_op_nop);
KYTY_CP_OP_PARSER(cp_op_num_instances);
KYTY_CP_OP_PARSER(cp_op_one_reg_write);
KYTY_CP_OP_PARSER(cp_op_pop_marker);
KYTY_CP_OP_PARSER(cp_op_push_marker);
KYTY_CP_OP_PARSER(cp_op_release_mem);
KYTY_CP_OP_PARSER(cp_op_set_base);
KYTY_CP_OP_PARSER(cp_op_set_context_reg);
KYTY_CP_OP_PARSER(cp_op_set_shader_reg);
KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg);
KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg_index);
KYTY_CP_OP_PARSER(cp_op_wait_flip_done);
KYTY_CP_OP_PARSER(cp_op_wait_on_ce_counter);
KYTY_CP_OP_PARSER(cp_op_wait_on_de_counter_diff);
KYTY_CP_OP_PARSER(cp_op_wait_reg_mem);
KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_32);
KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_64);
KYTY_CP_OP_PARSER(cp_op_write_const_ram);
KYTY_CP_OP_PARSER(cp_op_write_data);

uint64_t SuspendedWaitNowNs();
uint64_t SuspendedWaitTimeoutMs();

class ScopedDebugStatsTimer
{
public:
	using RecordFunc = void (*)(uint64_t);

	explicit ScopedDebugStatsTimer(RecordFunc record): m_record(record), m_start(std::chrono::steady_clock::now()) {}
	~ScopedDebugStatsTimer()
	{
		const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
		m_record(static_cast<uint64_t>(elapsed_ns));
	}

	KYTY_CLASS_NO_COPY(ScopedDebugStatsTimer);

private:
	RecordFunc                            m_record;
	std::chrono::steady_clock::time_point m_start;
};

inline void TraceWait(const char* stage, int queue, uint64_t address, uint64_t value, uint64_t reference, uint64_t mask, uint64_t sequence,
                      uint64_t elapsed_ns = 0)
{
	GraphicsRunTraceWait(stage, queue, address, value, reference, mask, sequence, elapsed_ns);
}

class CommandProcessor
{
public:
	struct FlipInfo
	{
		int     handle    = 0;
		int     index     = 0;
		int     flip_mode = 0;
		int64_t flip_arg  = 0;
	};

	CommandProcessor(GpuSubmissionCoordinator* submission_coordinator, int queue)
	    : m_submission_slots(submission_coordinator, GpuQueueId(static_cast<uint32_t>(queue))),
	      m_publication_gate(GpuQueueId(static_cast<uint32_t>(queue))), m_queue(queue)
	{
		EXIT_IF(submission_coordinator == nullptr);
		EXIT_IF(queue < 0 || queue >= GraphicContext::QUEUES_NUM);
	}
	virtual ~CommandProcessor() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(CommandProcessor);

	void Reset();

	void               BufferInit();
	SubmissionId       BufferFlush();
	void               BufferWait();
	void               PumpCompletedSubmissions();
	void               SubmitAndWait();
	void               WaitSubmission(SubmissionId submission);
	[[nodiscard]] bool OwnsSubmissionQueue(SubmissionId submission) const
	{
		return submission.queue == GpuQueueId(static_cast<uint32_t>(m_queue));
	}

	void RunLock() { m_run_mutex.Lock(); }
	void RunUnlock() { m_run_mutex.Unlock(); }

	HW::Context*    GetCtx() { return &m_ctx; }
	HW::UserConfig* GetUcfg() { return &m_ucfg; }
	HW::Shader*     GetShCtx() { return &m_sh_ctx; }

	void SetIndexType(uint32_t index_type_and_size);
	void SetIndexBaseAddress(uint64_t index_base_addr);
	void SetIndexBufferSize(uint32_t index_buffer_size);
	void SetIndirectArgsBaseAddress(uint32_t base_index, uint64_t address);
	void SetNumInstances(uint32_t num_instances);
	void DrawIndex(uint32_t index_count, const void* index_addr, uint64_t draw_modifier, uint32_t type);
	void DrawIndexOffset(uint32_t index_offset, uint32_t index_count, uint32_t flags);
	void DrawIndexIndirect(uint32_t data_offset, uint32_t initiator);
	void DrawIndexAuto(uint32_t index_count, uint64_t draw_modifier);
	void WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
	                        uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint32_t value,
	                        uint32_t interrupt_selector);
	void WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
	                        uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint64_t value,
	                        uint32_t interrupt_selector);
	void Flip();
	void Flip(void* dst_gpu_addr, uint32_t value);
	void FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action, void* dst_gpu_addr, uint32_t value);
	void QueueQueuedGraphicsInterrupt();
	void WriteBack();
	void MemoryBarrier();
	void RenderTextureBarrier(uint64_t vaddr, uint64_t size);
	void DepthStencilBarrier(uint64_t vaddr, uint64_t size);
	void DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
	void DispatchIndirect(uint32_t data_offset, uint32_t mode);
	void WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index);
	void TriggerEvent(uint32_t event_type, uint32_t event_index);

	void                           SetUserDataMarker(HW::UserSgprType type) { m_user_data_marker = type; }
	[[nodiscard]] HW::UserSgprType GetUserDataMarker() const { return m_user_data_marker; }
	void                           SetEmbeddedDataMarker(const uint32_t* buffer, uint32_t num_dw, uint32_t align) {}
	void                           PushMarker(const char* str) {}
	void                           PopMarker() {}

	void PrefetchL2(void* addr, uint32_t size) {}
	void ClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void ReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

	void ResetDeCe();
	void WaitCe();
	void WaitDeDiff(uint32_t diff);
	void IncremenetDe();
	void IncremenetCe();

	void WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num);
	void DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num);

	struct SuspendedRun
	{
		uint32_t*   data             = nullptr;
		uint32_t    num_dw           = 0;
		uint32_t*   resume_data      = nullptr;
		uint32_t    resume_num_dw    = 0;
		const void* address          = nullptr;
		uint64_t    reference        = 0;
		uint64_t    mask             = 0;
		uint32_t    function         = 0;
		uint32_t    size             = 0;
		uint64_t    blocked_since_ns = 0;
		bool        skip_wait        = false;
	};

	void WaitRegMem32(uint32_t func, const uint32_t* addr, uint32_t ref, uint32_t mask, uint32_t poll);
	void WaitRegMem64(uint32_t func, const uint64_t* addr, uint64_t ref, uint64_t mask, uint32_t poll);
	void WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control, bool custom, bool matching_wait_mem64);

	void Run(uint32_t* data, uint32_t num_dw, const uint32_t* source_data);
	[[nodiscard]] bool TakeSuspendedRun(SuspendedRun* run);
	[[nodiscard]] const uint32_t* GetActiveRunBegin() const { return m_active_run_begin; }
	[[nodiscard]] const uint32_t* GetActiveRunEnd() const { return m_active_run_end; }

	[[nodiscard]] const FlipInfo& GetFlip() const { return m_flip; }
	void                          SetFlip(const FlipInfo& flip)
	{
		m_flip                       = flip;
		m_flip_issued                = false;
		m_completion_callback_issued = false;
	}
	[[nodiscard]] bool FlipIssued() const { return m_flip_issued; }
	[[nodiscard]] bool CompletionCallbackIssued() const { return m_completion_callback_issued; }

	[[nodiscard]] uint64_t GetSumbitId() const { return m_sumbit_id; }
	void                   SetSumbitId(uint64_t sumbit_id) { m_sumbit_id = sumbit_id; }

private:
	static constexpr int VK_BUFFERS_NUM = static_cast<int>(CommandProcessorSubmissionSlots::SlotCount);
	void                 CompleteSubmittedThroughLocked(SubmissionId target, SubmissionId* latest_completed);
	void                 TryCompleteSubmittedLocked(SubmissionId* latest_completed);
	SubmissionId         SubmitCurrentLocked(SubmissionId* latest_completed);
	void                 PublishCompletedSubmissions();
	void                 WaitUntilPublishedUnlessReentrant(SubmissionId submission);

	struct Counter
	{
		Core::Mutex   mutex;
		Core::CondVar cond_var;
		uint32_t      value = 0;
	};

	HW::Context      m_ctx;
	HW::UserConfig   m_ucfg;
	HW::Shader       m_sh_ctx;
	HW::UserSgprType m_user_data_marker    = HW::UserSgprType::Unknown;
	uint32_t         m_index_type_and_size = 0;
	uint32_t         m_index_buffer_size   = 0;
	uint64_t         m_index_base_addr     = 0;
	uint64_t         m_draw_indirect_args_base_addr     = 0;
	uint64_t         m_dispatch_indirect_args_base_addr = 0;
	uint32_t         m_num_instances       = 1;

	Core::Mutex m_mutex;
	Core::Mutex m_run_mutex;

	CommandBuffer*                  m_buffer[VK_BUFFERS_NUM] = {};
	int                             m_current_buffer         = -1;
	CommandProcessorSubmissionSlots m_submission_slots;
	GpuSubmissionPublicationGate    m_publication_gate;
	int                             m_queue = -1;

	Counter m_de_counter;
	Counter m_ce_counter;

	uint32_t m_const_ram[0x3000] = {0};

	FlipInfo m_flip;
	bool     m_flip_issued                = false;
	bool     m_completion_callback_issued = false;
	uint64_t m_sumbit_id                  = 0;
	const uint32_t* m_active_run_begin    = nullptr;
	const uint32_t* m_active_run_end      = nullptr;
	bool            m_suspend_run_requested = false;
	bool            m_suspended_run_valid   = false;
	SuspendedRun    m_suspended_run;

	void RequestSuspendedWait(const void* address, uint32_t size, uint32_t function, uint64_t reference, uint64_t mask)
	{
		m_suspend_run_requested = true;
		m_suspended_run.address = address;
		m_suspended_run.size = size;
		m_suspended_run.function = function;
		m_suspended_run.reference = reference;
		m_suspended_run.mask = mask;
	}
};

class GraphicsRing
{
public:
	GraphicsRing(): m_job1("Thread_Gfx_Draw"), m_job2("Thread_Gfx_Const") { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~GraphicsRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(GraphicsRing);

	void Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle, int index,
	            int flip_mode, int64_t flip_arg, bool with_api_flip, GraphicsSubmissionCompletion completion);
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp)
	{
		m_cp = cp;
		Start();
	}

private:
	void Start()
	{
		Core::Thread t(ThreadBatchRun, this);
		t.Detach();
	}

	struct CmdBuffer
	{
		struct Snapshot
		{
			std::vector<uint32_t>                               words;
			std::vector<std::shared_ptr<std::vector<uint32_t>>> indirect_register_blocks;
		};

		std::shared_ptr<Snapshot> snapshot;
		uint32_t*                 data        = nullptr;
		const uint32_t*           source_data = nullptr;
		uint32_t                  num_dw      = 0;
	};

	struct CmdBatch
	{
		struct DecodeCompletion
		{
			void Wait()
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [&] { return complete; });
			}

			void Signal()
			{
				{
					std::lock_guard<std::mutex> lock(mutex);
					complete = true;
				}
				condition.notify_all();
			}

			std::mutex              mutex;
			std::condition_variable condition;
			bool                    complete = false;
		};

		CmdBuffer draw_buffer;
		CmdBuffer const_buffer;

		CommandProcessor::FlipInfo flip;
		// True only for GraphicsRunSubmitAndFlip. Do not infer from flip.handle:
		// handle 0 is a legal VideoOut handle, and plain Submit also stores zeros.
		bool                           with_api_flip = false;
		GraphicsSubmissionCompletion completion   = GraphicsSubmissionCompletion::None;
		std::shared_ptr<DecodeCompletion> decode_completion;
	};

	static void ThreadBatchRun(void* data);
	static CmdBuffer SnapshotCommandBuffer(uint32_t* data, uint32_t num_dw);

	CmdBatch GetCmdBatch();

	Core::Mutex          m_mutex;
	Core::CondVar        m_cond_var;
	Core::CondVar        m_idle_cond_var;
	Core::List<CmdBatch> m_cmd_batches;
	bool                 m_done = true;
	bool                 m_idle = true;

	AsyncJob m_job1;
	AsyncJob m_job2;

	CommandProcessor* m_cp = nullptr;
};

class ComputeRing
{
public:
	ComputeRing() = default;
	virtual ~ComputeRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(ComputeRing);

	void MapComputeQueue(uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr);
	void DingDong(uint32_t offset_dw);
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp)
	{
		m_cp = cp;
		Start();
	}

	[[nodiscard]] int GetQueueId() const { return m_queue_id; }
	void              SetQueueId(int id) { m_queue_id = id; }

	void SetActive(bool flag);
	bool IsActive();

private:
	void Start()
	{
		Core::Thread t(ThreadRun, this);
		t.Detach();
	}

	static void ThreadRun(void* data);

	Core::Mutex   m_mutex;
	Core::CondVar m_cond_var;
	Core::CondVar m_idle_cond_var;
	bool          m_done = true;
	bool          m_idle = true;

	CommandProcessor* m_cp       = nullptr;
	int               m_queue_id = -1;

	bool      m_active                  = false;
	uint32_t* m_ring_addr               = nullptr;
	uint32_t* m_read_ptr_addr           = nullptr;
	uint32_t* m_internal_buffer         = nullptr;
	uint32_t  m_ring_size_dw            = 0;
	uint32_t  m_run_offset_dw           = 0;
	uint32_t  m_last_dingdong_offset_dw = 0;
};

class Gpu
{
public:
	Gpu()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		Init();
	}
	virtual ~Gpu() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(Gpu);

	void     Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
	                GraphicsSubmissionCompletion completion);
	void     SubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
	                       int index, int flip_mode, int64_t flip_arg);
	uint32_t MapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr);
	void     Unmap(uint32_t ring_id);
	void     DingDong(uint32_t ring_id, uint32_t offset_dw);
	void     Done();
	void     Wait();
	void     WaitSubmission(SubmissionId submission);
	bool     RunQuiesced(GraphicsRunQuiescedAction action, void* data);
	int      GetFrameNum();
	bool     AreSubmitsAllowed();

private:
	void Init();
	void WaitLocked();

	ComputeRing* GetRing(uint32_t ring_id);

	GpuSubmissionAdmissionGate m_submission_admission_gate;
	std::mutex                 m_topology_mutex;
	GpuSubmissionCoordinator   m_submission_coordinator;

	CommandProcessor* m_gfx_cp   = nullptr;
	GraphicsRing*     m_gfx_ring = nullptr;

	CommandProcessor* m_compute_cp[8]    = {};
	ComputeRing*      m_compute_ring[64] = {};

	std::atomic_int m_done_num = 0;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSRUNINTERNAL_H_ */
