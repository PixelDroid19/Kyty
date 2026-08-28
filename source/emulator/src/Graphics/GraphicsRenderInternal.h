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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
struct RenderColorInfo;

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
	bool                       depth_clip_enable       = true;
	bool                       depth_clamp_enable      = false;

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
	                                   const int* textures2d_sampled_view, VulkanImage** textures2d_sampled_depth,
	                                   const int* textures2d_sampled_depth_view, VulkanImage** textures2d_array_sampled,
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
		VulkanBufferDescriptorKey storage_buffers[BUFFERS_MAX]             = {};
		int                  textures2d_sampled_num                        = 0;
		uint64_t             textures2d_sampled_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_sampled_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_sampled_depth_num                        = 0;
		uint64_t             textures2d_sampled_depth_id[TEXTURES_SAMPLED_MAX]   = {};
		uint8_t              textures2d_sampled_depth_view[TEXTURES_SAMPLED_MAX] = {};
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
		VulkanBufferDescriptorKey vsharp_uniform_buffer_key                = {};
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
	uint64_t  GetSamplerId(const ShaderSamplerResource& r, State::ImageSampleOperation operation, bool allow_unnormalized = true);
	void      DeleteAllForTesting(GraphicContext* ctx);

private:
	struct Sampler
	{
		ShaderSamplerResource r;
		State::ImageSampleOperation operation {};
		bool                  allow_unnormalized = true;
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
	VkAttachmentLoadOp        depth_load_op                     = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkImageLayout             depth_initial_layout              = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageLayout             depth_stencil_layout              = VK_IMAGE_LAYOUT_UNDEFINED;
	VkExtent2D                 extent                            = {};
	VkImageView                owned_color_view[TARGETS_MAX]     = {};
};

enum class DepthStencilAttachmentAccess
{
	Writable,
	ReadOnly,
	Unsupported,
};

DepthStencilAttachmentAccess ResolveDepthStencilAttachmentAccess(const RenderDepthInfo& depth, bool sampled_in_same_draw,
                                                                 bool load_store_op_none_supported);

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
		VkAttachmentLoadOp depth_load_op           = VK_ATTACHMENT_LOAD_OP_LOAD;
		VkImageLayout      depth_initial_layout    = VK_IMAGE_LAYOUT_UNDEFINED;
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

class VertexClipProbeRenderer
{
public:
	VertexClipProbeRenderer() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~VertexClipProbeRenderer() = default;
	KYTY_CLASS_NO_COPY(VertexClipProbeRenderer);

	[[nodiscard]] bool Init(GraphicContext* ctx);
	// Test contexts own their VkDevice. Refuse teardown while the selected draw
	// has not completed its exact command-buffer fence.
	[[nodiscard]] bool Done(GraphicContext* ctx);
	[[nodiscard]] bool Reserve(GraphicContext* ctx, CommandBuffer* buffer, uint64_t checksum, bool indexed,
	                           uint32_t guest_count, uint32_t descriptor_set, uint64_t pixel_checksum = 0,
	                           bool vertex_probe_enabled = true, bool pixel_probe_enabled = false,
	                           bool fixed_test_state_known = false, bool depth_test_enabled = false,
	                           bool stencil_test_enabled = false, bool depth_bounds_test_enabled = false,
	                           ShaderPixelProbeKind pixel_probe_kind = ShaderPixelProbeKind::None,
	                           uint32_t pixel_probe_ordinal = 0, uint32_t match_ordinal = 0,
	                           uint32_t pixel_probe_target = 0, bool pixel_probe_sparse = false,
	                           bool pixel_probe_attachment_readback = false,
	                           uint32_t pixel_probe_attachment_min_invocations = 1u,
	                           const RenderColorInfo* attachment_color = nullptr,
	                           VkAttachmentLoadOp attachment_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                           VkImageLayout attachment_initial_layout = VK_IMAGE_LAYOUT_UNDEFINED);
	void               Arm(CommandBuffer* buffer, VkPipelineLayout pipeline_layout);
	void               CaptureAttachmentBeforePass(CommandBuffer* buffer);
	void               BeginDepthPassQuery(CommandBuffer* buffer);
	void               EndDepthPassQuery(CommandBuffer* buffer);
	void               Finish(CommandBuffer* buffer);
	void               Complete(CommandBuffer* buffer);

	[[nodiscard]] VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_descriptor_set_layout; }

private:
	struct PendingDraw
	{
		CommandBuffer* buffer         = nullptr;
		uint64_t       checksum       = 0;
		uint64_t       pixel_checksum = 0;
		bool           indexed        = false;
		bool           vertex_probe_enabled = false;
		bool           pixel_probe_enabled  = false;
		ShaderPixelProbeKind pixel_probe_kind = ShaderPixelProbeKind::None;
		uint32_t       pixel_probe_ordinal = 0;
		uint32_t       pixel_probe_target  = 0;
		uint32_t       match_ordinal       = 0;
		bool           pixel_probe_sparse  = false;
		bool           attachment_readback_requested = false;
		uint32_t       attachment_min_invocations     = 1u;
		bool           attachment_before_recorded    = false;
		bool           attachment_readback_recorded  = false;
		VertexClipProbeAttachmentStatus attachment_readback_status = VertexClipProbeAttachmentStatus::TargetUnavailable;
		VertexClipProbeAttachmentStatus attachment_delta_status    = VertexClipProbeAttachmentStatus::InvalidData;
		VulkanImage*                    attachment_readback_image  = nullptr;
		uint64_t                        attachment_guest_addr      = 0;
		VertexClipProbeAttachmentFormat attachment_readback_format = VertexClipProbeAttachmentFormat::Unsupported;
		VkAttachmentLoadOp               attachment_load_op        = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		VkImageLayout                    attachment_initial_layout  = VK_IMAGE_LAYOUT_UNDEFINED;
		uint32_t                         attachment_readback_width  = 0;
		uint32_t                         attachment_readback_height = 0;
		uint64_t                         attachment_readback_bytes  = 0;
		bool           depth_query_active   = false;
		bool           depth_query_recorded = false;
		bool           fixed_test_state_known = false;
		bool           depth_test_enabled     = false;
		bool           stencil_test_enabled   = false;
		bool           depth_bounds_test_enabled = false;
		uint32_t       guest_count    = 0;
		uint32_t       descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	};

	static constexpr uint64_t kRawStatsBytes = sizeof(VertexClipProbeRawStats);

	[[nodiscard]] bool InitLocked(GraphicContext* ctx);
	void               DoneLocked(GraphicContext* ctx);
	void               InitializeRawStatsLocked();
	void               LogCompletedRawStatsLocked(const VertexClipProbeRawStats& stats);
	void               LogCompletedAttachmentReadbackLocked(const VertexClipProbeRawStats& stats);

	Core::Mutex              m_mutex;
	VertexClipProbeLifecycle m_lifecycle;
	GraphicContext*          m_context               = nullptr;
	VulkanBuffer             m_raw_stats_buffer;
	VulkanBuffer             m_attachment_before_readback_buffer;
	VulkanBuffer             m_attachment_readback_buffer;
	uint32_t                 m_attachment_empty_retries = 0;
	VkDescriptorSetLayout    m_descriptor_set_layout = nullptr;
	VkDescriptorPool         m_descriptor_pool       = nullptr;
	VkDescriptorSet          m_descriptor_set        = nullptr;
	VkQueryPool              m_depth_pass_query_pool = VK_NULL_HANDLE;
	PendingDraw              m_pending_draw;
};

class RenderContext
{
public:
	RenderContext()
	    : m_pipeline_cache(new PipelineCache), m_descriptor_cache(new DescriptorCache), m_framebuffer_cache(new FramebufferCache),
	      m_sampler_cache(new SamplerCache),
	      m_shader_translation_cache(new ShaderTranslationCache(16384, &SpirvBinaryCacheDefaultStore(), Config::ShaderValidationEnabled())),
	      m_gds_buffer(new GdsBuffer), m_vertex_clip_probe_renderer(new VertexClipProbeRenderer)
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
	VertexClipProbeRenderer* GetVertexClipProbeRenderer() { return m_vertex_clip_probe_renderer; }
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
	VertexClipProbeRenderer* m_vertex_clip_probe_renderer = nullptr;
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

// Opt-in PS bind probe. KYTY_TRACE_DRAW_PS is a hex checksum, a comma-separated
// list of up to four checksums, or "*" / "all" for a unique-checksum census.
// Limit is always clamped to [1, 32].
struct DrawPsTraceConfig
{
	bool     enabled         = false;
	bool     census          = false;
	uint64_t checksum        = 0;
	uint64_t checksums[4]    = {};
	int      checksum_count  = 0;
	uint32_t limit           = 32;
};

struct RenderTargetLifetimeDepthFilter
{
	bool     enabled                  = false;
	bool     address_enabled          = false;
	uint64_t depth_buffer_vaddr       = 0;
	bool     extent_enabled           = false;
	uint32_t width                    = 0;
	uint32_t height                   = 0;
	bool     format_enabled           = false;
	uint32_t format                   = 0;
};

struct RenderTargetLifetimeColorAddressFilter
{
	bool     enabled   = false;
	uint64_t base_addr = 0;
};

struct RenderTargetLifetimeColorFormatFilter
{
	bool     enabled = false;
	uint32_t format  = 0;
};

enum class RenderTargetLifetimeAgentArmState: uint8_t
{
	Disabled,
	Idle,
	Pending,
	Open,
};

enum class RenderTargetLifetimeAgentArmRequestResult: uint8_t
{
	Armed,
	Disabled,
	AlreadyPending,
	AlreadyOpen,
};

// Called by the render path before publishing any lifetime trace activity.
// Tests may invoke the same production initialization boundary in a
// process-isolated diagnostics scenario.
void GraphicsInitializeRenderTargetLifetimeTraceOnRenderThread();

[[nodiscard]] inline RenderTargetLifetimeAgentArmRequestResult RenderTargetLifetimeAgentArmRequest(
	std::atomic<RenderTargetLifetimeAgentArmState>* state)
{
	if (state == nullptr)
	{
		return RenderTargetLifetimeAgentArmRequestResult::Disabled;
	}
	auto expected = RenderTargetLifetimeAgentArmState::Idle;
	if (state->compare_exchange_strong(expected, RenderTargetLifetimeAgentArmState::Pending,
	                                   std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return RenderTargetLifetimeAgentArmRequestResult::Armed;
	}
	switch (expected)
	{
		case RenderTargetLifetimeAgentArmState::Pending:
			return RenderTargetLifetimeAgentArmRequestResult::AlreadyPending;
		case RenderTargetLifetimeAgentArmState::Open:
			return RenderTargetLifetimeAgentArmRequestResult::AlreadyOpen;
		case RenderTargetLifetimeAgentArmState::Disabled:
		case RenderTargetLifetimeAgentArmState::Idle:
		default: return RenderTargetLifetimeAgentArmRequestResult::Disabled;
	}
}

[[nodiscard]] inline bool RenderTargetLifetimeAgentArmGateOpen(
	std::atomic<RenderTargetLifetimeAgentArmState>* state, bool render_eligible)
{
	if (state == nullptr)
	{
		return true;
	}
	auto current = state->load(std::memory_order_acquire);
	if (current == RenderTargetLifetimeAgentArmState::Disabled || current == RenderTargetLifetimeAgentArmState::Open)
	{
		return true;
	}
	if (current != RenderTargetLifetimeAgentArmState::Pending || !render_eligible)
	{
		return false;
	}
	if (state->compare_exchange_strong(current, RenderTargetLifetimeAgentArmState::Open,
	                                   std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return true;
	}
	return current == RenderTargetLifetimeAgentArmState::Open;
}

[[nodiscard]] inline bool ParseRenderTargetLifetimeColorAddressFilter(const char* address,
	                                                                      RenderTargetLifetimeColorAddressFilter* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	if (address == nullptr || address[0] == '\0')
	{
		return true;
	}
	const char* cursor = address;
	uint32_t    base   = 10u;
	if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
	{
		base = 16u;
		cursor += 2;
	}
	if (cursor[0] == '\0')
	{
		return false;
	}
	uint64_t parsed = 0;
	for (; *cursor != '\0'; ++cursor)
	{
		uint32_t digit = 0;
		if (*cursor >= '0' && *cursor <= '9')
		{
			digit = static_cast<uint32_t>(*cursor - '0');
		} else if (base == 16u && *cursor >= 'a' && *cursor <= 'f')
		{
			digit = static_cast<uint32_t>(*cursor - 'a') + 10u;
		} else if (base == 16u && *cursor >= 'A' && *cursor <= 'F')
		{
			digit = static_cast<uint32_t>(*cursor - 'A') + 10u;
		} else
		{
			return false;
		}
		if (digit >= base || parsed > (UINT64_MAX - digit) / base)
		{
			return false;
		}
		parsed = parsed * base + digit;
	}
	if (parsed == 0)
	{
		return false;
	}
	out->enabled   = true;
	out->base_addr = parsed;
	return true;
}

[[nodiscard]] inline bool RenderTargetLifetimeColorAddressFilterMatches(
	const RenderTargetLifetimeColorAddressFilter& filter, uint64_t base_addr)
{
	return !filter.enabled || filter.base_addr == base_addr;
}

[[nodiscard]] inline bool ParseRenderTargetLifetimeColorFormatFilter(const char* format,
	                                                                  RenderTargetLifetimeColorFormatFilter* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	if (format == nullptr || format[0] == '\0')
	{
		return true;
	}
	const char* cursor = format;
	uint32_t    base   = 10u;
	if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
	{
		base = 16u;
		cursor += 2;
	}
	if (cursor[0] == '\0')
	{
		return false;
	}
	uint64_t parsed = 0;
	for (; *cursor != '\0'; ++cursor)
	{
		uint32_t digit = 0;
		if (*cursor >= '0' && *cursor <= '9')
		{
			digit = static_cast<uint32_t>(*cursor - '0');
		} else if (base == 16u && *cursor >= 'a' && *cursor <= 'f')
		{
			digit = static_cast<uint32_t>(*cursor - 'a') + 10u;
		} else if (base == 16u && *cursor >= 'A' && *cursor <= 'F')
		{
			digit = static_cast<uint32_t>(*cursor - 'A') + 10u;
		} else
		{
			return false;
		}
		if (digit >= base || parsed > (UINT32_MAX - digit) / base)
		{
			return false;
		}
		parsed = parsed * base + digit;
	}
	if (parsed == 0)
	{
		return false;
	}
	out->enabled = true;
	out->format  = static_cast<uint32_t>(parsed);
	return true;
}

[[nodiscard]] inline bool RenderTargetLifetimeColorFormatFilterMatches(
	const RenderTargetLifetimeColorFormatFilter& filter, uint32_t format)
{
	return !filter.enabled || filter.format == format;
}

[[nodiscard]] inline bool RenderTargetLifetimeColorSelectorEnabled(
	const RenderTargetLifetimeColorAddressFilter& address_filter,
	const RenderTargetLifetimeColorFormatFilter& format_filter)
{
	return address_filter.enabled || format_filter.enabled;
}

[[nodiscard]] inline bool RenderTargetLifetimeColorSelectorMatches(
	const RenderTargetLifetimeColorAddressFilter& address_filter,
	const RenderTargetLifetimeColorFormatFilter& format_filter, uint64_t base_addr, uint32_t format)
{
	return RenderTargetLifetimeColorAddressFilterMatches(address_filter, base_addr) &&
	       RenderTargetLifetimeColorFormatFilterMatches(format_filter, format);
}

[[nodiscard]] inline bool RenderTargetLifetimeTraceSelectorsCompatible(
	const RenderTargetLifetimeDepthFilter& depth_filter,
	const RenderTargetLifetimeColorAddressFilter& color_address_filter,
	const RenderTargetLifetimeColorFormatFilter& color_format_filter, bool probe_color_target)
{
	const bool color_selector = RenderTargetLifetimeColorSelectorEnabled(color_address_filter, color_format_filter);
	return !(probe_color_target && color_selector) && !(depth_filter.enabled && (color_selector || probe_color_target));
}

[[nodiscard]] inline bool ParseRenderTargetLifetimeAfterCapture(const char* value, uint32_t* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = 0;
	if (value == nullptr || value[0] == '\0')
	{
		return true;
	}
	if (value[0] < '1' || value[0] > '9')
	{
		return false;
	}
	uint64_t parsed = 0;
	for (const char* cursor = value; *cursor != '\0'; ++cursor)
	{
		if (*cursor < '0' || *cursor > '9')
		{
			return false;
		}
		const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
		if (parsed > (UINT32_MAX - digit) / 10u)
		{
			return false;
		}
		parsed = parsed * 10u + digit;
	}
	*out = static_cast<uint32_t>(parsed);
	return true;
}

[[nodiscard]] constexpr bool RenderTargetLifetimeAfterCaptureEligible(uint32_t ordinal, uint64_t baseline_capture_count,
	                                                                   uint64_t successful_capture_count)
{
	return ordinal == 0u || (successful_capture_count >= baseline_capture_count &&
	                         successful_capture_count - baseline_capture_count >= ordinal);
}

[[nodiscard]] inline bool ParseRenderTargetLifetimeDepthFilter(const char* address, const char* extent, const char* format,
	                                                             RenderTargetLifetimeDepthFilter* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	RenderTargetLifetimeDepthFilter result {};
	if (address != nullptr && address[0] != '\0')
	{
		const char* cursor = address;
		if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
		{
			cursor += 2;
		}
		if (cursor[0] == '\0')
		{
			return false;
		}
		uint64_t parsed = 0;
		for (; *cursor != '\0'; ++cursor)
		{
			uint32_t digit = 0;
			if (*cursor >= '0' && *cursor <= '9')
			{
				digit = static_cast<uint32_t>(*cursor - '0');
			} else if (*cursor >= 'a' && *cursor <= 'f')
			{
				digit = static_cast<uint32_t>(*cursor - 'a') + 10u;
			} else if (*cursor >= 'A' && *cursor <= 'F')
			{
				digit = static_cast<uint32_t>(*cursor - 'A') + 10u;
			} else
			{
				return false;
			}
			if (parsed > (UINT64_MAX - digit) / 16u)
			{
				return false;
			}
			parsed = parsed * 16u + digit;
		}
		if (parsed == 0)
		{
			return false;
		}
		result.address_enabled    = true;
		result.depth_buffer_vaddr = parsed;
	}
	if (extent != nullptr && extent[0] != '\0')
	{
		const char* cursor = extent;
		uint32_t    values[2] {};
		for (int i = 0; i < 2; ++i)
		{
			if (*cursor < '1' || *cursor > '9')
			{
				return false;
			}
			uint64_t parsed = 0;
			for (; *cursor >= '0' && *cursor <= '9'; ++cursor)
			{
				const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
				if (parsed > (UINT32_MAX - digit) / 10u)
				{
					return false;
				}
				parsed = parsed * 10u + digit;
			}
			values[i] = static_cast<uint32_t>(parsed);
			if (i == 0 && *cursor++ != 'x')
			{
				return false;
			}
		}
		if (*cursor != '\0')
		{
			return false;
		}
		result.extent_enabled = true;
		result.width          = values[0];
		result.height         = values[1];
	}
	if (format != nullptr && format[0] != '\0')
	{
		uint64_t parsed = 0;
		for (const char* cursor = format; *cursor != '\0'; ++cursor)
		{
			if ((*cursor < '0' || *cursor > '9') || (cursor == format && *cursor == '0'))
			{
				return false;
			}
			const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
			if (parsed > (UINT32_MAX - digit) / 10u)
			{
				return false;
			}
			parsed = parsed * 10u + digit;
		}
		result.format_enabled = true;
		result.format         = static_cast<uint32_t>(parsed);
	}
	result.enabled = result.address_enabled || result.extent_enabled || result.format_enabled;
	*out           = result;
	return true;
}

[[nodiscard]] inline bool RenderTargetLifetimeDepthFilterMatches(const RenderTargetLifetimeDepthFilter& filter,
	                                                                uint64_t depth_buffer_vaddr, uint32_t width, uint32_t height,
	                                                                uint32_t format)
{
	return (!filter.address_enabled || filter.depth_buffer_vaddr == depth_buffer_vaddr) &&
	       (!filter.extent_enabled || (filter.width == width && filter.height == height)) &&
	       (!filter.format_enabled || filter.format == format);
}

struct RenderTargetLifetimeDepthAddressTraceState
{
	uint64_t guest_addr    = 0;
	uint64_t host_id       = 0;
	int      armed_present = -1;
};

struct RenderTargetLifetimeDepthAddressArmTransition
{
	bool accepted    = false;
	bool newly_armed = false;
	bool remapped    = false;
};

[[nodiscard]] inline RenderTargetLifetimeDepthAddressArmTransition RenderTargetLifetimeDepthAddressTraceArm(
	const RenderTargetLifetimeDepthFilter& filter, uint64_t depth_buffer_vaddr, uint32_t width, uint32_t height, uint32_t format,
	uint64_t host_id, int present,
	RenderTargetLifetimeDepthAddressTraceState* state)
{
	RenderTargetLifetimeDepthAddressArmTransition result {};
	if (state == nullptr || depth_buffer_vaddr == 0 || host_id == 0 ||
	    !RenderTargetLifetimeDepthFilterMatches(filter, depth_buffer_vaddr, width, height, format))
	{
		return result;
	}
	result.accepted    = true;
	result.newly_armed = state->guest_addr == 0 && state->host_id == 0;
	result.remapped    = !result.newly_armed &&
	                  (state->guest_addr != depth_buffer_vaddr || state->host_id != host_id);
	if (result.newly_armed || result.remapped)
	{
		state->guest_addr    = depth_buffer_vaddr;
		state->host_id       = host_id;
		state->armed_present = present;
	}
	return result;
}

[[nodiscard]] inline bool RenderTargetLifetimeDepthAddressTraceUseEligible(
	const RenderTargetLifetimeDepthFilter& filter, uint64_t depth_buffer_vaddr, uint32_t width, uint32_t height, uint32_t format,
	uint64_t host_id, int present,
	const RenderTargetLifetimeDepthAddressTraceState& state)
{
	return RenderTargetLifetimeDepthFilterMatches(filter, depth_buffer_vaddr, width, height, format) &&
	       state.guest_addr == depth_buffer_vaddr && state.host_id == host_id && present > state.armed_present;
}

[[nodiscard]] constexpr uint32_t DrawMaterialTraceVertexAttributeByteSize(uint32_t format)
{
	return format == 74u ? 12u : ((format == 64u || format == 71u) ? 8u : (format == 29u ? 4u : 0u));
}

bool DecodeDrawMaterialTraceVertexAttribute(uint64_t address, uint32_t format, float* values, uint32_t* components,
                                            uint32_t* bytes);

[[nodiscard]] inline bool DrawPsTraceChecksumMatch(const DrawPsTraceConfig& config, uint64_t checksum)
{
	if (config.census)
	{
		return true;
	}
	for (int i = 0; i < config.checksum_count && i < 4; ++i)
	{
		if (config.checksums[i] == checksum)
		{
			return true;
		}
	}
	return config.checksum_count == 0 && config.checksum != 0 && config.checksum == checksum;
}

[[nodiscard]] inline bool ParseDrawPsTraceConfig(const char* value, const char* limit, DrawPsTraceConfig* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	out->limit = 32u;
	if (value == nullptr || value[0] == '\0')
	{
		return true;
	}
	if (std::strcmp(value, "*") == 0 || std::strcmp(value, "all") == 0 || std::strcmp(value, "ALL") == 0)
	{
		out->enabled = true;
		out->census  = true;
	}
	else
	{
		const char* cursor = value;
		while (*cursor != '\0' && out->checksum_count < 4)
		{
			char* end = nullptr;
			const uint64_t checksum = std::strtoull(cursor, &end, 16);
			if (end == cursor || (*end != '\0' && *end != ','))
			{
				*out       = {};
				out->limit = 32u;
				return true;
			}
			out->checksums[out->checksum_count++] = checksum;
			cursor                                = (*end == ',') ? end + 1 : end;
		}
		if (out->checksum_count == 0)
		{
			return true;
		}
		out->enabled  = true;
		out->checksum = out->checksums[0];
	}
	if (limit != nullptr && limit[0] != '\0')
	{
		char* limit_end = nullptr;
		const unsigned long parsed = std::strtoul(limit, &limit_end, 10);
		if (limit_end != limit && *limit_end == '\0')
		{
			out->limit = std::clamp(static_cast<uint32_t>(parsed), 1u, 32u);
		}
	}
	return true;
}

// Guest type 11 cubemaps are sampled as a 2D array. Sampler ops use the
// RDNA2 [1,2] ST window (unbias by 1) and face_id as the array layer.
struct CubeSampleBind
{
	bool is_cube         = false;
	bool uses_array_view = false;
	bool st_window_unbias = false;
	bool face_as_layer   = false;
};

[[nodiscard]] inline CubeSampleBind ResolveCubeSampleBind(uint8_t guest_type, int view)
{
	CubeSampleBind bind {};
	if (guest_type != 11u)
	{
		return bind;
	}
	bind.is_cube          = true;
	bind.uses_array_view  = view == VulkanImage::VIEW_ARRAY;
	bind.st_window_unbias = true;
	bind.face_as_layer    = true;
	return bind;
}

// Skybox-style coverage: depth test on, no depth write, GEQUAL. Against a
// reverse-Z far of 0 the far sky (z≈0) can pass empty pixels.
struct CubeDrawDepthCoverage
{
	bool     test_enable            = false;
	bool     write_enable           = false;
	bool     clear_enable           = false;
	uint32_t compare_op             = 0;
	bool     far_sky_can_pass_empty = false;
};

[[nodiscard]] inline CubeDrawDepthCoverage ResolveCubeDrawDepthCoverage(bool test_enable, bool write_enable, bool clear_enable,
                                                                        uint32_t compare_op)
{
	CubeDrawDepthCoverage coverage {};
	coverage.test_enable            = test_enable;
	coverage.write_enable           = write_enable;
	coverage.clear_enable           = clear_enable;
	coverage.compare_op             = compare_op;
	coverage.far_sky_can_pass_empty = test_enable && !write_enable && compare_op == static_cast<uint32_t>(VK_COMPARE_OP_GREATER_OR_EQUAL);
	return coverage;
}

// Vulkan forbids unnormalizedCoordinates on cube, 2D-array, and 3D views.
// The restriction is per sampler/view pair: a 2D Dref sampler in the same
// draw as a cubemap may still take ForceUnorm.
[[nodiscard]] inline bool SamplerViewAllowsUnnormalized(ShaderGen5SampledTextureShape shape)
{
	return shape == ShaderGen5SampledTextureShape::TwoDimensional;
}

[[nodiscard]] inline bool BindSamplerAllowsUnnormalized(const ShaderTextureResources& textures, int sampler_slot)
{
	bool matched = false;
	for (int i = 0; i < textures.textures_num; ++i)
	{
		const auto& descriptor = textures.desc[i];
		if (descriptor.usage != ShaderTextureUsage::ReadOnly || descriptor.slot != sampler_slot)
		{
			continue;
		}
		matched = true;
		if (!SamplerViewAllowsUnnormalized(ShaderResolvedSampledTextureShape(descriptor)))
		{
			return false;
		}
	}
	if (matched)
	{
		return true;
	}
	for (int i = 0; i < textures.textures_num; ++i)
	{
		const auto& descriptor = textures.desc[i];
		if (descriptor.usage != ShaderTextureUsage::ReadOnly)
		{
			continue;
		}
		if (!SamplerViewAllowsUnnormalized(ShaderResolvedSampledTextureShape(descriptor)))
		{
			return false;
		}
	}
	return true;
}

// Draw-local context for an opt-in, bounded material-binding diagnostic.
// BindDescriptors only borrows these pointers for the duration of the call.
struct DrawMaterialTraceContext
{
	uint64_t                     ps_checksum             = 0;
	uint64_t                     ps_addr                 = 0;
	uint32_t                     ps_in_control           = 0;
	uint32_t                     ps_required_subgroup    = 0;
	uint64_t                     vs_checksum             = 0;
	uint64_t                     vs_addr                 = 0;
	int                          vs_export_count         = 0;
	uint8_t                      vertex_float_mode       = 0;
	bool                         vertex_dx10_clamp       = false;
	bool                         vertex_ieee_mode        = false;
	bool                         indexed                 = false;
	uint32_t                     primitive_type          = 0;
	uint32_t                     guest_count             = 0;
	uint32_t                     index_type              = 0;
	uint64_t                     index_addr              = 0;
	uint64_t                     index_size              = 0;
	int32_t                      vertex_offset           = 0;
	uint32_t                     instance_count          = 1;
	uint32_t                     first_instance          = 0;
	uint32_t                     required_vertex_records = 0;
	const ShaderVertexInputInfo* vertex_input            = nullptr;
	const RenderColorInfo*       color                   = nullptr;
	const RenderDepthInfo*       depth                   = nullptr;
	const HW::Context*           hardware                = nullptr;
	const VulkanFramebuffer*     framebuffer             = nullptr;
	uint32_t                     declared_vertex_records = 0;
	uint32_t                     target_mask             = 0;
	uint32_t                     blend_enable            = 0;
	uint32_t                     blend_src               = 0;
	uint32_t                     blend_op                = 0;
	uint32_t                     blend_dst               = 0;
	uint32_t                     blend_bypass            = 0;
	uint32_t                     color_mask              = 0;
	uint32_t                     color_on_depth_fail     = 0;
	uint32_t                     color_off_depth_pass    = 0;
	uint32_t                     cull_front              = 0;
	uint32_t                     cull_back               = 0;
	uint32_t                     face                    = 0;
	float                        vp_x                    = 0.0f;
	float                        vp_y                    = 0.0f;
	float                        vp_width                = 0.0f;
	float                        vp_height               = 0.0f;
	float                        vp_zmin                 = 0.0f;
	float                        vp_zmax                 = 1.0f;
	uint32_t                     dx_clip                 = 0;
	uint32_t                     ps_input_num            = 0;
	uint32_t                     interpolators[8]        = {};
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
extern VulkanImage* g_dump_depth_image;
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
                     uint32_t storage_seed_skip_mask = 0, const DrawMaterialTraceContext* material_trace = nullptr);
void TraceRenderTargetLifetimeDraw(uint64_t submit_id, const DrawMaterialTraceContext& draw);
void TraceRenderTargetLifetimePassBegin(uint64_t submit_id, const RenderColorInfo& color,
	                                    const VulkanFramebuffer& framebuffer);
void TraceRenderTargetLifetimeSelectProbeColor(const RenderColorInfo& color, uint32_t slot, uint32_t ordinal);
void TraceRenderTargetLifetimeProbeDepthAttempt(
    uint64_t submit_id, uint64_t ps_checksum, uint64_t vs_checksum, bool indexed, uint32_t guest_count,
    uint32_t match_ordinal, uint32_t mrt_target, uint32_t export_ordinal, const RenderDepthInfo& depth,
    const VulkanFramebuffer& framebuffer, const HW::Context& hardware, const VulkanSampleLocationState& sample_locations);
void TraceRenderTargetLifetimeDepthClearPass(uint64_t submit_id, const RenderDepthInfo& depth,
	                                         const VulkanFramebuffer& framebuffer);
void TraceRenderTargetLifetimeResolve(uint64_t submit_id, const RenderColorInfo& source, const RenderColorInfo& destination);
void SetDynamicParams(VkCommandBuffer vk_buffer, VulkanPipeline* pipeline);


bool GraphicsResolvePrimitiveDrawPlan(uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num, bool indexed,
                                      PrimitiveDrawPlan* plan);
void MaybeDumpPrimitiveDrawPlan(const char* path, uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num, bool indexed,
                                const PrimitiveDrawPlan& plan);
bool AutoDrawModifierSupported(uint64_t draw_modifier);
void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
	                                uint32_t index_count, uint32_t index_type_and_size, const void* index_addr,
	                                uint32_t instance_count, int32_t vertex_offset_add, uint32_t first_instance);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSRENDERINTERNAL_H_ */
