#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// HW state print/check validators

void uc_print(const char* func, const HW::UserConfig& uc)
{
	printf("%s\n", func);

	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	printf("\t GetPrimType()         = 0x%08" PRIx32 "\n", uc.GetPrimType());
	printf("\t primitive_group_size  = 0x%04" PRIx16 "\n", ge_cntl.primitive_group_size);
	printf("\t vertex_group_size     = 0x%04" PRIx16 "\n", ge_cntl.vertex_group_size);
	printf("\t en_user_vgpr1         = %s\n", user_en.vgpr1 ? "true" : "false");
	printf("\t en_user_vgpr2         = %s\n", user_en.vgpr2 ? "true" : "false");
	printf("\t en_user_vgpr3         = %s\n", user_en.vgpr3 ? "true" : "false");
}

void uc_check(const HW::UserConfig& uc)
{
	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	// GE_CNTL group sizes are host scheduling hints. Gen5 titles emit values
	// other than 0/0x40; accept the documented max of 0x40 (same bound as
	// Kyty) rather than an exact-value whitelist.
	EXIT_NOT_IMPLEMENTED(ge_cntl.primitive_group_size > 0x0040);
	EXIT_NOT_IMPLEMENTED(ge_cntl.vertex_group_size > 0x0040);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr1 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr2 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr3 != false);
}

void sh_print(const char* func, const HW::Shader& /*uc*/)
{
	printf("%s\n", func);
}

void sh_check(const HW::Shader& /*uc*/) {}

static Core::StringList rt_print(const char* func, const HW::RenderTarget& rt)
{
	Core::StringList dst;

	dst.Add(String::FromPrintf("%s\n", func));

	dst.Add(String::FromPrintf("\t base.addr                       = 0x%016" PRIx64 "\n", rt.base.addr));
	dst.Add(String::FromPrintf("\t pitch.pitch_div8_minus1         = 0x%08" PRIx32 "\n", rt.pitch.pitch_div8_minus1));
	dst.Add(String::FromPrintf("\t pitch.fmask_pitch_div8_minus1   = 0x%08" PRIx32 "\n", rt.pitch.fmask_pitch_div8_minus1));
	dst.Add(String::FromPrintf("\t slice.slice_div64_minus1        = 0x%08" PRIx32 "\n", rt.slice.slice_div64_minus1));
	dst.Add(String::FromPrintf("\t view.base_array_slice_index     = 0x%08" PRIx32 "\n", rt.view.base_array_slice_index));
	dst.Add(String::FromPrintf("\t view.last_array_slice_index     = 0x%08" PRIx32 "\n", rt.view.last_array_slice_index));
	dst.Add(String::FromPrintf("\t view.current_mip_level          = 0x%08" PRIx32 "\n", rt.view.current_mip_level));
	dst.Add(String::FromPrintf("\t info.fmask_compression_enable   = %s\n", rt.info.fmask_compression_enable ? "true" : "false"));

	// dst.Add(String::FromPrintf("\t info.fmask_compression_mode     = 0x%08" PRIx32 "\n", rt.info.fmask_compression_mode));
	dst.Add(String::FromPrintf("\t info.fmask_data_compression_disable = %s\n", rt.info.fmask_data_compression_disable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.fmask_one_frag_mode        = %s\n", rt.info.fmask_one_frag_mode ? "true" : "false"));

	dst.Add(String::FromPrintf("\t info.cmask_fast_clear_enable    = %s\n", rt.info.cmask_fast_clear_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.dcc_compression_enable     = %s\n", rt.info.dcc_compression_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.neo_mode                   = %s\n", rt.info.neo_mode ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.cmask_tile_mode            = 0x%08" PRIx32 "\n", rt.info.cmask_tile_mode));
	dst.Add(String::FromPrintf("\t info.cmask_tile_mode_neo        = 0x%08" PRIx32 "\n", rt.info.cmask_tile_mode_neo));
	dst.Add(String::FromPrintf("\t info.format                     = 0x%08" PRIx32 "\n", rt.info.format));
	dst.Add(String::FromPrintf("\t info.channel_type               = 0x%08" PRIx32 "\n", rt.info.channel_type));
	dst.Add(String::FromPrintf("\t info.channel_order              = 0x%08" PRIx32 "\n", rt.info.channel_order));
	dst.Add(String::FromPrintf("\t info.blend_bypa                 = %s\n", rt.info.blend_bypass ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.blend_clamp                = %s\n", rt.info.blend_clamp ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.round_mode                 = %s\n", rt.info.round_mode ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib.force_dest_alpha_to_one  = %s\n", rt.attrib.force_dest_alpha_to_one ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib.tile_mode                = 0x%08" PRIx32 "\n", rt.attrib.tile_mode));
	dst.Add(String::FromPrintf("\t attrib.fmask_tile_mode          = 0x%08" PRIx32 "\n", rt.attrib.fmask_tile_mode));
	dst.Add(String::FromPrintf("\t attrib.num_samples              = 0x%08" PRIx32 "\n", rt.attrib.num_samples));
	dst.Add(String::FromPrintf("\t attrib.num_fragments            = 0x%08" PRIx32 "\n", rt.attrib.num_fragments));
	dst.Add(String::FromPrintf("\t attrib2.width                   = 0x%08" PRIx32 "\n", rt.attrib2.width));
	dst.Add(String::FromPrintf("\t attrib2.height                  = 0x%08" PRIx32 "\n", rt.attrib2.height));
	dst.Add(String::FromPrintf("\t attrib2.num_mip_levels          = 0x%08" PRIx32 "\n", rt.attrib2.num_mip_levels));
	dst.Add(String::FromPrintf("\t attrib3.depth                   = 0x%08" PRIx32 "\n", rt.attrib3.depth));
	dst.Add(String::FromPrintf("\t attrib3.tile_mode               = 0x%08" PRIx32 "\n", rt.attrib3.tile_mode));
	dst.Add(String::FromPrintf("\t attrib3.dimension               = 0x%08" PRIx32 "\n", rt.attrib3.dimension));
	dst.Add(String::FromPrintf("\t attrib3.cmask_pipe_aligned      = %s\n", rt.attrib3.cmask_pipe_aligned ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib3.dcc_pipe_aligned        = %s\n", rt.attrib3.dcc_pipe_aligned ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.max_uncompressed_block_size = 0x%08" PRIx32 "\n", rt.dcc.max_uncompressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.max_compressed_block_size   = 0x%08" PRIx32 "\n", rt.dcc.max_compressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.min_compressed_block_size   = 0x%08" PRIx32 "\n", rt.dcc.min_compressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.color_transform             = 0x%08" PRIx32 "\n", rt.dcc.color_transform));
	dst.Add(String::FromPrintf("\t dcc.overwrite_combiner_disable  = %s\n", rt.dcc.overwrite_combiner_disable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.independent_64b_blocks      = %s\n", rt.dcc.independent_64b_blocks ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.independent_128b_blocks     = %s\n", rt.dcc.independent_128b_blocks ? "true" : "false"));
	dst.Add(String::FromPrintf("\t data_write_on_dcc_clear_to_reg  = %s\n", rt.dcc.data_write_on_dcc_clear_to_reg ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.dcc_clear_key_enable        = %s\n", rt.dcc.dcc_clear_key_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t cmask.addr                      = 0x%016" PRIx64 "\n", rt.cmask.addr));
	dst.Add(String::FromPrintf("\t cmask_slice.slice_minus1        = 0x%08" PRIx32 "\n", rt.cmask_slice.slice_minus1));
	dst.Add(String::FromPrintf("\t fmask.addr                      = 0x%016" PRIx64 "\n", rt.fmask.addr));
	dst.Add(String::FromPrintf("\t fmask_slice.slice_minus1        = 0x%08" PRIx32 "\n", rt.fmask_slice.slice_minus1));
	dst.Add(String::FromPrintf("\t clear_word0.word0               = 0x%08" PRIx32 "\n", rt.clear_word0.word0));
	dst.Add(String::FromPrintf("\t clear_word1.word1               = 0x%08" PRIx32 "\n", rt.clear_word1.word1));
	dst.Add(String::FromPrintf("\t dcc_addr.addr                   = 0x%016" PRIx64 "\n", rt.dcc_addr.addr));
	dst.Add(String::FromPrintf("\t size.width                      = 0x%08" PRIx32 "\n", rt.size.width));
	dst.Add(String::FromPrintf("\t size.height                     = 0x%08" PRIx32 "\n", rt.size.height));

	return dst;
}

static bool rt_uses_cmask_metadata(const HW::RenderTarget& rt)
{
	return rt.info.cmask_fast_clear_enable || rt.info.cmask_tile_mode != 0 || rt.info.cmask_tile_mode_neo != 0 ||
	       rt.cmask.addr != 0 || rt.cmask_slice.slice_minus1 != 0;
}

static bool rt_uses_dcc_metadata(const HW::RenderTarget& rt)
{
	return rt.info.dcc_compression_enable || rt.dcc_addr.addr != 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void rt_check(const HW::RenderTarget& rt)
{
	if (rt.base.addr != 0)
	{
		bool ps5 = Config::IsNextGen();
		// bool render_to_texture = (rt.attrib.tile_mode == 0x0d);
		//  EXIT_NOT_IMPLEMENTED(rt.base_addr == 0);
		if (ps5)
		{
			EXIT_NOT_IMPLEMENTED(rt.pitch.pitch_div8_minus1 != 0);
			EXIT_NOT_IMPLEMENTED(rt.pitch.fmask_pitch_div8_minus1 != 0);
			EXIT_NOT_IMPLEMENTED(rt.slice.slice_div64_minus1 != 0);
		}
		EXIT_NOT_IMPLEMENTED(rt.view.base_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.view.last_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.view.current_mip_level != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.info.fmask_compression_enable != false);

		// EXIT_NOT_IMPLEMENTED(rt.info.fmask_compression_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.info.fmask_data_compression_disable != false);
		EXIT_NOT_IMPLEMENTED(rt.info.fmask_one_frag_mode != false);

		EXIT_NOT_IMPLEMENTED(rt.info.cmask_fast_clear_enable != false);
		EXIT_NOT_IMPLEMENTED(rt.info.dcc_compression_enable != false);
		EXIT_NOT_IMPLEMENTED(!(rt.attrib.tile_mode == 0x0d) && rt.info.neo_mode != Config::IsNeo());
		EXIT_NOT_IMPLEMENTED(rt.info.cmask_tile_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.info.cmask_tile_mode_neo != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(rt.info.blend_clamp != false);
		// ROUND_MODE (CB_COLOR*_INFO bit 18): truncate vs round when writing
		// fixed-point blend results. Captured post-Play RTs set it true.
		// Layout-neutral; host blend uses Vulkan's fixed conversion. Accept both.
		// EXIT_NOT_IMPLEMENTED(rt.info.round_mode != false);
		//		 EXIT_NOT_IMPLEMENTED(rt.format != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.channel_type != 0x00000006);
		// EXIT_NOT_IMPLEMENTED(rt.channel_order != 0x00000001);
		// force_dest_alpha_to_one: output-alpha override. Ignored for bring-up.
		// EXIT_NOT_IMPLEMENTED(rt.attrib.force_dest_alpha_to_one != false);
		// EXIT_NOT_IMPLEMENTED(rt.tile_mode != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.fmask_tile_mode != 0x0000000a);
		(void)decode_guest_sample_count(rt.attrib.num_samples);
		EXIT_NOT_IMPLEMENTED(rt.attrib.num_samples != rt.attrib.num_fragments);
		if (ps5)
		{
			EXIT_NOT_IMPLEMENTED(rt.attrib2.width == 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib2.height == 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib2.num_mip_levels != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.depth != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.tile_mode != 0x0000001b);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.dimension != 0x00000001);
			// Pipe alignment only affects the separately addressed CMASK/DCC
			// metadata surfaces. An inactive surface cannot alter the color image
			// layout, while active metadata remains rejected below.
			EXIT_NOT_IMPLEMENTED(rt_uses_cmask_metadata(rt) && !rt.attrib3.cmask_pipe_aligned);
			EXIT_NOT_IMPLEMENTED(rt_uses_dcc_metadata(rt) && !rt.attrib3.dcc_pipe_aligned);
		} else
		{
			EXIT_NOT_IMPLEMENTED(rt.attrib2.width != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib2.height != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib2.num_mip_levels != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.depth != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.tile_mode != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.dimension != 0x00000000);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.cmask_pipe_aligned != false);
			EXIT_NOT_IMPLEMENTED(rt.attrib3.dcc_pipe_aligned != false);
		}
		// EXIT_NOT_IMPLEMENTED(rt.dcc_max_uncompressed_block_size != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.max_compressed_block_size != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc.min_compressed_block_size != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.color_transform != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc.overwrite_combiner_disable != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.force_independent_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.independent_128b_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.data_write_on_dcc_clear_to_reg != false);
		EXIT_NOT_IMPLEMENTED(rt.dcc.dcc_clear_key_enable != false);
		EXIT_NOT_IMPLEMENTED(rt.cmask.addr != 0x0000000000000000);
		EXIT_NOT_IMPLEMENTED(rt.cmask_slice.slice_minus1 != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.fmask.addr != 0x0000000000000000);
		EXIT_NOT_IMPLEMENTED(rt.fmask_slice.slice_minus1 != 0x00000000 && rt.fmask_slice.slice_minus1 != rt.slice.slice_div64_minus1);
		// CLEAR_WORD0/1 hold the raw clear pixel; format-aware decode is in
		// DecodeGuestColorClearWords (Mesa/RADV packing). Non-zero words are legal.
		EXIT_NOT_IMPLEMENTED(rt.dcc_addr.addr != 0x0000000000000000);
		if (ps5)
		{
			EXIT_NOT_IMPLEMENTED(rt.size.width != 0);
			EXIT_NOT_IMPLEMENTED(rt.size.height != 0);
		}
	}
}

static void z_print(const char* func, const HW::DepthRenderTarget& z)
{
	printf("%s\n", func);

	printf("\t z_info.format                         = 0x%08" PRIx32 "\n", z.z_info.format);
	printf("\t z_info.tile_mode_index                = 0x%08" PRIx32 "\n", z.z_info.tile_mode_index);
	printf("\t z_info.num_samples                    = 0x%08" PRIx32 "\n", z.z_info.num_samples);
	printf("\t z_info.tile_surface_enable            = %s\n", z.z_info.tile_surface_enable ? "true" : "false");
	printf("\t z_info.expclear_enabled               = %s\n", z.z_info.expclear_enabled ? "true" : "false");
	printf("\t z_info.zrange_precision               = 0x%08" PRIx32 "\n", z.z_info.zrange_precision);
	printf("\t z_info.embedded_sample_locations      = %s\n", z.z_info.embedded_sample_locations ? "true" : "false");
	printf("\t z_info.partially_resident             = %s\n", z.z_info.partially_resident ? "true" : "false");
	printf("\t z_info.num_mip_levels                 = 0x%02" PRIx8 "\n", z.z_info.num_mip_levels);
	printf("\t z_info.plane_compression              = 0x%02" PRIx8 "\n", z.z_info.plane_compression);
	printf("\t stencil_info.format                   = 0x%08" PRIx32 "\n", z.stencil_info.format);
	printf("\t stencil_info.tile_stencil_disable     = %s\n", z.stencil_info.tile_stencil_disable ? "true" : "false");
	printf("\t stencil_info.expclear_enabled         = %s\n", z.stencil_info.expclear_enabled ? "true" : "false");
	printf("\t stencil_info.tile_mode_index          = 0x%08" PRIx32 "\n", z.stencil_info.tile_mode_index);
	printf("\t stencil_info.tile_split               = 0x%08" PRIx32 "\n", z.stencil_info.tile_split);
	printf("\t stencil_info.texture_compatible_stencil = %s\n", z.stencil_info.texture_compatible_stencil ? "true" : "false");
	printf("\t stencil_info.partially_resident       = %s\n", z.stencil_info.partially_resident ? "true" : "false");
	printf("\t depth_info.addr5_swizzle_mask         = 0x%08" PRIx32 "\n", z.depth_info.addr5_swizzle_mask);
	printf("\t depth_info.array_mode                 = 0x%08" PRIx32 "\n", z.depth_info.array_mode);
	printf("\t depth_info.pipe_config                = 0x%08" PRIx32 "\n", z.depth_info.pipe_config);
	printf("\t depth_info.bank_width                 = 0x%08" PRIx32 "\n", z.depth_info.bank_width);
	printf("\t depth_info.bank_height                = 0x%08" PRIx32 "\n", z.depth_info.bank_height);
	printf("\t depth_info.macro_tile_aspect          = 0x%08" PRIx32 "\n", z.depth_info.macro_tile_aspect);
	printf("\t depth_info.num_banks                  = 0x%08" PRIx32 "\n", z.depth_info.num_banks);
	printf("\t depth_view.slice_start                = 0x%08" PRIx32 "\n", z.depth_view.slice_start);
	printf("\t depth_view.slice_max                  = 0x%08" PRIx32 "\n", z.depth_view.slice_max);
	printf("\t depth_view.current_mip_level          = 0x%02" PRIx8 "\n", z.depth_view.current_mip_level);
	printf("\t depth_view.depth_write_disable        = %s\n", z.depth_view.depth_write_disable ? "true" : "false");
	printf("\t depth_view.stencil_write_disable      = %s\n", z.depth_view.stencil_write_disable ? "true" : "false");
	printf("\t htile_surface.linear                  = 0x%08" PRIx32 "\n", z.htile_surface.linear);
	printf("\t htile_surface.full_cache              = 0x%08" PRIx32 "\n", z.htile_surface.full_cache);
	printf("\t htile_surface.htile_uses_preload_win  = 0x%08" PRIx32 "\n", z.htile_surface.htile_uses_preload_win);
	printf("\t htile_surface.preload                 = 0x%08" PRIx32 "\n", z.htile_surface.preload);
	printf("\t htile_surface.prefetch_width          = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_width);
	printf("\t htile_surface.prefetch_height         = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_height);
	printf("\t htile_surface.dst_outside_zero_to_one = 0x%08" PRIx32 "\n", z.htile_surface.dst_outside_zero_to_one);
	printf("\t z_read_base_addr                      = 0x%016" PRIx64 "\n", z.z_read_base_addr);
	printf("\t stencil_read_base_addr                = 0x%016" PRIx64 "\n", z.stencil_read_base_addr);
	printf("\t z_write_base_addr                     = 0x%016" PRIx64 "\n", z.z_write_base_addr);
	printf("\t stencil_write_base_addr               = 0x%016" PRIx64 "\n", z.stencil_write_base_addr);
	printf("\t pitch_div8_minus1                     = 0x%08" PRIx32 "\n", z.pitch_div8_minus1);
	printf("\t height_div8_minus1                    = 0x%08" PRIx32 "\n", z.height_div8_minus1);
	printf("\t slice_div64_minus1                    = 0x%08" PRIx32 "\n", z.slice_div64_minus1);
	printf("\t htile_data_base_addr                  = 0x%016" PRIx64 "\n", z.htile_data_base_addr);
	printf("\t width                                 = 0x%08" PRIx32 "\n", z.width);
	printf("\t height                                = 0x%08" PRIx32 "\n", z.height);
	printf("\t size.x_max                            = 0x%04" PRIx16 "\n", z.size.x_max);
	printf("\t size.y_max                            = 0x%04" PRIx16 "\n", z.size.y_max);
}

static void validate_depth_plane(const HW::DepthRenderTarget& z)
{
	if (z.z_info.format == 0)
	{
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled);
		EXIT_NOT_IMPLEMENTED(z.z_info.embedded_sample_locations);
		EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.plane_compression != 0);
		return;
	}
	EXIT_NOT_IMPLEMENTED(z.z_info.format != 0x00000003);
	(void)decode_guest_sample_count(z.z_info.num_samples);
	EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled);
	EXIT_NOT_IMPLEMENTED(z.z_info.zrange_precision != 1);
	EXIT_NOT_IMPLEMENTED(z.z_info.embedded_sample_locations);
	EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident);
	EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
	EXIT_NOT_IMPLEMENTED(z.z_info.plane_compression != 0);
	EXIT_NOT_IMPLEMENTED(z.z_read_base_addr != z.z_write_base_addr);
	EXIT_NOT_IMPLEMENTED(z.z_write_base_addr == 0);
}

static void emit_invalid_stencil_plane(const HW::DepthRenderTarget& z, const HW::RenderControl& render_control,
                                       const HW::DepthControl& depth_control)
{
	Emulator::Agent::Lifecycle::StencilFrontierContext context {};
	context.stencil_enable     = depth_control.stencil_enable;
	context.clear_enable       = render_control.stencil_clear_enable;
	context.htile              = z.z_info.tile_surface_enable;
	context.depth_decompress   = render_control.depth_compress_disable;
	context.stencil_decompress = render_control.stencil_compress_disable;
	context.resummarize        = render_control.resummarize_enable;
	context.copy_centroid      = render_control.copy_centroid;
	context.copy_sample        = render_control.copy_sample;
	context.read_only          = z.depth_view.stencil_write_disable;
	context.read_base_present  = z.stencil_read_base_addr != 0;
	context.write_base_present = z.stencil_write_base_addr != 0;
	Emulator::Agent::Lifecycle::EmitStencilFrontier(context);
}

static State::StencilPlaneValidation validate_stencil_plane(const HW::DepthRenderTarget& z, const HW::RenderControl& render_control,
                                                            const HW::DepthControl& depth_control)
{
	const auto validation = State::ValidateStencilPlane(z, render_control, depth_control);
	if (validation == State::StencilPlaneValidation::Inactive)
	{
		return validation;
	}
	if (validation != State::StencilPlaneValidation::Valid)
	{
		emit_invalid_stencil_plane(z, render_control, depth_control);
	}
	EXIT_NOT_IMPLEMENTED(validation == State::StencilPlaneValidation::MissingReadBase);
	EXIT_NOT_IMPLEMENTED(validation == State::StencilPlaneValidation::MissingWriteBase);
	EXIT_NOT_IMPLEMENTED(validation == State::StencilPlaneValidation::MismatchedBases);
	EXIT_NOT_IMPLEMENTED(z.stencil_info.format == 0);
	EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != true);
	EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled);
	EXIT_NOT_IMPLEMENTED(z.stencil_info.partially_resident);
	return validation;
}

static void validate_depth_target_layout(const HW::DepthRenderTarget& z, bool ps5)
{
	if (ps5)
	{
		EXIT_NOT_IMPLEMENTED(z.depth_info.addr5_swizzle_mask != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.array_mode != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.pipe_config != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_width != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_height != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.macro_tile_aspect != 0);
		EXIT_NOT_IMPLEMENTED(z.depth_info.num_banks != 0);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.preload != 0);
	} else
	{
		EXIT_NOT_IMPLEMENTED(z.depth_info.addr5_swizzle_mask != 1);
		EXIT_NOT_IMPLEMENTED(z.depth_info.array_mode != 4);
		EXIT_NOT_IMPLEMENTED(z.depth_info.pipe_config != (Config::IsNeo() ? 0x12 : 0x0c));
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_width != 0);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.preload != 1);
	}
	EXIT_NOT_IMPLEMENTED(z.htile_surface.linear != 0);
	EXIT_NOT_IMPLEMENTED(z.htile_surface.full_cache != 0);
	EXIT_NOT_IMPLEMENTED(z.htile_surface.htile_uses_preload_win != 0);
	EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_width != 0);
	EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_height != 0);
	EXIT_NOT_IMPLEMENTED(z.htile_surface.dst_outside_zero_to_one != 0);
}

static void z_check(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control, const HW::DepthControl& depth_control)
{
	const auto z       = State::ResolveDepthStencilBasePairs(target);
	const auto stencil = validate_stencil_plane(z, render_control, depth_control);
	const auto usage   = State::ResolveDepthStencilUsage(z, render_control, depth_control);
	if (!usage.target_active && !render_control.depth_clear_enable && stencil == State::StencilPlaneValidation::Inactive)
	{
		return;
	}

	validate_depth_plane(z);
	if (z.z_info.format == 0 && stencil == State::StencilPlaneValidation::Inactive)
	{
		return;
	}

	const bool ps5 = Config::IsNextGen();
	validate_depth_target_layout(z, ps5);
	EXIT_NOT_IMPLEMENTED(z.depth_view.slice_start != 0);
	EXIT_NOT_IMPLEMENTED(z.depth_view.slice_max != 0);
	EXIT_NOT_IMPLEMENTED(z.depth_view.current_mip_level != 0);
	EXIT_NOT_IMPLEMENTED(z.depth_view.depth_write_disable);
	if (ps5)
	{
		EXIT_NOT_IMPLEMENTED(z.width != 0);
		EXIT_NOT_IMPLEMENTED(z.height != 0);
		EXIT_NOT_IMPLEMENTED(!State::ResolveDepthTargetExtent(z, true).valid);
	}
}

static void clip_print(const char* func, const HW::ClipControl& c)
{
	printf("%s\n", func);

	printf("\t user_clip_planes                    = 0x%02" PRIx8 "\n", c.user_clip_planes);
	printf("\t user_clip_plane_mode                = 0x%02" PRIx8 "\n", c.user_clip_plane_mode);
	printf("\t dx_clip_space                       = %s\n", c.dx_clip_space ? "true" : "false");
	printf("\t vertex_kill_any                     = %s\n", c.vertex_kill_any ? "true" : "false");
	printf("\t min_z_clip_disable                  = %s\n", c.min_z_clip_disable ? "true" : "false");
	printf("\t max_z_clip_disable                  = %s\n", c.max_z_clip_disable ? "true" : "false");
	printf("\t user_clip_plane_negate_y            = %s\n", c.user_clip_plane_negate_y ? "true" : "false");
	printf("\t clip_disable                        = %s\n", c.clip_disable ? "true" : "false");
	printf("\t user_clip_plane_cull_only           = %s\n", c.user_clip_plane_cull_only ? "true" : "false");
	printf("\t cull_on_clipping_error_disable      = %s\n", c.cull_on_clipping_error_disable ? "true" : "false");
	printf("\t linear_attribute_clip_enable        = %s\n", c.linear_attribute_clip_enable ? "true" : "false");
	printf("\t force_viewport_index_from_vs_enable = %s\n", c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void clip_check(const HW::ClipControl& c)
{
	EXIT_NOT_IMPLEMENTED(c.user_clip_planes != 0);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_mode != 0);
	// Both depth conventions are translated by ResolveViewportDepth and the
	// Vulkan pipeline carries the matching depth-clip control state.
	EXIT_NOT_IMPLEMENTED(c.vertex_kill_any != false);
	EXIT_NOT_IMPLEMENTED(c.min_z_clip_disable != false);
	EXIT_NOT_IMPLEMENTED(c.max_z_clip_disable != false);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_negate_y != false);
	EXIT_NOT_IMPLEMENTED(c.clip_disable != false);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_cull_only != false);
	EXIT_NOT_IMPLEMENTED(c.cull_on_clipping_error_disable != false);
	EXIT_NOT_IMPLEMENTED(c.linear_attribute_clip_enable != false);
	EXIT_NOT_IMPLEMENTED(c.force_viewport_index_from_vs_enable != false);
}

static void rc_print(const char* func, const HW::RenderControl& c)
{
	printf("%s\n", func);

	printf("\t depth_clear_enable       = %s\n", c.depth_clear_enable ? "true" : "false");
	printf("\t stencil_clear_enable     = %s\n", c.stencil_clear_enable ? "true" : "false");
	printf("\t depth_copy               = %s\n", c.depth_copy ? "true" : "false");
	printf("\t stencil_copy             = %s\n", c.stencil_copy ? "true" : "false");
	printf("\t resummarize_enable       = %s\n", c.resummarize_enable ? "true" : "false");
	printf("\t stencil_compress_disable = %s\n", c.stencil_compress_disable ? "true" : "false");
	printf("\t depth_compress_disable   = %s\n", c.depth_compress_disable ? "true" : "false");
	printf("\t copy_centroid            = %s\n", c.copy_centroid ? "true" : "false");
	printf("\t copy_sample              = %" PRIu8 "\n", c.copy_sample);
}

static void rc_check(const HW::RenderControl& c, bool allow_depth_stencil_copy)
{
	// EXIT_NOT_IMPLEMENTED(c.depth_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_clear_enable != false);
	// Resummarization only changes hierarchical depth metadata.
	// Vulkan maintains that implementation detail for the depth attachment, so
	// the regular attachment path preserves the logical depth/stencil contents.
	// EXIT_NOT_IMPLEMENTED(c.stencil_compress_disable != false);
	// EXIT_NOT_IMPLEMENTED(c.depth_compress_disable != false);
	EXIT_NOT_IMPLEMENTED((c.depth_copy || c.stencil_copy) && !allow_depth_stencil_copy);
}

static void mc_print(const char* func, const HW::ModeControl& c)
{
	printf("%s\n", func);

	printf("\t cull_front               = %s\n", c.cull_front ? "true" : "false");
	printf("\t cull_back                = %s\n", c.cull_back ? "true" : "false");
	printf("\t face                     = %s\n", c.face ? "true" : "false");
	printf("\t poly_mode                = %" PRIu8 "\n", c.poly_mode);
	printf("\t polymode_front_ptype     = %" PRIu8 "\n", c.polymode_front_ptype);
	printf("\t polymode_back_ptype      = %" PRIu8 "\n", c.polymode_back_ptype);
	printf("\t poly_offset_front_enable = %s\n", c.poly_offset_front_enable ? "true" : "false");
	printf("\t poly_offset_back_enable  = %s\n", c.poly_offset_back_enable ? "true" : "false");
	printf("\t vtx_window_offset_enable = %s\n", c.vtx_window_offset_enable ? "true" : "false");
	printf("\t provoking_vtx_last       = %s\n", c.provoking_vtx_last ? "true" : "false");
	printf("\t persp_corr_dis           = %s\n", c.persp_corr_dis ? "true" : "false");
}

static void mc_check(const HW::ModeControl& c)
{
	// EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	// EXIT_NOT_IMPLEMENTED(c.face != false);
	// Dual polygon mode with triangles selected for both faces is equivalent
	// to the solid-fill Vulkan state used below.
	EXIT_NOT_IMPLEMENTED(c.poly_mode != 0 && !(c.poly_mode == 1 && c.polymode_front_ptype == 2 && c.polymode_back_ptype == 2));
	EXIT_NOT_IMPLEMENTED(c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.vtx_window_offset_enable != false);
	EXIT_NOT_IMPLEMENTED(c.provoking_vtx_last != false);
	EXIT_NOT_IMPLEMENTED(c.persp_corr_dis != false);
}

static void bc_print(const char* func, const HW::BlendControl& c, const HW::BlendColor& color, const HW::ColorControl& cc)
{
	printf("%s\n", func);

	printf("\t color_srcblend       = %" PRIu8 "\n", c.color_srcblend);
	printf("\t color_comb_fcn       = %" PRIu8 "\n", c.color_comb_fcn);
	printf("\t color_destblend      = %" PRIu8 "\n", c.color_destblend);
	printf("\t alpha_srcblend       = %" PRIu8 "\n", c.alpha_srcblend);
	printf("\t alpha_comb_fcn       = %" PRIu8 "\n", c.alpha_comb_fcn);
	printf("\t alpha_destblend      = %" PRIu8 "\n", c.alpha_destblend);
	printf("\t separate_alpha_blend = %s\n", c.separate_alpha_blend ? "true" : "false");
	printf("\t enable               = %s\n", c.enable ? "true" : "false");
	printf("\t red                  = %f\n", color.red);
	printf("\t green                = %f\n", color.green);
	printf("\t blue                 = %f\n", color.blue);
	printf("\t alpha                = %f\n", color.alpha);
	printf("\t cc.mode              = %" PRIu8 "\n", cc.mode);
	printf("\t cc.op                = %" PRIu8 "\n", cc.op);
}

static void bc_check(const HW::BlendControl& /*c*/, const HW::BlendColor& color, const HW::ColorControl& cc)
{
	// EXIT_NOT_IMPLEMENTED(c.color_srcblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.color_comb_fcn != 0);
	// EXIT_NOT_IMPLEMENTED(c.color_destblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_srcblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_comb_fcn != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_destblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.separate_alpha_blend != false);
	// EXIT_NOT_IMPLEMENTED(c.enable != false);
	// Blend constants are emitted through vkCmdSetBlendConstants when the
	// pipeline uses dynamic blend state, so all finite values are valid.
	EXIT_NOT_IMPLEMENTED(!std::isfinite(color.red));
	EXIT_NOT_IMPLEMENTED(!std::isfinite(color.green));
	EXIT_NOT_IMPLEMENTED(!std::isfinite(color.blue));
	EXIT_NOT_IMPLEMENTED(!std::isfinite(color.alpha));
	// CB_COLOR_CONTROL.MODE is a three-bit field. MODE=3 selects the
	// fixed-function resolve path; every other encoded value is a valid
	// ordinary color draw and must not be rejected during state validation.
	EXIT_NOT_IMPLEMENTED(cc.op != 0xCC);
}

static void d_print(const char* func, const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& sm)
{
	printf("%s\n", func);

	printf("\t stencil_enable       = %s\n", c.stencil_enable ? "true" : "false");
	printf("\t z_enable             = %s\n", c.z_enable ? "true" : "false");
	printf("\t z_write_enable       = %s\n", c.z_write_enable ? "true" : "false");
	printf("\t depth_bounds_enable  = %s\n", c.depth_bounds_enable ? "true" : "false");
	printf("\t zfunc                = %" PRIu8 "\n", c.zfunc);
	printf("\t backface_enable      = %s\n", c.backface_enable ? "true" : "false");
	printf("\t stencilfunc          = %" PRIu8 "\n", c.stencilfunc);
	printf("\t stencilfunc_bf       = %" PRIu8 "\n", c.stencilfunc_bf);
	printf("\t color_writes_on_depth_fail_enable  = %s\n", c.color_writes_on_depth_fail_enable ? "true" : "false");
	printf("\t color_writes_on_depth_pass_disable = %s\n", c.color_writes_on_depth_pass_disable ? "true" : "false");
	printf("\t stencil_fail         = %" PRIu8 "\n", s.stencil_fail);
	printf("\t stencil_zpass        = %" PRIu8 "\n", s.stencil_zpass);
	printf("\t stencil_zfail        = %" PRIu8 "\n", s.stencil_zfail);
	printf("\t stencil_fail_bf      = %" PRIu8 "\n", s.stencil_fail_bf);
	printf("\t stencil_zpass_bf     = %" PRIu8 "\n", s.stencil_zpass_bf);
	printf("\t stencil_zfail_bf     = %" PRIu8 "\n", s.stencil_zfail_bf);
	printf("\t stencil_testval      = %" PRIu8 "\n", sm.stencil_testval);
	printf("\t stencil_mask         = %" PRIu8 "\n", sm.stencil_mask);
	printf("\t stencil_writemask    = %" PRIu8 "\n", sm.stencil_writemask);
	printf("\t stencil_opval        = %" PRIu8 "\n", sm.stencil_opval);
	printf("\t stencil_testval_bf   = %" PRIu8 "\n", sm.stencil_testval_bf);
	printf("\t stencil_mask_bf      = %" PRIu8 "\n", sm.stencil_mask_bf);
	printf("\t stencil_writemask_bf = %" PRIu8 "\n", sm.stencil_writemask_bf);
	printf("\t stencil_opval_bf     = %" PRIu8 "\n", sm.stencil_opval_bf);
}

static void d_check(const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& /*sm*/)
{
	// EXIT_NOT_IMPLEMENTED(c.stencil_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_write_enable != false);
	EXIT_NOT_IMPLEMENTED(c.depth_bounds_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.zfunc != 0);
	// Front and back stencil faces are translated independently below.
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc != 0);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc_bf != 0);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_fail_enable != false);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_pass_disable != false);
	// EXIT_NOT_IMPLEMENTED(s.stencil_fail != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zpass != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zfail != 0);
	// get_stencil_state() maps the back face independently when
	// DB_DEPTH_CONTROL.BACKFACE_ENABLE is set. Vulkan carries distinct front
	// and back VkStencilOpState values, including fail, depth-fail, and
	// depth-pass operations, so these are not unsupported states.
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval_bf != 0);
}

static void eqaa_print(const char* func, const HW::EqaaControl& c)
{
	printf("%s\n", func);

	printf("\t max_anchor_samples         = %" PRIu8 "\n", c.max_anchor_samples);
	printf("\t ps_iter_samples            = %" PRIu8 "\n", c.ps_iter_samples);
	printf("\t mask_export_num_samples    = %" PRIu8 "\n", c.mask_export_num_samples);
	printf("\t alpha_to_mask_num_samples  = %" PRIu8 "\n", c.alpha_to_mask_num_samples);
	printf("\t high_quality_intersections = %s\n", c.high_quality_intersections ? "true" : "false");
	printf("\t incoherent_eqaa_reads      = %s\n", c.incoherent_eqaa_reads ? "true" : "false");
	printf("\t interpolate_comp_z         = %s\n", c.interpolate_comp_z ? "true" : "false");
	printf("\t static_anchor_associations = %s\n", c.static_anchor_associations ? "true" : "false");
}

static void aa_print(const char* func, const HW::AaSampleControl& c, const HW::AaConfig& cf)
{
	printf("%s\n", func);

	printf("\t centroid_priority = %016" PRIx64 "\n", c.centroid_priority);
	for (int i = 0; i < 16; i++)
	{
		printf("\t locations[%d] = %08" PRIx32 "\n", i, c.locations[i]);
	}
	printf("\t msaa_num_samples      = %" PRIu8 "\n", cf.msaa_num_samples);
	printf("\t aa_mask_centroid_dtmn = %s\n", cf.aa_mask_centroid_dtmn ? "true" : "false");
	printf("\t max_sample_dist       = %" PRIu8 "\n", cf.max_sample_dist);
	printf("\t msaa_exposed_samples  = %" PRIu8 "\n", cf.msaa_exposed_samples);
}

// AA/EQAA registers remain programmed across draws. Validate their host mapping
// only after attachment inspection has established that Vulkan will rasterize
// with more than one sample; a single-sample pipeline cannot observe them.
void aa_check_for_attachment_samples(const HW::Context& hw, VkSampleCountFlagBits attachment_samples,
                                            VulkanSampleLocationState* sample_locations)
{
	EXIT_IF(sample_locations == nullptr);
	*sample_locations              = {};
	sample_locations->sample_count = attachment_samples;

	if (attachment_samples == VK_SAMPLE_COUNT_1_BIT)
	{
		return;
	}

	const auto& c         = hw.GetAaSampleControl();
	const auto& cf        = hw.GetAaConfig();
	const auto& eqaa      = hw.GetEqaaControl();
	const auto& scan_mode = hw.GetScanModeControl();

	if (!scan_mode.msaa_enable)
	{
		const auto& color_control  = hw.GetColorControl();
		const auto& render_control = hw.GetRenderControl();
		const auto& depth_control  = hw.GetDepthControl();
		std::fprintf(stderr,
		             "KYTY_AA_CONTRACT attachment_samples=%u msaa_enable=0 color_mode=%u color_op=0x%02x "
		             "aa_samples=%u exposed_samples=%u depth_test=%u stencil_test=%u depth_copy=%u stencil_copy=%u\n",
		             static_cast<uint32_t>(attachment_samples), static_cast<uint32_t>(color_control.mode), color_control.op,
		             cf.msaa_num_samples, cf.msaa_exposed_samples, depth_control.z_enable ? 1u : 0u,
		             depth_control.stencil_enable ? 1u : 0u, render_control.depth_copy ? 1u : 0u,
		             render_control.stencil_copy ? 1u : 0u);
	}
	// Vulkan requires the pipeline sample count to match the bound attachments.
	// MSAA_ENABLE does not change the color-buffer allocation, so keep deriving
	// rasterization samples from the attachment and use the EQAA state below to
	// decide whether fragment shading is per-sample.
	// Titles may leave PA_SC_AA_CONFIG at the default 0 while the color
	// buffers carry a sample count; the hardware derives MSAA from the CB.
	// Fall back to the attachment count in that case instead of rejecting.
	const VkSampleCountFlagBits msaa_num_samples =
	    (cf.msaa_num_samples == 0 ? attachment_samples
	                              : static_cast<VkSampleCountFlagBits>(decode_guest_sample_count(cf.msaa_num_samples)));
	const auto encode_sample_count = [](VkSampleCountFlagBits samples) -> uint32_t {
		switch (samples)
		{
			case VK_SAMPLE_COUNT_1_BIT: return 0;
			case VK_SAMPLE_COUNT_2_BIT: return 1;
			case VK_SAMPLE_COUNT_4_BIT: return 2;
			case VK_SAMPLE_COUNT_8_BIT: return 3;
			case VK_SAMPLE_COUNT_16_BIT: return 4;
			case VK_SAMPLE_COUNT_32_BIT: return 5;
			case VK_SAMPLE_COUNT_64_BIT: return 6;
			default: return 0;
		}
	};
	// PA_SC_AA_CONFIG may still contain the single-sample default while the
	// attachment and DB_EQAA already describe the active multisample target.
	// Compare EQAA against the effective attachment encoding in that case.
	const uint32_t effective_guest_samples =
	    cf.msaa_num_samples != 0 ? cf.msaa_num_samples : encode_sample_count(attachment_samples);
	EXIT_NOT_IMPLEMENTED(msaa_num_samples != attachment_samples);
	EXIT_NOT_IMPLEMENTED(cf.msaa_exposed_samples != 0 && cf.msaa_exposed_samples != effective_guest_samples);
	EXIT_NOT_IMPLEMENTED(cf.aa_mask_centroid_dtmn);

	// EQAA controls remain latched while a title switches between MSAA targets.
	// Vulkan exposes the raster sample count and optional sample shading, while
	// the remaining EQAA quality controls have no independent pipeline state.
	// Preserve the values for sample-shading selection in the pipeline and let
	// the rasterizer use the effective attachment count instead of rejecting a
	// valid draw because an EQAA field describes a lower anchor/iteration count.
	if (scan_mode.msaa_enable && std::getenv("KYTY_DUMP_AA_REGS") != nullptr)
	{
		static bool dumped_eqaa_mapping = false;
		if (!dumped_eqaa_mapping)
		{
			std::fprintf(stderr,
			             "KYTY_AA_MAPPING attachment=%u effective_guest=%u max_anchor=%u ps_iter=%u mask_export=%u "
			             "alpha_to_mask=%u\n",
			             static_cast<uint32_t>(attachment_samples), effective_guest_samples, eqaa.max_anchor_samples,
			             eqaa.ps_iter_samples, eqaa.mask_export_num_samples, eqaa.alpha_to_mask_num_samples);
			dumped_eqaa_mapping = true;
		}
	}

	auto* graphic_context = g_render_ctx->GetGraphicCtx();
	EXIT_IF(graphic_context == nullptr);
	const auto sample_location_status = BuildVulkanSampleLocationState(c.locations, c.centroid_priority, attachment_samples,
	                                                                    graphic_context->sample_location_capabilities, sample_locations);
	if (sample_location_status != VulkanSampleLocationStatus::Success)
	{
		std::fprintf(stderr, "KYTY_GRAPHICS: sample locations cannot be represented: %s\n",
		             VulkanSampleLocationStatusName(sample_location_status));
		EXIT_NOT_IMPLEMENTED(sample_location_status != VulkanSampleLocationStatus::Success);
	}
}

static void vp_print(const char* func, const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	printf("%s\n", func);

	printf("\t msaa_enable                    = %s\n", smc.msaa_enable ? "true" : "false");
	printf("\t vport_scissor_enable           = %s\n", smc.vport_scissor_enable ? "true" : "false");
	printf("\t line_stipple_enable            = %s\n", smc.line_stipple_enable ? "true" : "false");
	printf("\t vp[0].zmin                     = %f\n", vp.viewports[0].zmin);
	printf("\t vp[0].zmax                     = %f\n", vp.viewports[0].zmax);
	printf("\t vp[0].xscale                   = %f\n", vp.viewports[0].xscale);
	printf("\t vp[0].xoffset                  = %f\n", vp.viewports[0].xoffset);
	printf("\t vp[0].yscale                   = %f\n", vp.viewports[0].yscale);
	printf("\t vp[0].yoffset                  = %f\n", vp.viewports[0].yoffset);
	printf("\t vp[0].zscale                   = %f\n", vp.viewports[0].zscale);
	printf("\t vp[0].zoffset                  = %f\n", vp.viewports[0].zoffset);
	printf("\t vp[0].viewport_scissor_left    = %d\n", vp.viewports[0].viewport_scissor_left);
	printf("\t vp[0].viewport_scissor_top     = %d\n", vp.viewports[0].viewport_scissor_top);
	printf("\t vp[0].viewport_scissor_right   = %d\n", vp.viewports[0].viewport_scissor_right);
	printf("\t vp[0].viewport_scissor_bottom  = %d\n", vp.viewports[0].viewport_scissor_bottom);
	printf("\t transform_control              = 0x%08" PRIx32 "\n", vp.transform_control);
	printf("\t screen_scissor_left            = %d\n", vp.screen_scissor_left);
	printf("\t screen_scissor_top             = %d\n", vp.screen_scissor_top);
	printf("\t screen_scissor_right           = %d\n", vp.screen_scissor_right);
	printf("\t screen_scissor_bottom          = %d\n", vp.screen_scissor_bottom);
	printf("\t generic_scissor_left           = %d\n", vp.generic_scissor_left);
	printf("\t generic_scissor_top            = %d\n", vp.generic_scissor_top);
	printf("\t generic_scissor_right          = %d\n", vp.generic_scissor_right);
	printf("\t generic_scissor_bottom         = %d\n", vp.generic_scissor_bottom);
	printf("\t hw_offset_x                    = %u\n", vp.hw_offset_x);
	printf("\t hw_offset_y                    = %u\n", vp.hw_offset_y);
	printf("\t guard_band_horz_clip           = %f\n", vp.guard_band_horz_clip);
	printf("\t guard_band_vert_clip           = %f\n", vp.guard_band_vert_clip);
	printf("\t guard_band_horz_discard        = %f\n", vp.guard_band_horz_discard);
	printf("\t guard_band_vert_discard        = %f\n", vp.guard_band_vert_discard);
	printf("\t generic_scissor_window_offset_enable               = %s\n", vp.generic_scissor_window_offset_enable ? "true" : "false");
	printf("\t viewports[0].viewport_scissor_window_offset_enable = %s\n",
	       vp.viewports[0].viewport_scissor_window_offset_enable ? "true" : "false");
}

static void vp_check(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	bool ps5 = Config::IsNextGen();

	// This control also enables coverage processing for single-sample render
	// targets. Attachment sample counts, rather than this flag alone, select
	// Vulkan multisampling.
	// EXIT_NOT_IMPLEMENTED(smc.vport_scissor_enable);
	EXIT_NOT_IMPLEMENTED(smc.line_stipple_enable);

	// zmin/zmax are the hardware depth clamp. ResolveViewportDepth folds them
	// into the Vulkan viewport depth range, so any window is translatable.
	EXIT_NOT_IMPLEMENTED(!std::isfinite(vp.viewports[0].zmin) || !std::isfinite(vp.viewports[0].zmax));
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	EXIT_NOT_IMPLEMENTED(vp.transform_control != 1087);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_x != 60);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_y != 32);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_horz_clip - 33.133327f) > 0.001f);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_vert_clip - 59.629623f) > 0.001f);
	// Guard-band discard adj floats are clip-space host hints; Gen5 titles set
	// non-zero / non-1.0 values. Do not hard-fail on exact AGC defaults.
	(void)ps5;
	(void)vp.guard_band_horz_discard;
	(void)vp.guard_band_vert_discard;
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_left != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_top != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_right != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_bottom != 0);
	// viewport_scissor_window_offset_enable: window-offset detail; ignored for bring-up.
	// EXIT_NOT_IMPLEMENTED(viewport_scissor && vp.viewports[0].viewport_scissor_window_offset_enable != true);
}

void hw_check(const HW::Context& hw, bool allow_depth_stencil_copy)
{
	const auto& rt   = hw.GetRenderTarget(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& cc   = hw.GetColorControl();
	const auto& smc  = hw.GetScanModeControl();

	rt_check(rt);
	vp_check(vp, smc);
	z_check(z, rc, d);
	clip_check(c);
	rc_check(rc, allow_depth_stencil_copy);
	d_check(d, s, sm);
	mc_check(mc);
	bc_check(bc, bclr, cc);
	// CB_TARGET_MASK may enable multiple MRT slots (captured 0x0000ffff =
	// RT0..RT3 full RGBA). Pipeline creation still binds a single color
	// attachment and applies the RT0 nibble as colorWriteMask.
	const float depth_clear = hw.GetDepthClearValue();
	EXIT_NOT_IMPLEMENTED(rc.depth_clear_enable && (!std::isfinite(depth_clear) || depth_clear < 0.0f || depth_clear > 1.0f));
	// EXIT_NOT_IMPLEMENTED(hw.GetStencilClearValue() != 0);
}

void hw_print(const HW::Context& hw)
{
	const auto& rt   = hw.GetRenderTarget(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& eqaa = hw.GetEqaaControl();
	const auto& cc   = hw.GetColorControl();
	const auto& smc  = hw.GetScanModeControl();
	const auto& aa   = hw.GetAaSampleControl();
	const auto& ac   = hw.GetAaConfig();

	if (Kyty::Log::GetDirection() != Kyty::Log::Direction::Silent)
	{
		printf("Context\n");
		printf("\t GetRenderTargetMask()   = 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
		printf("\t GetDepthClearValue()    = %f\n", hw.GetDepthClearValue());
		printf("\t GetStencilClearValue()  = %" PRIu8 "\n", hw.GetStencilClearValue());
		printf("\t GetLineWidth()          = %f\n", hw.GetLineWidth());

		printf("%s", rt_print("RenderTraget:", rt).Concat(U"").C_Str());

		z_print("DepthRenderTraget:", z);
		vp_print("ScreenViewport:", vp, smc);
		clip_print("ClipControl:", c);
		rc_print("RenderControl:", rc);
		d_print("DepthStencilControlMask:", d, s, sm);
		mc_print("ModeControl:", mc);
		bc_print("BlendColorControl:", bc, bclr, cc);
		eqaa_print("EqaaControl:", eqaa);
		aa_print("AaSampleControl:", aa, ac);
	}
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
