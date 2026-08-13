#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hash.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"


// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// DescriptorCache, DeleteFramebuffer/Descriptor, stencil + Find* helpers

static bool IsDepthSampledView(int view)
{
	return view == VulkanImage::VIEW_DEPTH_TEXTURE || view == VulkanImage::VIEW_DEPTH_TEXTURE_ARRAY;
}

static void create_layout(GraphicContext* gctx, int storage_buffers_num, int sampled_descriptor_num, int textures2d_storage_num,
                          int samplers_num, int gds_buffers_num, bool vsharp_uniform_buffer, VkShaderStageFlags stage,
                          VkDescriptorSetLayout* dst)
{
	uint32_t binding_num = 0;

	ShaderBindResources tmp {};
	tmp.storage_buffers.buffers_num = storage_buffers_num;
	tmp.textures2D.textures_num     = sampled_descriptor_num + textures2d_storage_num;
	tmp.samplers.samplers_num       = samplers_num;
	tmp.gds_pointers.pointers_num   = gds_buffers_num;

	ShaderCalcBindingIndices(&tmp);

	constexpr uint32_t B_MAX = 13;

	VkDescriptorSetLayoutBinding ubo_layout_binding[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.storage_buffers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = storage_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_depth_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_uint_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_array_uint_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_3d_uint_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_storage_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = textures2d_storage_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.samplers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER;
		ubo_layout_binding[binding_num].descriptorCount    = samplers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.gds_pointers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = gds_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_array_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (sampled_descriptor_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_3d_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = sampled_descriptor_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (vsharp_uniform_buffer)
	{
		EXIT_IF(binding_num >= B_MAX);
		// Texture descriptors reserve three consecutive bindings even when this
		// particular shader stage only uses sampled or storage images. Keep that
		// sparse numbering for the metadata UBO; using the count of layout entries
		// would collide with a storage-image binding.
		uint32_t vsharp_binding = 0;
		if (storage_buffers_num > 0)
		{
			vsharp_binding = tmp.storage_buffers.binding_index + 1;
		}
		if (tmp.textures2D.textures_num > 0)
		{
			vsharp_binding = tmp.textures2D.binding_sampled_depth_index + 1;
		}
		if (samplers_num > 0)
		{
			vsharp_binding = tmp.samplers.binding_index + 1;
		}
		if (gds_buffers_num > 0)
		{
			vsharp_binding = tmp.gds_pointers.binding_index + 1;
		}
		ubo_layout_binding[binding_num].binding            = vsharp_binding;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = 1;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (binding_num > 0)
	{
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.pNext        = nullptr;
		layout_info.flags        = 0;
		layout_info.bindingCount = binding_num;
		layout_info.pBindings    = ubo_layout_binding;

		EXIT_IF(*dst != nullptr);

		vkCreateDescriptorSetLayout(gctx->device, &layout_info, nullptr, dst);

		if (*dst == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: *dst == nullptr condition ignored (continuing)\n"); }
	} else
	{
		*dst = nullptr;
	}
};

VkDescriptorSetLayout DescriptorCache::GetOrCreateLayout(Stage stage, int storage_buffers_num, int sampled_descriptor_num,
                                                         int textures2d_storage_num, int samplers_num, int gds_buffers_num,
                                                         bool vsharp_uniform_buffer)
{
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	VkDescriptorSetLayout* layout = nullptr;
	VkShaderStageFlags     vk_stage {};
	switch (stage)
	{
		case Stage::Vertex:
			layout   = &m_descriptor_set_layout_vertex[storage_buffers_num][sampled_descriptor_num][textures2d_storage_num][samplers_num]
			                                          [gds_buffers_num][vsharp_uniform_buffer ? 1 : 0];
			vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
			break;
		case Stage::Pixel:
			layout   = &m_descriptor_set_layout_pixel[storage_buffers_num][sampled_descriptor_num][textures2d_storage_num][samplers_num]
			                                         [gds_buffers_num][vsharp_uniform_buffer ? 1 : 0];
			vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			break;
		case Stage::Compute:
			layout   = &m_descriptor_set_layout_compute[storage_buffers_num][sampled_descriptor_num][textures2d_storage_num][samplers_num]
			                                           [gds_buffers_num][vsharp_uniform_buffer ? 1 : 0];
			vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		default: KYTY_LOG_DEBUG("WARNING: unknown shader stage (continuing)\n"); break;
	}

	if (*layout == nullptr)
	{
		create_layout(gctx, storage_buffers_num, sampled_descriptor_num, textures2d_storage_num, samplers_num, gds_buffers_num,
		              vsharp_uniform_buffer, vk_stage, layout);
	}
	return *layout;
}

void DescriptorCache::CreatePool()
{
	KYTY_PROFILER_BLOCK("DescriptorCache::CreatePool");

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	static const uint32_t max_sets = 512;

	VkDescriptorPoolSize pool_size[5];
	pool_size[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size[0].descriptorCount = max_sets * (BUFFERS_MAX + GDS_BUFFER_MAX);
	pool_size[1].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_size[1].descriptorCount = max_sets * TEXTURES_SAMPLED_MAX * 7;
	pool_size[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_size[2].descriptorCount = max_sets * TEXTURES_STORAGE_MAX;
	pool_size[3].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
	pool_size[3].descriptorCount = max_sets * SAMPLERS_MAX;
	pool_size[4].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_size[4].descriptorCount = max_sets;

	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.pNext         = nullptr;
	pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.poolSizeCount = 5;
	pool_info.pPoolSizes    = pool_size;
	pool_info.maxSets       = max_sets;

	Pool pool {};

	vkCreateDescriptorPool(gctx->device, &pool_info, nullptr, &pool.pool);

	if (pool.pool == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: pool.pool == nullptr condition ignored (continuing)\n"); }

	pool.free           = true;
	pool.next_free_pool = m_first_free_pool;
	m_first_free_pool   = static_cast<int>(m_pools.Size());

	m_pools.Add(pool);
}

VulkanDescriptorSet* DescriptorCache::Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
                                               int samplers_num, int gds_buffers_num, bool vsharp_uniform_buffer)
{
	KYTY_PROFILER_BLOCK("DescriptorCache::Allocate");

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	const VkDescriptorSetLayout layout = GetOrCreateLayout(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num,
	                                                       samplers_num, gds_buffers_num, vsharp_uniform_buffer);

	auto* ret = new VulkanDescriptorSet;

	ret->set = nullptr;

	for (int try_num = 0; try_num < 2; try_num++)
	{
		int pool_id = m_first_free_pool;
		while (pool_id != -1)
		{
			auto& pool = m_pools[pool_id];

			VkDescriptorSetAllocateInfo alloc_info {};
			alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.pNext              = nullptr;
			alloc_info.descriptorPool     = pool.pool;
			alloc_info.descriptorSetCount = 1;
			alloc_info.pSetLayouts        = &layout;

			ret->pool_id = pool_id;
			ret->layout  = layout;

			EXIT_IF(!pool.free);

			{
				KYTY_PROFILER_BLOCK("vkAllocateDescriptorSets");

				auto result = vkAllocateDescriptorSets(gctx->device, &alloc_info, &ret->set);

				if (result == VK_SUCCESS)
				{
					return ret;
				}
			}

			pool.free         = false;
			m_first_free_pool = pool.next_free_pool;
			pool_id           = pool.next_free_pool;
		}

		CreatePool();
	}

	delete ret;
	return nullptr;
}

void DescriptorCache::Free(VulkanDescriptorSet* set)
{
	EXIT_IF(set == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);
	EXIT_IF(!m_pools.IndexValid(set->pool_id));

	auto& pool = m_pools[set->pool_id];

	vkFreeDescriptorSets(gctx->device, pool.pool, 1, &set->set);

	if (!pool.free)
	{
		pool.free           = true;
		pool.next_free_pool = m_first_free_pool;
		m_first_free_pool   = set->pool_id;
	}

	delete set;
}

uint32_t DescriptorCache::CalcHash(const Set& s)
{
	uint32_t hash = 0;
	hash += Core::hash8(static_cast<uint8_t>(s.stage));
	hash ^= Core::hash8(static_cast<uint8_t>(s.storage_buffers_num));
	hash += Core::hash8(static_cast<uint8_t>(s.textures2d_sampled_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures2d_sampled_depth_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures3d_sampled_num));
	hash += Core::hash8(static_cast<uint8_t>(s.textures2d_array_sampled_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures2d_sampled_uint_num));
	hash += Core::hash8(static_cast<uint8_t>(s.textures2d_array_sampled_uint_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures3d_sampled_uint_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures2d_storage_num));
	hash += Core::hash8(static_cast<uint8_t>(s.samplers_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.gds_buffers_num));
	for (int i = 0; i < s.storage_buffers_num; i++)
	{
		hash += Core::hash64(s.storage_buffers_id[i]);
	}
	for (int i = 0; i < s.textures2d_sampled_num; i++)
	{
		hash ^= Core::hash64(s.textures2d_sampled_id[i]);
		hash += Core::hash8(s.textures2d_sampled_view[i]);
	}
	for (int i = 0; i < s.textures2d_sampled_depth_num; i++)
	{
		hash += Core::hash64(s.textures2d_sampled_depth_id[i]);
		hash ^= Core::hash8(s.textures2d_sampled_depth_view[i]);
	}
	for (int i = 0; i < s.textures2d_array_sampled_num; i++)
	{
		hash ^= Core::hash64(s.textures2d_array_sampled_id[i]);
		hash += Core::hash8(s.textures2d_array_sampled_view[i]);
	}
	for (int i = 0; i < s.textures2d_storage_num; i++)
	{
		hash += Core::hash64(s.textures2d_storage_id[i]);
		hash ^= Core::hash8(s.textures2d_storage_view[i]);
	}
	for (int i = 0; i < s.samplers_num; i++)
	{
		hash ^= Core::hash64(s.samplers_id[i]);
	}
	for (int i = 0; i < s.gds_buffers_num; i++)
	{
		hash += Core::hash64(s.gds_buffers_id[i]);
	}
	for (int i = 0; i < s.textures3d_sampled_num; i++)
	{
		hash += Core::hash64(s.textures3d_sampled_id[i]);
		hash ^= Core::hash8(s.textures3d_sampled_view[i]);
	}
	for (int i = 0; i < s.textures2d_sampled_uint_num; i++)
	{
		hash += Core::hash64(s.textures2d_sampled_uint_id[i]);
		hash ^= Core::hash8(s.textures2d_sampled_uint_view[i]);
	}
	for (int i = 0; i < s.textures2d_array_sampled_uint_num; i++)
	{
		hash ^= Core::hash64(s.textures2d_array_sampled_uint_id[i]);
		hash += Core::hash8(s.textures2d_array_sampled_uint_view[i]);
	}
	for (int i = 0; i < s.textures3d_sampled_uint_num; i++)
	{
		hash += Core::hash64(s.textures3d_sampled_uint_id[i]);
		hash ^= Core::hash8(s.textures3d_sampled_uint_view[i]);
	}
	hash ^= Core::hash8(static_cast<uint8_t>(s.vsharp_uniform_buffer));
	if (s.vsharp_uniform_buffer)
	{
		hash += Core::hash64(s.vsharp_uniform_buffer_id);
	}
	return hash;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VulkanDescriptorSet* DescriptorCache::FindSet(const Set& s)
{
	const auto* list = m_sets_map.Find(s.hash);
	if (list != nullptr)
	{
		for (int index: *list)
		{
			auto& set = m_sets[index];

			if (set.set != nullptr && set.stage == s.stage && set.storage_buffers_num == s.storage_buffers_num &&
			    set.textures2d_sampled_num == s.textures2d_sampled_num &&
			    set.textures2d_sampled_depth_num == s.textures2d_sampled_depth_num &&
			    set.textures2d_array_sampled_num == s.textures2d_array_sampled_num &&
			    set.textures2d_storage_num == s.textures2d_storage_num && set.textures3d_sampled_num == s.textures3d_sampled_num &&
			    set.textures2d_sampled_uint_num == s.textures2d_sampled_uint_num &&
			    set.textures2d_array_sampled_uint_num == s.textures2d_array_sampled_uint_num &&
			    set.textures3d_sampled_uint_num == s.textures3d_sampled_uint_num &&
			    set.samplers_num == s.samplers_num &&
			    set.gds_buffers_num == s.gds_buffers_num)
			{
				bool match = true;
				for (int i = 0; i < s.storage_buffers_num; i++)
				{
					if (match && s.storage_buffers_id[i] != set.storage_buffers_id[i])
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_sampled_num; i++)
					{
						if (s.textures2d_sampled_id[i] != set.textures2d_sampled_id[i] ||
						    s.textures2d_sampled_view[i] != set.textures2d_sampled_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_sampled_depth_num; i++)
					{
						if (s.textures2d_sampled_depth_id[i] != set.textures2d_sampled_depth_id[i] ||
						    s.textures2d_sampled_depth_view[i] != set.textures2d_sampled_depth_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_array_sampled_num; i++)
					{
						if (s.textures2d_array_sampled_id[i] != set.textures2d_array_sampled_id[i] ||
						    s.textures2d_array_sampled_view[i] != set.textures2d_array_sampled_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures3d_sampled_num; i++)
					{
						if (s.textures3d_sampled_id[i] != set.textures3d_sampled_id[i] ||
						    s.textures3d_sampled_view[i] != set.textures3d_sampled_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_sampled_uint_num; i++)
					{
						if (s.textures2d_sampled_uint_id[i] != set.textures2d_sampled_uint_id[i] ||
						    s.textures2d_sampled_uint_view[i] != set.textures2d_sampled_uint_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_array_sampled_uint_num; i++)
					{
						if (s.textures2d_array_sampled_uint_id[i] != set.textures2d_array_sampled_uint_id[i] ||
						    s.textures2d_array_sampled_uint_view[i] != set.textures2d_array_sampled_uint_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures3d_sampled_uint_num; i++)
					{
						if (s.textures3d_sampled_uint_id[i] != set.textures3d_sampled_uint_id[i] ||
						    s.textures3d_sampled_uint_view[i] != set.textures3d_sampled_uint_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_storage_num; i++)
					{
						if (s.textures2d_storage_id[i] != set.textures2d_storage_id[i] ||
						    s.textures2d_storage_view[i] != set.textures2d_storage_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.samplers_num; i++)
					{
						if (s.samplers_id[i] != set.samplers_id[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.gds_buffers_num; i++)
					{
						if (s.gds_buffers_id[i] != set.gds_buffers_id[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match && (s.vsharp_uniform_buffer != set.vsharp_uniform_buffer ||
				              (s.vsharp_uniform_buffer && s.vsharp_uniform_buffer_id != set.vsharp_uniform_buffer_id)))
				{
					match = false;
				}
				if (match)
				{
					return set.set;
				}
			}
		}
	}
	return nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VulkanDescriptorSet* DescriptorCache::GetDescriptor(Stage stage, VulkanBuffer** storage_buffers, VulkanImage** textures2d_sampled,
                                                    const int* textures2d_sampled_view, VulkanImage** textures2d_sampled_depth,
                                                    const int* textures2d_sampled_depth_view, VulkanImage** textures2d_array_sampled,
                                                    const int* textures2d_array_sampled_view, VulkanImage** textures3d_sampled,
	                                                const int* textures3d_sampled_view, VulkanImage** textures2d_sampled_uint,
	                                                const int* textures2d_sampled_uint_view,
	                                                VulkanImage** textures2d_array_sampled_uint,
	                                                const int* textures2d_array_sampled_uint_view,
	                                                VulkanImage** textures3d_sampled_uint, const int* textures3d_sampled_uint_view,
	                                                VulkanImage** textures2d_storage,
                                                    const int* textures2d_storage_view, uint64_t* samplers, VulkanBuffer** gds_buffers,
                                                    VulkanBuffer* vsharp_buffer, const ShaderBindResources& bind)
{
	KYTY_PROFILER_BLOCK("DescriptorCache::GetDescriptor::search");

	int        storage_buffers_num    = bind.storage_buffers.buffers_num;
	const int  sampled_total = bind.textures2D.textures2d_sampled_num + bind.textures2D.textures2d_sampled_depth_num +
	                          bind.textures2D.textures2d_array_sampled_num + bind.textures2D.textures3d_sampled_num;
	const int  sampled_uint_total = bind.textures2D.textures2d_sampled_uint_num + bind.textures2D.textures2d_array_sampled_uint_num +
	                               bind.textures2D.textures3d_sampled_uint_num;
	const bool split_numeric_types = sampled_uint_total > 0 && sampled_uint_total < sampled_total;
	int        textures2d_sampled_num = !split_numeric_types || bind.textures2D.textures2d_sampled_uint_num < bind.textures2D.textures2d_sampled_num
	                                      ? bind.textures2D.textures2d_sampled_num
	                                      : 0;
	int textures2d_array_sampled_num =
	    !split_numeric_types || bind.textures2D.textures2d_array_sampled_uint_num < bind.textures2D.textures2d_array_sampled_num
	        ? bind.textures2D.textures2d_array_sampled_num
	        : 0;
	int textures3d_sampled_num = !split_numeric_types || bind.textures2D.textures3d_sampled_uint_num < bind.textures2D.textures3d_sampled_num
	                                 ? bind.textures2D.textures3d_sampled_num
	                                 : 0;
	int textures2d_sampled_uint_num = split_numeric_types && bind.textures2D.textures2d_sampled_uint_num > 0
	                                          ? bind.textures2D.textures2d_sampled_num
	                                          : 0;
	int textures2d_array_sampled_uint_num = split_numeric_types && bind.textures2D.textures2d_array_sampled_uint_num > 0
	                                                ? bind.textures2D.textures2d_array_sampled_num
	                                                : 0;
	int textures3d_sampled_uint_num = split_numeric_types && bind.textures2D.textures3d_sampled_uint_num > 0
	                                         ? bind.textures2D.textures3d_sampled_num
	                                         : 0;
	int        sampled_descriptor_num = sampled_total;
	int        textures2d_storage_num = bind.textures2D.textures2d_storage_num;
	int        samplers_num           = bind.samplers.samplers_num;
	int        gds_buffers_num        = bind.gds_pointers.pointers_num;
	const bool vsharp_uniform_buffer  = bind.vsharp_uniform_buffer;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(sampled_descriptor_num < 0 || sampled_descriptor_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_array_sampled_num < 0 || textures2d_array_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures3d_sampled_num < 0 || textures3d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_sampled_uint_num < 0 || textures2d_sampled_uint_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_array_sampled_uint_num < 0 || textures2d_array_sampled_uint_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures3d_sampled_uint_num < 0 || textures3d_sampled_uint_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(storage_buffers == nullptr);
	EXIT_IF(vsharp_uniform_buffer && vsharp_buffer == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	Set nset;
	nset.set                    = nullptr;
	nset.storage_buffers_num    = storage_buffers_num;
	nset.textures2d_sampled_num = textures2d_sampled_num;
	nset.textures2d_sampled_depth_num = bind.textures2D.textures2d_sampled_depth_num;
	nset.textures2d_array_sampled_num = textures2d_array_sampled_num;
	nset.textures3d_sampled_num = textures3d_sampled_num;
	nset.textures2d_sampled_uint_num = textures2d_sampled_uint_num;
	nset.textures2d_array_sampled_uint_num = textures2d_array_sampled_uint_num;
	nset.textures3d_sampled_uint_num = textures3d_sampled_uint_num;
	nset.textures2d_storage_num = textures2d_storage_num;
	nset.samplers_num           = samplers_num;
	nset.gds_buffers_num        = gds_buffers_num;
	nset.vsharp_uniform_buffer  = vsharp_uniform_buffer;
	nset.stage                  = stage;
	for (int i = 0; i < storage_buffers_num; i++)
	{
		nset.storage_buffers_id[i] = storage_buffers[i] != nullptr ? storage_buffers[i]->memory.unique_id : 0u;
	}
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		nset.textures2d_sampled_id[i]   = textures2d_sampled[i]->memory.unique_id;
		nset.textures2d_sampled_view[i] = static_cast<uint8_t>(textures2d_sampled_view[i]);
	}
	for (int i = 0; i < nset.textures2d_sampled_depth_num; i++)
	{
		nset.textures2d_sampled_depth_id[i]   = textures2d_sampled_depth[i]->memory.unique_id;
		nset.textures2d_sampled_depth_view[i] = static_cast<uint8_t>(textures2d_sampled_depth_view[i]);
	}
	for (int i = 0; i < textures2d_array_sampled_num; i++)
	{
		nset.textures2d_array_sampled_id[i]   = textures2d_array_sampled[i]->memory.unique_id;
		nset.textures2d_array_sampled_view[i] = static_cast<uint8_t>(textures2d_array_sampled_view[i]);
	}
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		nset.textures2d_storage_id[i]   = textures2d_storage[i]->memory.unique_id;
		nset.textures2d_storage_view[i] = static_cast<uint8_t>(textures2d_storage_view[i]);
	}
	for (int i = 0; i < samplers_num; i++)
	{
		nset.samplers_id[i] = samplers[i];
	}
	for (int i = 0; i < gds_buffers_num; i++)
	{
		nset.gds_buffers_id[i] = gds_buffers[i]->memory.unique_id;
	}
	for (int i = 0; i < textures3d_sampled_num; i++)
	{
		nset.textures3d_sampled_id[i]   = textures3d_sampled[i] != nullptr ? textures3d_sampled[i]->memory.unique_id : 0u;
		nset.textures3d_sampled_view[i] = static_cast<uint8_t>(textures3d_sampled_view[i]);
	}
	for (int i = 0; i < textures2d_sampled_uint_num; i++)
	{
		nset.textures2d_sampled_uint_id[i]   = textures2d_sampled_uint[i]->memory.unique_id;
		nset.textures2d_sampled_uint_view[i] = static_cast<uint8_t>(textures2d_sampled_uint_view[i]);
	}
	for (int i = 0; i < textures2d_array_sampled_uint_num; i++)
	{
		nset.textures2d_array_sampled_uint_id[i]   = textures2d_array_sampled_uint[i]->memory.unique_id;
		nset.textures2d_array_sampled_uint_view[i] = static_cast<uint8_t>(textures2d_array_sampled_uint_view[i]);
	}
	for (int i = 0; i < textures3d_sampled_uint_num; i++)
	{
		nset.textures3d_sampled_uint_id[i]   = textures3d_sampled_uint[i]->memory.unique_id;
		nset.textures3d_sampled_uint_view[i] = static_cast<uint8_t>(textures3d_sampled_uint_view[i]);
	}
	if (vsharp_uniform_buffer)
	{
		nset.vsharp_uniform_buffer_id = vsharp_buffer->memory.unique_id;
	}
	nset.hash = CalcHash(nset);

	if (auto* f = FindSet(nset); f != nullptr)
	{
		return f;
	}

	KYTY_PROFILER_END_BLOCK;

	KYTY_PROFILER_BLOCK("DescriptorCache::GetDescriptor::create");

	auto* new_set = Allocate(stage, storage_buffers_num, sampled_descriptor_num, textures2d_storage_num, samplers_num, gds_buffers_num,
	                         vsharp_uniform_buffer);
	if (new_set == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: new_set == nullptr condition ignored (continuing)\n"); }

	VkDescriptorBufferInfo buffer_info[BUFFERS_MAX] {};
	for (int i = 0; i < storage_buffers_num; i++)
	{
		// PrepareStorageBuffers must always materialize a carrier (including empty OOB
		// and null V#). A null entry here is a host bug, not a guest layout.
		EXIT_IF(storage_buffers[i] == nullptr || storage_buffers[i]->buffer == nullptr);
		buffer_info[i].buffer = storage_buffers[i]->buffer;
		buffer_info[i].offset = 0;
		buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	VkDescriptorImageInfo texture2d_sampled_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		texture2d_sampled_info[i].sampler   = nullptr;
		texture2d_sampled_info[i].imageView = textures2d_sampled[i]->image_view[textures2d_sampled_view[i]];
		texture2d_sampled_info[i].imageLayout = IsDepthSampledView(textures2d_sampled_view[i])
		                                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_sampled_depth_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < nset.textures2d_sampled_depth_num; i++)
	{
		texture2d_sampled_depth_info[i].sampler     = nullptr;
		texture2d_sampled_depth_info[i].imageView   = textures2d_sampled_depth[i]->image_view[textures2d_sampled_depth_view[i]];
		texture2d_sampled_depth_info[i].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_array_sampled_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_array_sampled_num; i++)
	{
		texture2d_array_sampled_info[i].sampler   = nullptr;
		texture2d_array_sampled_info[i].imageView = textures2d_array_sampled[i]->image_view[textures2d_array_sampled_view[i]];
		texture2d_array_sampled_info[i].imageLayout = IsDepthSampledView(textures2d_array_sampled_view[i])
		                                                   ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		                                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_storage_info[TEXTURES_STORAGE_MAX] {};
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		texture2d_storage_info[i].sampler     = nullptr;
		texture2d_storage_info[i].imageView   = textures2d_storage[i]->image_view[textures2d_storage_view[i]];
		texture2d_storage_info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorImageInfo sampler_info[SAMPLERS_MAX] {};
	for (int i = 0; i < samplers_num; i++)
	{
		sampler_info[i].sampler     = g_render_ctx->GetSamplerCache()->GetSampler(samplers[i]);
		sampler_info[i].imageView   = nullptr;
		sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorBufferInfo gds_buffer_info[GDS_BUFFER_MAX] {};
	for (int i = 0; i < gds_buffers_num; i++)
	{
		gds_buffer_info[i].buffer = gds_buffers[i]->buffer;
		gds_buffer_info[i].offset = 0;
		gds_buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	VkDescriptorImageInfo texture3d_sampled_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures3d_sampled_num; i++)
	{
		texture3d_sampled_info[i].sampler     = nullptr;
		texture3d_sampled_info[i].imageView   = textures3d_sampled[i]->image_view[textures3d_sampled_view[i]];
		texture3d_sampled_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_sampled_uint_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_sampled_uint_num; i++)
	{
		texture2d_sampled_uint_info[i].sampler   = nullptr;
		texture2d_sampled_uint_info[i].imageView = textures2d_sampled_uint[i]->image_view[textures2d_sampled_uint_view[i]];
		texture2d_sampled_uint_info[i].imageLayout = IsDepthSampledView(textures2d_sampled_uint_view[i])
		                                                  ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_array_sampled_uint_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_array_sampled_uint_num; i++)
	{
		texture2d_array_sampled_uint_info[i].sampler   = nullptr;
		texture2d_array_sampled_uint_info[i].imageView =
		    textures2d_array_sampled_uint[i]->image_view[textures2d_array_sampled_uint_view[i]];
		texture2d_array_sampled_uint_info[i].imageLayout = IsDepthSampledView(textures2d_array_sampled_uint_view[i])
		                                                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		                                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture3d_sampled_uint_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures3d_sampled_uint_num; i++)
	{
		texture3d_sampled_uint_info[i].sampler     = nullptr;
		texture3d_sampled_uint_info[i].imageView   = textures3d_sampled_uint[i]->image_view[textures3d_sampled_uint_view[i]];
		texture3d_sampled_uint_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorBufferInfo vsharp_buffer_info {};
	if (vsharp_uniform_buffer)
	{
		vsharp_buffer_info.buffer = vsharp_buffer->buffer;
		vsharp_buffer_info.offset = 0;
		vsharp_buffer_info.range  = bind.push_constant_size;
	}

	uint32_t binding_num = 0;

	constexpr uint32_t B_MAX = 13;

	VkWriteDescriptorSet descriptor_write[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.storage_buffers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = storage_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_sampled_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_sampled_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (nset.textures2d_sampled_depth_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_depth_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = nset.textures2d_sampled_depth_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_sampled_depth_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_array_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_array_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_array_sampled_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_array_sampled_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_storage_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_storage_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_storage_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.samplers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLER;
		descriptor_write[binding_num].descriptorCount  = samplers_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = sampler_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.gds_pointers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = gds_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = gds_buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures3d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_3d_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures3d_sampled_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture3d_sampled_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_uint_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_uint_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_sampled_uint_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_sampled_uint_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_array_sampled_uint_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_array_uint_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_array_sampled_uint_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_array_sampled_uint_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures3d_sampled_uint_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_3d_uint_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures3d_sampled_uint_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture3d_sampled_uint_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (vsharp_uniform_buffer)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.vsharp_binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptor_write[binding_num].descriptorCount  = 1;
		descriptor_write[binding_num].pBufferInfo      = &vsharp_buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	vkUpdateDescriptorSets(gctx->device, binding_num, descriptor_write, 0, nullptr);

	nset.set = new_set;

	int index = 0;

	if (m_first_free_set != -1)
	{
		index            = m_first_free_set;
		auto& set        = m_sets[m_first_free_set];
		m_first_free_set = set.next_free_set;
		set              = nset;
	} else
	{
		index = static_cast<int>(m_sets.Size());
		m_sets.Add(nset);
	}

	auto& ids = m_sets_map[nset.hash];
	if (!ids.Contains(index))
	{
		ids.Add(index);
	}

	return new_set;
}

void DescriptorCache::FreeDescriptor(VulkanBuffer* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer == nullptr);

	Core::LockGuard lock(m_mutex);

	int index = 0;
	for (auto& set: m_sets)
	{
		if (set.set != nullptr)
		{
			for (int i = 0; i < set.storage_buffers_num; i++)
			{
				if (set.storage_buffers_id[i] == buffer->memory.unique_id)
				{
					Free(set.set);
					set.set           = nullptr;
					set.next_free_set = m_first_free_set;
					m_first_free_set  = index;
					auto& ids         = m_sets_map[set.hash];
					ids.Remove(index);
					if (ids.IsEmpty())
					{
						m_sets_map.Remove(set.hash);
					}
					break;
				}
			}
		}
		index++;
	}
}

void DescriptorCache::FreeDescriptor(VulkanImage* image)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	int index = 0;
	for (auto& set: m_sets)
	{
		if (set.set != nullptr)
		{
			bool references_image = false;
			for (int i = 0; i < set.textures2d_sampled_num; i++)
			{
				if (set.textures2d_sampled_id[i] == image->memory.unique_id)
				{
					references_image = true;
					break;
				}
			}
			for (int i = 0; !references_image && i < set.textures2d_array_sampled_num; i++)
			{
				references_image = set.textures2d_array_sampled_id[i] == image->memory.unique_id;
			}
			for (int i = 0; !references_image && i < set.textures3d_sampled_num; i++)
			{
				references_image = set.textures3d_sampled_id[i] == image->memory.unique_id;
			}
			for (int i = 0; !references_image && i < set.textures2d_sampled_uint_num; i++)
			{
				references_image = set.textures2d_sampled_uint_id[i] == image->memory.unique_id;
			}
			for (int i = 0; !references_image && i < set.textures2d_array_sampled_uint_num; i++)
			{
				references_image = set.textures2d_array_sampled_uint_id[i] == image->memory.unique_id;
			}
			for (int i = 0; !references_image && i < set.textures3d_sampled_uint_num; i++)
			{
				references_image = set.textures3d_sampled_uint_id[i] == image->memory.unique_id;
			}
			for (int i = 0; !references_image && i < set.textures2d_storage_num; i++)
			{
				references_image = set.textures2d_storage_id[i] == image->memory.unique_id;
			}
			if (references_image)
			{
				Free(set.set);
				set.set           = nullptr;
				set.next_free_set = m_first_free_set;
				m_first_free_set  = index;
				auto& ids         = m_sets_map[set.hash];
				ids.Remove(index);
				if (ids.IsEmpty())
				{
					m_sets_map.Remove(set.hash);
				}
			}
		}
		index++;
	}
}

VkDescriptorSetLayout DescriptorCache::GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind)
{
	int        storage_buffers_num    = bind.storage_buffers.buffers_num;
	int        textures2d_sampled_num = bind.textures2D.textures2d_sampled_num + bind.textures2D.textures2d_sampled_depth_num +
	                                   bind.textures2D.textures2d_array_sampled_num + bind.textures2D.textures3d_sampled_num;
	int        textures2d_storage_num = bind.textures2D.textures2d_storage_num;
	int        samplers_num           = bind.samplers.samplers_num;
	int        gds_buffers_num        = bind.gds_pointers.pointers_num;
	const bool vsharp_uniform_buffer  = bind.vsharp_uniform_buffer;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	// Image and sampler descriptors are valid for every graphics stage. Vertex
	// layouts are cached separately below and use VK_SHADER_STAGE_VERTEX_BIT, so
	// do not reject a vertex shader merely because it reads a texture or sampler.
	// This path is also required by titles which perform texture fetches while
	// building vertex positions.

	Core::LockGuard lock(m_mutex);
	return GetOrCreateLayout(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num, gds_buffers_num,
	                         vsharp_uniform_buffer);
}

void DeleteFramebuffer(VideoOutVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(DepthStencilVulkanImage* image)
{
	g_render_ctx->ReleaseDepthStencilCopySource(image->memory.unique_id);
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByDepth(image);
}

void DeleteDescriptor(VulkanBuffer* buffer)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(buffer);
}

void DeleteDescriptor(TextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(StorageTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

static bool get_stencil_op(VkStencilOp* f, uint32_t* ref, uint8_t op, uint8_t testval, uint8_t opval)
{
	VkStencilOp vk      = VK_STENCIL_OP_KEEP;
	uint32_t    r       = opval;
	bool        use_ref = true;

	switch (op)
	{
		case 0x00:
			vk      = VK_STENCIL_OP_KEEP;
			use_ref = false;
			break; /* Keep           */
		case 0x01:
			vk      = VK_STENCIL_OP_ZERO;
			use_ref = false;
			break; /* Zero           */
		case 0x02:
			vk = VK_STENCIL_OP_REPLACE;
			r  = 0xFF;
			break; /* Ones           */
		case 0x03:
			vk = VK_STENCIL_OP_REPLACE;
			r  = testval;
			break;                                                /* ReplaceTest    */
		case 0x04: vk = VK_STENCIL_OP_REPLACE; break;             /* ReplaceOp      */
		case 0x05: vk = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break; /* AddClamp       */
		case 0x06: vk = VK_STENCIL_OP_DECREMENT_AND_CLAMP; break; /* SubClamp       */
		case 0x07: vk = VK_STENCIL_OP_INVERT; break;              /* Invert         */
		case 0x08: vk = VK_STENCIL_OP_INCREMENT_AND_WRAP; break;  /* AddWrap        */
		case 0x09: vk = VK_STENCIL_OP_DECREMENT_AND_WRAP; break;  /* SubWrap        */
		case 0x0a:                                                /* And            */
		case 0x0b:                                                /* Or             */
		case 0x0c:                                                /* Xor            */
		case 0x0d:                                                /* Nand           */
		case 0x0e:                                                /* Nor            */
		case 0x0f:                                                /* Xnor           */
		default: KYTY_LOG_DEBUG("WARNING: invalid PM4 op (continuing)\n"); break;
	}
	*f   = vk;
	*ref = r;
	return use_ref;
}

void get_stencil_state(PipelineStencilStaticState* s, PipelineStencilDynamicState* d, uint8_t func, uint8_t fail, uint8_t zpass,
                              uint8_t zfail, uint8_t testval, uint8_t mask, uint8_t writemask, uint8_t opval)
{
	EXIT_IF(s == nullptr || d == nullptr);

	uint32_t ref[3]     = {};
	bool     use_ref[3] = {};

	use_ref[0]     = get_stencil_op(&s->failOp, ref + 0, fail, testval, opval);
	use_ref[1]     = get_stencil_op(&s->passOp, ref + 1, zpass, testval, opval);
	use_ref[2]     = get_stencil_op(&s->depthFailOp, ref + 2, zfail, testval, opval);
	s->compareOp   = static_cast<VkCompareOp>(func);
	d->compareMask = mask;
	d->writeMask   = writemask;

	if (use_ref[0])
	{
		if ((ref[0] != ref[1] && use_ref[1]) || (ref[0] != ref[2] && use_ref[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (ref[0] != ref[1] && use_ref[1]) || (ref[0] != ref[2] && use_ref[2]) condition ignored (continuing)\n"); }
		d->reference = ref[0];
	} else if (use_ref[1])
	{
		if (ref[1] != ref[2] && use_ref[2]) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ref[1] != ref[2] && use_ref[2] condition ignored (continuing)\n"); }
		d->reference = ref[1];
	} else if (use_ref[2])
	{
		d->reference = ref[2];
	} else
	{
		d->reference = testval;
	}
}

Vector<RenderTextureVulkanImage*> FindRenderTexture(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer == nullptr);

	Vector<RenderTextureVulkanImage*> ret;

	auto objects = GpuMemoryFindObjectsForSubmission(buffer, vaddr, size, GpuMemoryObjectType::RenderTexture, exact, false);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::RenderTexture)
		{
			ret.Add(static_cast<RenderTextureVulkanImage*>(obj.obj));
		}
	}

	return ret;
}

Vector<StorageTextureVulkanImage*> FindStorageTexture(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer == nullptr);

	Vector<StorageTextureVulkanImage*> ret;

	auto objects = GpuMemoryFindObjectsForSubmission(buffer, vaddr, size, GpuMemoryObjectType::StorageTexture, exact, false);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::StorageTexture)
		{
			ret.Add(static_cast<StorageTextureVulkanImage*>(obj.obj));
		}
	}

	return ret;
}

Vector<DepthStencilVulkanImage*> FindDepthStencil(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool exact)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer == nullptr);

	Vector<DepthStencilVulkanImage*> ret;

	auto objects = GpuMemoryFindObjectsForSubmission(buffer, vaddr, size, GpuMemoryObjectType::DepthStencilBuffer, exact, true);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::DepthStencilBuffer)
		{
			ret.Add(static_cast<DepthStencilVulkanImage*>(obj.obj));
		}
	}

	return ret;
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
