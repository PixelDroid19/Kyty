#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRENDERINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRENDERINTERNAL_H_

// GraphicsRender module map (edit the smallest file that owns the contract):
//
//   GraphicsRenderInternal.h        private types, g_render_ctx, shared helpers
//   GraphicsRenderCore.cpp          color helpers, RT dump, g_render_ctx
//   GraphicsRenderHwCheck.cpp       HW print/check validators
//   GraphicsRenderContext.cpp       Init/CreateContext, RenderContext EOP, Gds, CommandPool
//   GraphicsRenderFramebuffer.cpp   FramebufferCache + SamplerCache::GetSampler
//   GraphicsRenderPipeline.cpp      PipelineCache + CreatePipelineInternal
//   GraphicsRenderDescriptor.cpp    DescriptorCache, Delete*, Find*, stencil helpers
//   GraphicsRenderAttachments.cpp   barriers, describe/materialize, resolution
//   GraphicsRenderBind.cpp          Prepare* resources + BindDescriptors + SetDynamicParams
//   GraphicsRenderDraw.cpp          DrawIndex / DrawIndexAuto / Dispatch / DS copy
//   GraphicsRenderEop.cpp           EOP labels, eq events, memory free/flush
//   GraphicsRenderCommandBuffer.cpp CommandBuffer methods + TransientBufferPool
//   GraphicsRender.cpp              empty stub (legacy path)
//
// Public API remains in include/Emulator/Graphics/GraphicsRender.h

#include "Emulator/Graphics/GraphicsRender.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DepthStencilCopy.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/RenderResolutionPlanner.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Kernel/EventQueue.h"

#include <chrono>
#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Gen5 AGC registers "queued graphics interrupt" as id 0; classic Gnm EOP is 0x40.
// Both ride the same end-of-pipe eq list and must trigger with the registered ident.
constexpr int GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT = 0x00;
constexpr int GRAPHICS_EVENT_EOP                       = 0x40;

inline bool IsGraphicsEopEventId(int id)
{
	return id == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT || id == GRAPHICS_EVENT_EOP;
}

struct Label;
struct RenderDepthInfo;

struct VulkanDescriptor
{
	VkDescriptorSet descriptor_set = nullptr;
};

// Pack structs to guarantee the uniquess of object representation
#pragma pack(push, 1)

struct PipelineStencilStaticState
{
	VkStencilOp failOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp passOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp depthFailOp = VK_STENCIL_OP_KEEP;
	VkCompareOp compareOp   = VK_COMPARE_OP_NEVER;
};

struct PipelineStencilDynamicState
{
	uint32_t compareMask = 0;
	uint32_t writeMask   = 0;
	uint32_t reference   = 0;
};

struct PipelineStaticParameters
{
	VkPrimitiveTopology        topology                 = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	VkSampleCountFlagBits      rasterization_samples    = VK_SAMPLE_COUNT_1_BIT;
	VulkanSampleLocationState  sample_locations;
	bool                       sample_shading_enable    = false;
	bool                       with_depth               = false;
	bool                       depth_test_enable        = false;
	bool                       depth_write_enable       = false;
	VkCompareOp                depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_targets_num       = 1;
	uint32_t                   color_mask[8]           = {};
	bool                       cull_front              = false;
	bool                       cull_back               = false;
	bool                       face                    = false;
	bool                       depth_bias_enable       = false;
	uint8_t                    color_srcblend[8]       = {};
	uint8_t                    color_comb_fcn[8]       = {};
	uint8_t                    color_destblend[8]      = {};
	uint8_t                    alpha_srcblend[8]       = {};
	uint8_t                    alpha_comb_fcn[8]       = {};
	uint8_t                    alpha_destblend[8]      = {};
	bool                       separate_alpha_blend[8] = {};
	bool                       blend_enable[8]         = {};
	bool                       blend_bypass[8]         = {};
	bool                       dx_clip_space           = false;

	bool operator==(const PipelineStaticParameters& other) const;
};

struct PipelineDynamicParameters
{
	bool vk_dynamic_state_line_width             = false;
	bool vk_dynamic_state_stencil_compare_mask   = false;
	bool vk_dynamic_state_stencil_write_mask     = false;
	bool vk_dynamic_state_stencil_reference      = false;
	bool vk_dynamic_state_color_write_enable_ext = false;
	bool vk_dynamic_state_viewport               = false;
	bool vk_dynamic_state_scissor                = false;
	bool vk_dynamic_state_blend_constants        = false;
	bool vk_dynamic_state_depth_bias             = false;

	float line_width         = 1.0f;
	bool  color_write_enable = true;

	float viewport_scale[3]       = {};
	float viewport_offset[3]      = {};
	float viewport_depth_clamp[2] = {0.0f, 1.0f};
	int   scissor_ltrb[4]         = {0};

	float blend_color_red   = 0.0f;
	float blend_color_green = 0.0f;
	float blend_color_blue  = 0.0f;
	float blend_color_alpha = 0.0f;

	float depth_bias_constant_factor = 0.0f;
	float depth_bias_clamp           = 0.0f;
	float depth_bias_slope_factor    = 0.0f;

	PipelineStencilDynamicState stencil_front;
	PipelineStencilDynamicState stencil_back;

	bool operator==(const PipelineDynamicParameters& other) const;
};
#pragma pack(pop)

struct VulkanPipeline
{
	VkPipelineLayout                pipeline_layout = nullptr;
	VkPipeline                      pipeline        = nullptr;
	const PipelineStaticParameters* static_params   = nullptr;
	PipelineDynamicParameters*      dynamic_params  = nullptr;
	VkExtent2D                      framebuffer_extent {};
};

class PipelineCache
{
public:
	PipelineCache() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~PipelineCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(PipelineCache);

	VulkanPipeline* CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
	                               const ShaderVertexInputInfo* vs_input_info, HW::Context* ctx, HW::Shader* sh_ctx,
	                               const ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology,
	                               const VulkanSampleLocationState& sample_locations);
	VulkanPipeline* CreatePipeline(const ShaderComputeInputInfo* input_info, const HW::ComputeShaderInfo* cs_regs,
	                               const HW::ShaderRegisters* sh_regs);
	void            DeletePipeline(VulkanPipeline* pipeline);
	void            DeletePipelines(VulkanFramebuffer* framebuffer);
	void            DeleteAllPipelines();

private:
	static constexpr uint32_t MAX_PIPELINES = 512;

	struct Pipeline
	{
		uint64_t                   render_pass_id = 0;
		ShaderId                   vs_shader_id;
		ShaderId                   ps_shader_id;
		ShaderId                   cs_shader_id;
		VulkanPipeline*            pipeline       = nullptr;
		PipelineStaticParameters*  static_params  = nullptr;
		PipelineDynamicParameters* dynamic_params = nullptr;
	};

	[[nodiscard]] VulkanPipeline* Find(const Pipeline& p) const;

	void DeletePipelineInternal(uint32_t id);
	void SaveDriverCacheIfDue();

	void DumpToFile(Core::File* f, const Pipeline& p);
	void DumpPipeline(const char* action, uint32_t id);

	Vector<Pipeline>                      m_pipelines;
	uint32_t                              m_evict_cursor = 0;
	Core::Mutex                           m_mutex;
	std::chrono::steady_clock::time_point m_last_driver_cache_attempt {};
	bool                                  m_driver_cache_attempted_once         = false;
	bool                                  m_driver_cache_dirty                  = false;
	size_t                                m_driver_cache_bytes_attempted        = 0;
	bool                                  m_driver_cache_write_budget_exhausted = false;
};

struct VulkanDescriptorSet
{
	VkDescriptorSet       set     = nullptr;
	VkDescriptorSetLayout layout  = nullptr;
	int                   pool_id = -1;
};

class DescriptorCache
{
public:
	enum class Stage
	{
		Unknown,
		Vertex,
		Pixel,
		Compute
	};

	static constexpr int BUFFERS_MAX          = ShaderStorageResources::BUFFERS_MAX;
	static constexpr int TEXTURES_SAMPLED_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int TEXTURES_STORAGE_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int SAMPLERS_MAX         = ShaderSamplerResources::RES_MAX;
	static constexpr int PUSH_CONSTANTS_MAX   = static_cast<int>(ShaderBindResources::PORTABLE_PUSH_CONSTANT_BYTES / 4);
	static constexpr int METADATA_DWORDS_MAX  = ShaderStorageResources::BUFFERS_MAX * 4 + ShaderTextureResources::RES_MAX * 8 +
	                                            ShaderSamplerResources::RES_MAX * 4 + 4 + ShaderDirectSgprsResources::SGPRS_MAX;
	static constexpr int GDS_BUFFER_MAX       = 1;

	DescriptorCache() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~DescriptorCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(DescriptorCache);

	VkDescriptorSetLayout GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind);

	VulkanDescriptorSet* Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
	                              int samplers_num, int gds_buffers_num, bool vsharp_uniform_buffer);
	void                 Free(VulkanDescriptorSet* set);

	VulkanDescriptorSet* GetDescriptor(Stage stage, VulkanBuffer** storage_buffers, VulkanImage** textures2d_sampled,
	                                   const int* textures2d_sampled_view, VulkanImage** textures2d_array_sampled,
	                                   const int* textures2d_array_sampled_view, VulkanImage** textures3d_sampled,
	                                   const int* textures3d_sampled_view, VulkanImage** textures2d_sampled_uint,
	                                   const int* textures2d_sampled_uint_view, VulkanImage** textures2d_array_sampled_uint,
	                                   const int* textures2d_array_sampled_uint_view, VulkanImage** textures3d_sampled_uint,
	                                   const int* textures3d_sampled_uint_view, VulkanImage** textures2d_storage,
	                                   const int* textures2d_storage_view, uint64_t* samplers, VulkanBuffer** gds_buffers,
	                                   VulkanBuffer* vsharp_buffer, const ShaderBindResources& bind);
	void                 FreeDescriptor(VulkanBuffer* buffer);
	void                 FreeDescriptor(VulkanImage* image);

private:
	struct Set
	{
		VulkanDescriptorSet* set                                           = nullptr;
		int                  next_free_set                                 = -1;
		uint32_t             hash                                          = 0;
		Stage                stage                                         = Stage::Unknown;
		int                  storage_buffers_num                           = 0;
		uint64_t             storage_buffers_id[BUFFERS_MAX]               = {};
		int                  textures2d_sampled_num                        = 0;
		uint64_t             textures2d_sampled_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_sampled_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_array_sampled_num                        = 0;
		uint64_t             textures2d_array_sampled_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_array_sampled_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures3d_sampled_num                        = 0;
		uint64_t             textures3d_sampled_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures3d_sampled_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_sampled_uint_num                        = 0;
		uint64_t             textures2d_sampled_uint_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_sampled_uint_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_array_sampled_uint_num                        = 0;
		uint64_t             textures2d_array_sampled_uint_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_array_sampled_uint_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures3d_sampled_uint_num                        = 0;
		uint64_t             textures3d_sampled_uint_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures3d_sampled_uint_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_storage_num                        = 0;
		uint64_t             textures2d_storage_id[TEXTURES_STORAGE_MAX]   = {};
		uint8_t              textures2d_storage_view[TEXTURES_STORAGE_MAX] = {};
		int                  samplers_num                                  = 0;
		uint64_t             samplers_id[SAMPLERS_MAX]                     = {};
		int                  gds_buffers_num                               = 0;
		uint64_t             gds_buffers_id[GDS_BUFFER_MAX]                = {};
		bool                 vsharp_uniform_buffer                         = false;
		uint64_t             vsharp_uniform_buffer_id                      = 0;
	};

	struct Pool
	{
		VkDescriptorPool pool           = nullptr;
		int              next_free_pool = -1;
		bool             free           = true;
	};

	VkDescriptorSetLayout GetOrCreateLayout(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
	                                        int samplers_num, int gds_buffers_num, bool vsharp_uniform_buffer);
	void                  CreatePool();

	static uint32_t CalcHash(const Set& s);

	VulkanDescriptorSet* FindSet(const Set& s);

	Core::Mutex  m_mutex;
	Vector<Pool> m_pools;
	Vector<Set>  m_sets;
	int          m_first_free_set  = -1;
	int          m_first_free_pool = -1;

	Core::Hashmap<uint32_t, Vector<int>> m_sets_map;

	VkDescriptorSetLayout m_descriptor_set_layout_vertex[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                    [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1][2] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_pixel[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                   [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1][2] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_compute[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                     [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1][2] = {};
};

class SamplerCache
{
public:
	SamplerCache() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~SamplerCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(SamplerCache);

	VkSampler GetSampler(uint64_t id);
	uint64_t  GetSamplerId(const ShaderSamplerResource& r, State::ImageSampleOperation operation);
	void      DeleteAllForTesting(GraphicContext* ctx);

private:
	struct Sampler
	{
		ShaderSamplerResource r;
		State::ImageSampleOperation operation {};
		VkSampler             vk = nullptr;
	};

	Core::Mutex     m_mutex;
	Vector<Sampler> m_samplers;
};

struct VulkanFramebuffer
{
	static constexpr uint32_t TARGETS_MAX                       = 8;
	VkRenderPass              render_pass                       = nullptr;
	uint64_t                  render_pass_id                    = 0;
	VkFramebuffer             framebuffer                       = nullptr;
	uint32_t                  color_count                       = 0;
	uint32_t                  attachment_count                  = 0;
	uint32_t                  depth_attachment_index            = VK_ATTACHMENT_UNUSED;
	VkAttachmentLoadOp        color_load_op[TARGETS_MAX]        = {};
	VkImageLayout             color_initial_layout[TARGETS_MAX] = {};
	VkImageLayout             depth_stencil_layout              = VK_IMAGE_LAYOUT_UNDEFINED;
	VkExtent2D                 extent                            = {};
	VkImageView                owned_color_view[TARGETS_MAX]     = {};
};

enum class DepthStencilAttachmentAccess
{
	Writable,
	ReadOnly,
};

class FramebufferCache
{
public:
	FramebufferCache() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~FramebufferCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FramebufferCache);

	VulkanFramebuffer* CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth,
	                                     DepthStencilAttachmentAccess depth_stencil_access = DepthStencilAttachmentAccess::Writable);
	void               FreeFramebufferByColor(VulkanImage* image);
	void               FreeFramebufferByDepth(DepthStencilVulkanImage* image);

private:
	VideoOutVulkanImage* CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height, VkSampleCountFlagBits samples);

	struct Framebuffer
	{
		VulkanFramebuffer* framebuffer             = nullptr;
		uint32_t           targets_num             = 0;
		uint64_t           image_id[8]             = {};
		uint32_t           base_array_layer[8]     = {};
		uint32_t           layer_count[8]          = {};
		uint64_t           depth_id                = 0;
		bool               depth_clear_enable      = false;
		bool               stencil_clear_enable    = false;
		bool               depth_stencil_read_only = false;
		VkAttachmentLoadOp color_load_op[8]        = {};
		VkImageLayout      color_initial_layout[8] = {};
	};

	Core::Mutex                  m_mutex;
	Vector<Framebuffer>          m_framebuffers;
	Vector<VideoOutVulkanImage*> m_dummy_buffers;
};

class GdsBuffer
{
public:
	GdsBuffer() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~GdsBuffer() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GdsBuffer);

	void Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

	VulkanBuffer* GetBuffer(GraphicContext* ctx);

private:
	static constexpr uint64_t DW_SIZE = 0x3000;

	void Init(GraphicContext* ctx);

	Core::Mutex   m_mutex;
	VulkanBuffer* m_buffer = nullptr;
};

class RenderContext
{
public:
	RenderContext()
	    : m_pipeline_cache(new PipelineCache), m_descriptor_cache(new DescriptorCache), m_framebuffer_cache(new FramebufferCache),
	      m_sampler_cache(new SamplerCache),
	      m_shader_translation_cache(new ShaderTranslationCache(16384, &SpirvBinaryCacheDefaultStore(), Config::ShaderValidationEnabled())),
	      m_gds_buffer(new GdsBuffer)
	{
		if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); }
	}
	virtual ~RenderContext() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(RenderContext);

	void            SetGraphicCtx(GraphicContext* ctx) { m_graphic_ctx = ctx; }
	GraphicContext* GetGraphicCtx() { return m_graphic_ctx; }

	Core::Mutex&            GetMutex() { return m_mutex; }
	PipelineCache*          GetPipelineCache() { return m_pipeline_cache; }
	DescriptorCache*        GetDescriptorCache() { return m_descriptor_cache; }
	FramebufferCache*       GetFramebufferCache() { return m_framebuffer_cache; }
	SamplerCache*           GetSamplerCache() { return m_sampler_cache; }
	ShaderTranslationCache* GetShaderTranslationCache() { return m_shader_translation_cache; }
	GdsBuffer*              GetGdsBuffer() { return m_gds_buffer; }
	DepthStencilCopyRenderer* GetDepthStencilCopyRenderer()
	{
		if (m_depth_stencil_copy_renderer == nullptr)
		{
			EXIT_IF(m_graphic_ctx == nullptr);
			m_depth_stencil_copy_renderer = new DepthStencilCopyRenderer;
		}
		return m_depth_stencil_copy_renderer;
	}
	void ReleaseDepthStencilCopySource(uint64_t source_id)
	{
		if (m_depth_stencil_copy_renderer != nullptr)
		{
			m_depth_stencil_copy_renderer->ReleaseSource(m_graphic_ctx, source_id);
		}
	}
	void ReleaseDepthStencilCopyRenderPass(uint64_t render_pass_id)
	{
		if (m_depth_stencil_copy_renderer != nullptr)
		{
			m_depth_stencil_copy_renderer->ReleaseRenderPass(m_graphic_ctx, render_pass_id);
		}
	}

	void*        BeginEopEqRegistration(Kernel::EventQueue::KernelEqueueIdentity identity, int id);
	void         PublishEopEqRegistration(void* registration);
	void         CancelEopEqRegistration(void* registration);
	void         DeleteEopEqRegistration(void* registration, Kernel::EventQueue::KernelEqueue eq, int id);
	void         TriggerEopEvent();
	void         TriggerQueuedGraphicsInterrupt();
	Core::Mutex& GetEopRegistrationMutex() { return m_eop_registration_mutex; }

private:
	struct EopEqRegistration
	{
		Kernel::EventQueue::KernelEqueueIdentity identity {};
		int                                         id        = GRAPHICS_EVENT_EOP;
		bool                                        published = false;
		bool                                        deleted   = false;
	};

	enum class CompletionSignal
	{
		EndOfPipe,
		QueuedGraphicsInterrupt,
	};

	void TriggerRegisteredEvents(CompletionSignal signal);

	Core::Mutex             m_mutex;
	PipelineCache*          m_pipeline_cache           = nullptr;
	DescriptorCache*        m_descriptor_cache         = nullptr;
	FramebufferCache*       m_framebuffer_cache        = nullptr;
	SamplerCache*           m_sampler_cache            = nullptr;
	ShaderTranslationCache* m_shader_translation_cache = nullptr;
	GraphicContext*         m_graphic_ctx              = nullptr;
	GdsBuffer*              m_gds_buffer               = nullptr;
	DepthStencilCopyRenderer* m_depth_stencil_copy_renderer = nullptr;

	Core::Mutex                m_eop_registration_mutex;
	Core::Mutex                m_eop_mutex;
	Vector<EopEqRegistration*> m_eop_eqs;
};

struct RenderDepthInfo
{
	VkFormat                    format                   = VK_FORMAT_UNDEFINED;
	VkSampleCountFlagBits       samples                  = VK_SAMPLE_COUNT_1_BIT;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	bool                        htile                    = false;
	bool                        neo                      = false;
	uint64_t                    depth_buffer_size        = 0;
	uint64_t                    depth_buffer_vaddr       = 0;
	uint64_t                    depth_tile_swizzle       = 0;
	uint64_t                    stencil_buffer_size      = 0;
	uint64_t                    stencil_buffer_vaddr     = 0;
	uint64_t                    stencil_tile_swizzle     = 0;
	uint64_t                    htile_buffer_size        = 0;
	uint64_t                    htile_buffer_vaddr       = 0;
	uint64_t                    htile_tile_swizzle       = 0;
	bool                        depth_clear_enable       = false;
	bool                        suppress_depth_write     = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	bool                        depth_write_enable       = false;
	VkCompareOp                 depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                        depth_bounds_test_enable = false;
	float                       depth_min_bounds         = 0.0f;
	float                       depth_max_bounds         = 0.0f;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	bool                        sampled                  = false;
	bool                        update_compression_state = false;
	bool                        compressed_after_draw    = false;
	PipelineStencilStaticState  stencil_static_front;
	PipelineStencilStaticState  stencil_static_back;
	PipelineStencilDynamicState stencil_dynamic_front;
	PipelineStencilDynamicState stencil_dynamic_back;
	DepthStencilVulkanImage*    vulkan_buffer = nullptr;
	uint64_t                    vaddr[3]      = {};
	uint64_t                    size[3]       = {};
	int                         vaddr_num     = 0;
};

enum class RenderColorType
{
	NoColorOutput,
	DisplayBuffer,
	// OffscreenBuffer,
	RenderTexture,
};

// Every color attachment owns its guest layout and host image identity. A
// framebuffer may combine attachments with different formats and backing
// sizes; its render area is the common extent of all attachments and all
// attachments must use the same sample count.
struct RenderColorAttachmentInfo
{
	RenderColorType       type                    = RenderColorType::NoColorOutput;
	VulkanImage*          vulkan_buffer           = nullptr;
	uint64_t              base_addr               = 0;
	bool                  cmask_fast_clear_enable = false;
	uint32_t              clear_word0             = 0;
	uint32_t              clear_word1             = 0;
	RenderTextureFormat   render_texture_format   = RenderTextureFormat::Unknown;
	VideoOutVulkanImage*  existing_video_image    = nullptr;
	uint32_t              width                   = 0;
	uint32_t              height                  = 0;
	VkSampleCountFlagBits samples                 = VK_SAMPLE_COUNT_1_BIT;
	uint32_t              pitch                   = 0;
	uint64_t              size                    = 0;
	uint32_t              image_layers            = 1;
	uint32_t              base_array_layer         = 0;
	uint32_t              layer_count              = 1;
	bool                  tile                    = false;
	bool                  neo                     = false;
	bool                  write_back              = false;
};

struct RenderColorInfo
{
	static constexpr uint32_t TARGETS_MAX = 8;
	uint32_t                  targets_num = 0;
	RenderColorAttachmentInfo attachment[TARGETS_MAX] {};
};

class CommandPool
{
public:
	CommandPool() = default;
	~CommandPool() // NOLINT
	{
		// TODO(): check if destructor is called from std::_Exit()
		// DeleteAll();
	}

	KYTY_CLASS_NO_COPY(CommandPool);

	VulkanCommandPool* GetPool(int id)
	{
		if (m_pool[id] == nullptr)
		{
			Create(id);
		}
		return m_pool[id];
	}

	// Unit-test renderer contexts own no production submissions. Expose their
	// pool teardown only through this private renderer header so a test can
	// release every command buffer before it destroys its VkDevice.
	void DeleteAllForTesting() { DeleteAll(); }

private:
	void Create(int id);
	void DeleteAll();

	VulkanCommandPool* m_pool[GraphicContext::QUEUES_NUM] = {};
};

// Global render context (defined in GraphicsRenderCore.cpp).
struct PrimitiveDrawPlan
{
	VkPrimitiveTopology topology      = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	uint32_t            draw_count    = 0;
	uint32_t            chunk_count   = 0;
	bool                chunked       = false;
	bool                requires_rect = false;
};

extern RenderContext*           g_render_ctx;
extern thread_local CommandPool g_command_pool;

// Test-only context ownership seam. It binds a caller-owned GraphicContext to
// the private renderer globals and unbinds it only after all thread-local
// command-pool resources have been destroyed. Do not use from runtime paths.
[[nodiscard]] bool GraphicsRenderBindContextForTesting(GraphicContext* ctx);
[[nodiscard]] bool GraphicsRenderUnbindContextForTesting(GraphicContext* ctx);


// --- Dump globals (Core) ---
constexpr uint32_t k_dump_rt_slots = 4;
extern VulkanImage* g_dump_rt_images[k_dump_rt_slots];
extern uint32_t     g_dump_rt_count;
extern VulkanImage* g_dump_bc3_image;
extern VulkanImage* g_dump_bc3_compute_source;
extern VulkanImage* g_dump_bc3_compute_destination;

// --- HwCheck detail validators used by draw paths ---
void uc_print(const char* func, const HW::UserConfig& uc);
void uc_check(const HW::UserConfig& uc);
void sh_print(const char* func, const HW::Shader& sh);
void sh_check(const HW::Shader& sh);
void aa_check_for_attachment_samples(const HW::Context& hw, VkSampleCountFlagBits attachment_samples,
                                     VulkanSampleLocationState* sample_locations);

// Internal image barriers (VkCommandBuffer overloads; public API uses CommandBuffer*).
void GraphicsRenderRenderTextureBarrier(VkCommandBuffer vk_buffer, VulkanImage* image);
void GraphicsRenderDepthStencilBarrier(VkCommandBuffer vk_buffer, VulkanImage* image);
void GraphicsRenderStorageImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image);

// --- Core (color / dump) ---

void FormatTextureList(const ShaderTextureResources& textures, char* buffer, size_t buffer_size);
VulkanBuffer* TryUploadTransientReadOnlyBuffer(CommandBuffer* buffer, uint64_t addr, uint64_t size, bool read_only,
                                               VkBufferUsageFlags usage);

bool                            RenderColorSlotConfigured(const RenderColorInfo& color, uint32_t slot);
bool                            RenderColorSlotActive(const RenderColorInfo& color, uint32_t slot);
bool                            RenderColorHasActiveTarget(const RenderColorInfo& color);
const RenderColorAttachmentInfo* RenderColorFirstConfiguredAttachment(const RenderColorInfo& color);
VulkanImage*                    RenderColorFirstActiveImage(const RenderColorInfo& color);
VkSampleCountFlagBits           decode_guest_sample_count(uint32_t encoded);
VkSampleCountFlagBits           resolve_render_attachment_sample_count(const RenderColorInfo& color, const RenderDepthInfo& depth);
VkExtent2D                      IntersectFramebufferAttachmentExtent(VkExtent2D current, const VulkanImage* attachment);
void                            SanitizeRenderDepthAgainstColor(RenderColorInfo* color, RenderDepthInfo* depth);
void                            MaybeDumpUiDraw(const RenderColorInfo& color, const ShaderVertexInputInfo& vs_input,
                                                const ShaderPixelInputInfo& ps_input, const HW::Context& hw, const HW::UserConfig& ucfg,
                                                uint32_t index_count, uint32_t index_type_and_size, bool indexed, uint32_t flags = 0);
void                            MaybeDumpColorTargets(GraphicContext* ctx, const RenderColorInfo& color);
bool                            DumpDrawFrameSelected();

// --- HwCheck ---
void hw_check(const HW::Context& hw, bool allow_depth_stencil_copy = false);
void hw_print(const HW::Context& hw);

// --- Descriptor / stencil / Find* ---
void get_stencil_state(PipelineStencilStaticState* s, PipelineStencilDynamicState* d, uint8_t func, uint8_t fail, uint8_t zpass,
                       uint8_t zfail, uint8_t testval, uint8_t mask, uint8_t writemask, uint8_t opval);
Vector<RenderTextureVulkanImage*>  FindRenderTexture(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact);
Vector<StorageTextureVulkanImage*> FindStorageTexture(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact);
Vector<DepthStencilVulkanImage*>   FindDepthStencil(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact);

// --- Attachments ---
void DescribeRenderDepthInfo(const HW::Context& hw, RenderDepthInfo* r);
void MaterializeRenderDepthInfo(uint64_t submit_id, CommandBuffer* buffer, RenderDepthInfo* r, uint32_t host_width = 0,
                                uint32_t host_height = 0, const VulkanSampleLocationState* sample_locations = nullptr);
void DescribeRenderColorInfo(CommandBuffer* buffer, const HW::Context& hw, RenderColorInfo* r);
void NormalizeRenderColorArrayBackings(RenderColorInfo* color);
void MaterializeRenderColorInfo(uint64_t submit_id, CommandBuffer* buffer, RenderColorInfo* r);
void InvalidateMemoryObject(const RenderColorInfo& r);
void InvalidateMemoryObject(const RenderDepthInfo& r);
bool GraphicsRenderColorResolve(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw);
RenderResolutionPlan PrepareDepthOnlyDisplayResolutionCohort(CommandBuffer* buffer, const RenderColorInfo& color,
                                                             const RenderDepthInfo& depth);
RenderResolutionPlan PrepareDisplayResolutionCohort(CommandBuffer* buffer, RenderColorInfo* color, const RenderDepthInfo& depth,
                                                    ShaderPixelInputInfo* ps);
void RequireSupportedRenderResolutionPlan(const RenderResolutionPlan& decision);
void CommitMaterializedRenderResolutionPlan(const RenderResolutionPlan& decision, const RenderColorInfo& color,
                                            const RenderDepthInfo& depth);

// --- Bind ---
void BindVertexBuffers(uint64_t submit_id, CommandBuffer* buffer, VkCommandBuffer vk_buffer, const ShaderVertexInputInfo& input,
	                   uint32_t required_records);
void BindDescriptors(uint64_t submit_id, CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                     const ShaderBindResources& bind, VkShaderStageFlags vk_stage, DescriptorCache::Stage stage,
                     uint32_t storage_seed_skip_mask = 0);
void SetDynamicParams(VkCommandBuffer vk_buffer, VulkanPipeline* pipeline);


bool GraphicsResolvePrimitiveDrawPlan(uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num, bool indexed,
                                      PrimitiveDrawPlan* plan);
void MaybeDumpPrimitiveDrawPlan(const char* path, uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num, bool indexed,
                                const PrimitiveDrawPlan& plan);
bool AutoDrawModifierSupported(uint64_t draw_modifier);
void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                    uint32_t index_count, uint32_t index_type_and_size, const void* index_addr);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSRENDERINTERNAL_H_ */
