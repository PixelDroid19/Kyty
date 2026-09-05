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

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// HW state print/check validators

void uc_print(const char* func, const HW::UserConfig& uc)
{
	KYTY_LOG_DEBUG("%s\n", func);

	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	KYTY_LOG_DEBUG("\t GetPrimType()         = 0x%08" PRIx32 "\n", uc.GetPrimType());
	KYTY_LOG_DEBUG("\t primitive_group_size  = 0x%04" PRIx16 "\n", ge_cntl.primitive_group_size);
	KYTY_LOG_DEBUG("\t vertex_group_size     = 0x%04" PRIx16 "\n", ge_cntl.vertex_group_size);
	KYTY_LOG_DEBUG("\t en_user_vgpr1         = %s\n", user_en.vgpr1 ? "true" : "false");
	KYTY_LOG_DEBUG("\t en_user_vgpr2         = %s\n", user_en.vgpr2 ? "true" : "false");
	KYTY_LOG_DEBUG("\t en_user_vgpr3         = %s\n", user_en.vgpr3 ? "true" : "false");
}

void uc_check(const HW::UserConfig& uc)
{
	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	// GE_CNTL group sizes are host scheduling hints. Gen5 titles emit values
	// other than 0/0x40; accept the documented max of 0x40 (same bound as
	// Kyty) rather than an exact-value whitelist.
	if (ge_cntl.primitive_group_size > 0x0040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ge_cntl.primitive_group_size > 0x0040 condition ignored (continuing)\n"); }
	if (ge_cntl.vertex_group_size > 0x0040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ge_cntl.vertex_group_size > 0x0040 condition ignored (continuing)\n"); }
	if (user_en.vgpr1 != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_en.vgpr1 != false condition ignored (continuing)\n"); }
	if (user_en.vgpr2 != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_en.vgpr2 != false condition ignored (continuing)\n"); }
	if (user_en.vgpr3 != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_en.vgpr3 != false condition ignored (continuing)\n"); }
}

void sh_print(const char* func, const HW::Shader& /*uc*/)
{
	KYTY_LOG_DEBUG("%s\n", func);
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
			if (rt.pitch.pitch_div8_minus1 != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.pitch.pitch_div8_minus1 != 0 condition ignored (continuing)\n"); }
			if (rt.pitch.fmask_pitch_div8_minus1 != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.pitch.fmask_pitch_div8_minus1 != 0 condition ignored (continuing)\n"); }
			if (rt.slice.slice_div64_minus1 != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.slice.slice_div64_minus1 != 0 condition ignored (continuing)\n"); }
		}
		if (!ps5 && rt.view.base_array_slice_index != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.view.base_array_slice_index != 0x00000000 condition ignored (continuing)\n"); }
		if (!ps5 && rt.view.last_array_slice_index != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.view.last_array_slice_index != 0x00000000 condition ignored (continuing)\n"); }
		if (rt.view.current_mip_level != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.view.current_mip_level != 0x00000000 condition ignored (continuing)\n"); }
		if (rt.info.fmask_compression_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.fmask_compression_enable != false condition ignored (continuing)\n"); }

		// EXIT_NOT_IMPLEMENTED(rt.info.fmask_compression_mode != 0x00000000);
		if (rt.info.fmask_data_compression_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.fmask_data_compression_disable != false condition ignored (continuing)\n"); }
		if (rt.info.fmask_one_frag_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.fmask_one_frag_mode != false condition ignored (continuing)\n"); }

		if (rt.info.cmask_fast_clear_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.cmask_fast_clear_enable != false condition ignored (continuing)\n"); }
		if (rt.info.dcc_compression_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.dcc_compression_enable != false condition ignored (continuing)\n"); }
		if (!(rt.attrib.tile_mode == 0x0d) && rt.info.neo_mode != Config::IsNeo()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(rt.attrib.tile_mode == 0x0d) && rt.info.neo_mode != Config::IsNeo() condition ignored (continuing)\n"); }
		if (rt.info.cmask_tile_mode != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.cmask_tile_mode != 0x00000000 condition ignored (continuing)\n"); }
		if (rt.info.cmask_tile_mode_neo != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.info.cmask_tile_mode_neo != 0x00000000 condition ignored (continuing)\n"); }
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
		if (rt.attrib.num_samples != rt.attrib.num_fragments) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib.num_samples != rt.attrib.num_fragments condition ignored (continuing)\n"); }
		if (ps5)
		{
			if (rt.attrib2.width == 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.width == 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib2.height == 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.height == 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib2.num_mip_levels != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.num_mip_levels != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.depth != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.depth != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.tile_mode != 0x0000001b) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.tile_mode != 0x0000001b condition ignored (continuing)\n"); }
			if (rt.attrib3.dimension != 0x00000001) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.dimension != 0x00000001 condition ignored (continuing)\n"); }
			// Pipe alignment only affects the separately addressed CMASK/DCC
			// metadata surfaces. An inactive surface cannot alter the color image
			// layout, while active metadata remains rejected below.
			if (rt_uses_cmask_metadata(rt) && !rt.attrib3.cmask_pipe_aligned) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt_uses_cmask_metadata(rt) && !rt.attrib3.cmask_pipe_aligned condition ignored (continuing)\n"); }
			if (rt_uses_dcc_metadata(rt) && !rt.attrib3.dcc_pipe_aligned) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt_uses_dcc_metadata(rt) && !rt.attrib3.dcc_pipe_aligned condition ignored (continuing)\n"); }
		} else
		{
			if (rt.attrib2.width != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.width != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib2.height != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.height != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib2.num_mip_levels != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib2.num_mip_levels != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.depth != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.depth != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.tile_mode != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.tile_mode != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.dimension != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.dimension != 0x00000000 condition ignored (continuing)\n"); }
			if (rt.attrib3.cmask_pipe_aligned != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.cmask_pipe_aligned != false condition ignored (continuing)\n"); }
			if (rt.attrib3.dcc_pipe_aligned != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.attrib3.dcc_pipe_aligned != false condition ignored (continuing)\n"); }
		}
		// EXIT_NOT_IMPLEMENTED(rt.dcc_max_uncompressed_block_size != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.max_compressed_block_size != 0x00000000);
		if (rt.dcc.min_compressed_block_size != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.dcc.min_compressed_block_size != 0x00000000 condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(rt.dcc.color_transform != 0x00000000);
		if (rt.dcc.overwrite_combiner_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.dcc.overwrite_combiner_disable != false condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(rt.dcc.force_independent_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.independent_128b_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.data_write_on_dcc_clear_to_reg != false);
		if (rt.dcc.dcc_clear_key_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.dcc.dcc_clear_key_enable != false condition ignored (continuing)\n"); }
		if (rt.cmask.addr != 0x0000000000000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.cmask.addr != 0x0000000000000000 condition ignored (continuing)\n"); }
		if (rt.cmask_slice.slice_minus1 != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.cmask_slice.slice_minus1 != 0x00000000 condition ignored (continuing)\n"); }
		if (rt.fmask.addr != 0x0000000000000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.fmask.addr != 0x0000000000000000 condition ignored (continuing)\n"); }
		if (rt.fmask_slice.slice_minus1 != 0x00000000 && rt.fmask_slice.slice_minus1 != rt.slice.slice_div64_minus1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.fmask_slice.slice_minus1 != 0x00000000 && rt.fmask_slice.slice_minus1 != rt.slice.slice_div64_minus1 condition ignored (continuing)\n"); }
		// CLEAR_WORD0/1 hold the raw clear pixel; format-aware decode is in
		// DecodeGuestColorClearWords (Mesa/RADV packing). Non-zero words are legal.
		if (rt.dcc_addr.addr != 0x0000000000000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.dcc_addr.addr != 0x0000000000000000 condition ignored (continuing)\n"); }
		if (ps5)
		{
			if (rt.size.width != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.size.width != 0 condition ignored (continuing)\n"); }
			if (rt.size.height != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rt.size.height != 0 condition ignored (continuing)\n"); }
		}
	}
}

static void z_print(const char* func, const HW::DepthRenderTarget& z)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t z_info.format                         = 0x%08" PRIx32 "\n", z.z_info.format);
	KYTY_LOG_DEBUG("\t z_info.tile_mode_index                = 0x%08" PRIx32 "\n", z.z_info.tile_mode_index);
	KYTY_LOG_DEBUG("\t z_info.num_samples                    = 0x%08" PRIx32 "\n", z.z_info.num_samples);
	KYTY_LOG_DEBUG("\t z_info.tile_surface_enable            = %s\n", z.z_info.tile_surface_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_info.expclear_enabled               = %s\n", z.z_info.expclear_enabled ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_info.zrange_precision               = 0x%08" PRIx32 "\n", z.z_info.zrange_precision);
	KYTY_LOG_DEBUG("\t z_info.embedded_sample_locations      = %s\n", z.z_info.embedded_sample_locations ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_info.partially_resident             = %s\n", z.z_info.partially_resident ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_info.num_mip_levels                 = 0x%02" PRIx8 "\n", z.z_info.num_mip_levels);
	KYTY_LOG_DEBUG("\t z_info.plane_compression              = 0x%02" PRIx8 "\n", z.z_info.plane_compression);
	KYTY_LOG_DEBUG("\t stencil_info.format                   = 0x%08" PRIx32 "\n", z.stencil_info.format);
	KYTY_LOG_DEBUG("\t stencil_info.tile_stencil_disable     = %s\n", z.stencil_info.tile_stencil_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_info.expclear_enabled         = %s\n", z.stencil_info.expclear_enabled ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_info.tile_mode_index          = 0x%08" PRIx32 "\n", z.stencil_info.tile_mode_index);
	KYTY_LOG_DEBUG("\t stencil_info.tile_split               = 0x%08" PRIx32 "\n", z.stencil_info.tile_split);
	KYTY_LOG_DEBUG("\t stencil_info.texture_compatible_stencil = %s\n", z.stencil_info.texture_compatible_stencil ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_info.partially_resident       = %s\n", z.stencil_info.partially_resident ? "true" : "false");
	KYTY_LOG_DEBUG("\t depth_info.addr5_swizzle_mask         = 0x%08" PRIx32 "\n", z.depth_info.addr5_swizzle_mask);
	KYTY_LOG_DEBUG("\t depth_info.array_mode                 = 0x%08" PRIx32 "\n", z.depth_info.array_mode);
	KYTY_LOG_DEBUG("\t depth_info.pipe_config                = 0x%08" PRIx32 "\n", z.depth_info.pipe_config);
	KYTY_LOG_DEBUG("\t depth_info.bank_width                 = 0x%08" PRIx32 "\n", z.depth_info.bank_width);
	KYTY_LOG_DEBUG("\t depth_info.bank_height                = 0x%08" PRIx32 "\n", z.depth_info.bank_height);
	KYTY_LOG_DEBUG("\t depth_info.macro_tile_aspect          = 0x%08" PRIx32 "\n", z.depth_info.macro_tile_aspect);
	KYTY_LOG_DEBUG("\t depth_info.num_banks                  = 0x%08" PRIx32 "\n", z.depth_info.num_banks);
	KYTY_LOG_DEBUG("\t depth_view.slice_start                = 0x%08" PRIx32 "\n", z.depth_view.slice_start);
	KYTY_LOG_DEBUG("\t depth_view.slice_max                  = 0x%08" PRIx32 "\n", z.depth_view.slice_max);
	KYTY_LOG_DEBUG("\t depth_view.current_mip_level          = 0x%02" PRIx8 "\n", z.depth_view.current_mip_level);
	KYTY_LOG_DEBUG("\t depth_view.depth_write_disable        = %s\n", z.depth_view.depth_write_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t depth_view.stencil_write_disable      = %s\n", z.depth_view.stencil_write_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t htile_surface.linear                  = 0x%08" PRIx32 "\n", z.htile_surface.linear);
	KYTY_LOG_DEBUG("\t htile_surface.full_cache              = 0x%08" PRIx32 "\n", z.htile_surface.full_cache);
	KYTY_LOG_DEBUG("\t htile_surface.htile_uses_preload_win  = 0x%08" PRIx32 "\n", z.htile_surface.htile_uses_preload_win);
	KYTY_LOG_DEBUG("\t htile_surface.preload                 = 0x%08" PRIx32 "\n", z.htile_surface.preload);
	KYTY_LOG_DEBUG("\t htile_surface.prefetch_width          = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_width);
	KYTY_LOG_DEBUG("\t htile_surface.prefetch_height         = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_height);
	KYTY_LOG_DEBUG("\t htile_surface.dst_outside_zero_to_one = 0x%08" PRIx32 "\n", z.htile_surface.dst_outside_zero_to_one);
	KYTY_LOG_DEBUG("\t z_read_base_addr                      = 0x%016" PRIx64 "\n", z.z_read_base_addr);
	KYTY_LOG_DEBUG("\t stencil_read_base_addr                = 0x%016" PRIx64 "\n", z.stencil_read_base_addr);
	KYTY_LOG_DEBUG("\t z_write_base_addr                     = 0x%016" PRIx64 "\n", z.z_write_base_addr);
	KYTY_LOG_DEBUG("\t stencil_write_base_addr               = 0x%016" PRIx64 "\n", z.stencil_write_base_addr);
	KYTY_LOG_DEBUG("\t pitch_div8_minus1                     = 0x%08" PRIx32 "\n", z.pitch_div8_minus1);
	KYTY_LOG_DEBUG("\t height_div8_minus1                    = 0x%08" PRIx32 "\n", z.height_div8_minus1);
	KYTY_LOG_DEBUG("\t slice_div64_minus1                    = 0x%08" PRIx32 "\n", z.slice_div64_minus1);
	KYTY_LOG_DEBUG("\t htile_data_base_addr                  = 0x%016" PRIx64 "\n", z.htile_data_base_addr);
	KYTY_LOG_DEBUG("\t width                                 = 0x%08" PRIx32 "\n", z.width);
	KYTY_LOG_DEBUG("\t height                                = 0x%08" PRIx32 "\n", z.height);
	KYTY_LOG_DEBUG("\t size.x_max                            = 0x%04" PRIx16 "\n", z.size.x_max);
	KYTY_LOG_DEBUG("\t size.y_max                            = 0x%04" PRIx16 "\n", z.size.y_max);
}

static void validate_depth_plane(const HW::DepthRenderTarget& z)
{
	if (z.z_info.format == 0)
	{
		if (z.z_info.tile_mode_index != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.tile_mode_index != 0 condition ignored (continuing)\n"); }
		if (z.z_info.num_samples != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.num_samples != 0 condition ignored (continuing)\n"); }
		if (z.z_info.tile_surface_enable) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.tile_surface_enable condition ignored (continuing)\n"); }
		if (z.z_info.expclear_enabled) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.expclear_enabled condition ignored (continuing)\n"); }
		if (z.z_info.embedded_sample_locations) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.embedded_sample_locations condition ignored (continuing)\n"); }
		if (z.z_info.partially_resident) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.partially_resident condition ignored (continuing)\n"); }
		if (z.z_info.num_mip_levels != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.num_mip_levels != 0 condition ignored (continuing)\n"); }
		if (z.z_info.plane_compression != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.plane_compression != 0 condition ignored (continuing)\n"); }
		return;
	}
	if (z.z_info.format != 0x00000003) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.format != 0x00000003 condition ignored (continuing)\n"); }
	(void)decode_guest_sample_count(z.z_info.num_samples);
	if (z.z_info.expclear_enabled) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.expclear_enabled condition ignored (continuing)\n"); }
	if (z.z_info.zrange_precision != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.zrange_precision != 1 condition ignored (continuing)\n"); }
	if (z.z_info.embedded_sample_locations) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.embedded_sample_locations condition ignored (continuing)\n"); }
	if (z.z_info.partially_resident) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.partially_resident condition ignored (continuing)\n"); }
	if (z.z_info.num_mip_levels != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.num_mip_levels != 0 condition ignored (continuing)\n"); }
	if (z.z_info.plane_compression != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.plane_compression != 0 condition ignored (continuing)\n"); }
	if (z.z_read_base_addr != z.z_write_base_addr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_read_base_addr != z.z_write_base_addr condition ignored (continuing)\n"); }
	if (z.z_write_base_addr == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_write_base_addr == 0 condition ignored (continuing)\n"); }
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
	if (validation == State::StencilPlaneValidation::MissingReadBase) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: validation == State::StencilPlaneValidation::MissingReadBase condition ignored (continuing)\n"); }
	if (validation == State::StencilPlaneValidation::MissingWriteBase) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: validation == State::StencilPlaneValidation::MissingWriteBase condition ignored (continuing)\n"); }
	if (validation == State::StencilPlaneValidation::MismatchedBases) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: validation == State::StencilPlaneValidation::MismatchedBases condition ignored (continuing)\n"); }
	if (z.stencil_info.format == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.stencil_info.format == 0 condition ignored (continuing)\n"); }
	if (z.stencil_info.tile_stencil_disable != true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.stencil_info.tile_stencil_disable != true condition ignored (continuing)\n"); }
	if (z.stencil_info.expclear_enabled) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.stencil_info.expclear_enabled condition ignored (continuing)\n"); }
	if (z.stencil_info.partially_resident) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.stencil_info.partially_resident condition ignored (continuing)\n"); }
	return validation;
}

static void validate_depth_target_layout(const HW::DepthRenderTarget& z, bool ps5)
{
	if (ps5)
	{
		if (z.depth_info.addr5_swizzle_mask != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.addr5_swizzle_mask != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.array_mode != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.array_mode != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.pipe_config != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.pipe_config != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.bank_width != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.bank_width != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.bank_height != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.bank_height != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.macro_tile_aspect != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.macro_tile_aspect != 0 condition ignored (continuing)\n"); }
		if (z.depth_info.num_banks != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.num_banks != 0 condition ignored (continuing)\n"); }
		if (z.htile_surface.preload != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.preload != 0 condition ignored (continuing)\n"); }
	} else
	{
		if (z.depth_info.addr5_swizzle_mask != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.addr5_swizzle_mask != 1 condition ignored (continuing)\n"); }
		if (z.depth_info.array_mode != 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.array_mode != 4 condition ignored (continuing)\n"); }
		if (z.depth_info.pipe_config != (Config::IsNeo() ? 0x12 : 0x0c)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.pipe_config != (Config::IsNeo() ? 0x12 : 0x0c) condition ignored (continuing)\n"); }
		if (z.depth_info.bank_width != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_info.bank_width != 0 condition ignored (continuing)\n"); }
		if (z.htile_surface.preload != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.preload != 1 condition ignored (continuing)\n"); }
	}
	if (z.htile_surface.linear != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.linear != 0 condition ignored (continuing)\n"); }
	if (z.htile_surface.full_cache != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.full_cache != 0 condition ignored (continuing)\n"); }
	if (z.htile_surface.htile_uses_preload_win != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.htile_uses_preload_win != 0 condition ignored (continuing)\n"); }
	if (z.htile_surface.prefetch_width != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.prefetch_width != 0 condition ignored (continuing)\n"); }
	if (z.htile_surface.prefetch_height != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.prefetch_height != 0 condition ignored (continuing)\n"); }
	if (z.htile_surface.dst_outside_zero_to_one != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.htile_surface.dst_outside_zero_to_one != 0 condition ignored (continuing)\n"); }
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
	if (z.depth_view.slice_start != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_view.slice_start != 0 condition ignored (continuing)\n"); }
	if (z.depth_view.slice_max != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_view.slice_max != 0 condition ignored (continuing)\n"); }
	if (z.depth_view.current_mip_level != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_view.current_mip_level != 0 condition ignored (continuing)\n"); }
	if (z.depth_view.depth_write_disable) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.depth_view.depth_write_disable condition ignored (continuing)\n"); }
	if (ps5)
	{
		if (z.width != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.width != 0 condition ignored (continuing)\n"); }
		if (z.height != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.height != 0 condition ignored (continuing)\n"); }
		if (!State::ResolveDepthTargetExtent(z, true).valid) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !State::ResolveDepthTargetExtent(z, true).valid condition ignored (continuing)\n"); }
	}
}

static void clip_print(const char* func, const HW::ClipControl& c)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t user_clip_planes                    = 0x%02" PRIx8 "\n", c.user_clip_planes);
	KYTY_LOG_DEBUG("\t user_clip_plane_mode                = 0x%02" PRIx8 "\n", c.user_clip_plane_mode);
	KYTY_LOG_DEBUG("\t dx_clip_space                       = %s\n", c.dx_clip_space ? "true" : "false");
	KYTY_LOG_DEBUG("\t vertex_kill_any                     = %s\n", c.vertex_kill_any ? "true" : "false");
	KYTY_LOG_DEBUG("\t min_z_clip_disable                  = %s\n", c.min_z_clip_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t max_z_clip_disable                  = %s\n", c.max_z_clip_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t user_clip_plane_negate_y            = %s\n", c.user_clip_plane_negate_y ? "true" : "false");
	KYTY_LOG_DEBUG("\t clip_disable                        = %s\n", c.clip_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t user_clip_plane_cull_only           = %s\n", c.user_clip_plane_cull_only ? "true" : "false");
	KYTY_LOG_DEBUG("\t cull_on_clipping_error_disable      = %s\n", c.cull_on_clipping_error_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t linear_attribute_clip_enable        = %s\n", c.linear_attribute_clip_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t force_viewport_index_from_vs_enable = %s\n", c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void clip_check(const HW::ClipControl& c)
{
	if (c.user_clip_planes != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.user_clip_planes != 0 condition ignored (continuing)\n"); }
	if (c.user_clip_plane_mode != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.user_clip_plane_mode != 0 condition ignored (continuing)\n"); }
	// Both depth conventions are translated by ResolveViewportDepth and the
	// Vulkan pipeline carries the matching depth-clip control state.
	if (c.vertex_kill_any != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.vertex_kill_any != false condition ignored (continuing)\n"); }
	if (c.min_z_clip_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.min_z_clip_disable != false condition ignored (continuing)\n"); }
	if (c.max_z_clip_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.max_z_clip_disable != false condition ignored (continuing)\n"); }
	if (c.user_clip_plane_negate_y != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.user_clip_plane_negate_y != false condition ignored (continuing)\n"); }
	if (c.clip_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.clip_disable != false condition ignored (continuing)\n"); }
	if (c.user_clip_plane_cull_only != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.user_clip_plane_cull_only != false condition ignored (continuing)\n"); }
	if (c.cull_on_clipping_error_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.cull_on_clipping_error_disable != false condition ignored (continuing)\n"); }
	if (c.linear_attribute_clip_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.linear_attribute_clip_enable != false condition ignored (continuing)\n"); }
	if (c.force_viewport_index_from_vs_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.force_viewport_index_from_vs_enable != false condition ignored (continuing)\n"); }
}

static void rc_print(const char* func, const HW::RenderControl& c)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t depth_clear_enable       = %s\n", c.depth_clear_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_clear_enable     = %s\n", c.stencil_clear_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t depth_copy               = %s\n", c.depth_copy ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_copy             = %s\n", c.stencil_copy ? "true" : "false");
	KYTY_LOG_DEBUG("\t resummarize_enable       = %s\n", c.resummarize_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_compress_disable = %s\n", c.stencil_compress_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t depth_compress_disable   = %s\n", c.depth_compress_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t copy_centroid            = %s\n", c.copy_centroid ? "true" : "false");
	KYTY_LOG_DEBUG("\t copy_sample              = %" PRIu8 "\n", c.copy_sample);
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
	if ((c.depth_copy || c.stencil_copy) && !allow_depth_stencil_copy) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (c.depth_copy || c.stencil_copy) && !allow_depth_stencil_copy condition ignored (continuing)\n"); }
}

static void mc_print(const char* func, const HW::ModeControl& c)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t cull_front               = %s\n", c.cull_front ? "true" : "false");
	KYTY_LOG_DEBUG("\t cull_back                = %s\n", c.cull_back ? "true" : "false");
	KYTY_LOG_DEBUG("\t face                     = %s\n", c.face ? "true" : "false");
	KYTY_LOG_DEBUG("\t poly_mode                = %" PRIu8 "\n", c.poly_mode);
	KYTY_LOG_DEBUG("\t polymode_front_ptype     = %" PRIu8 "\n", c.polymode_front_ptype);
	KYTY_LOG_DEBUG("\t polymode_back_ptype      = %" PRIu8 "\n", c.polymode_back_ptype);
	KYTY_LOG_DEBUG("\t poly_offset_front_enable = %s\n", c.poly_offset_front_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t poly_offset_back_enable  = %s\n", c.poly_offset_back_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t vtx_window_offset_enable = %s\n", c.vtx_window_offset_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t provoking_vtx_last       = %s\n", c.provoking_vtx_last ? "true" : "false");
	KYTY_LOG_DEBUG("\t persp_corr_dis           = %s\n", c.persp_corr_dis ? "true" : "false");
}

static void mc_check(const HW::ModeControl& c)
{
	// EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	// EXIT_NOT_IMPLEMENTED(c.face != false);
	// Dual polygon mode with triangles selected for both faces is equivalent
	// to the solid-fill Vulkan state used below.
	if (c.poly_mode != 0 && !(c.poly_mode == 1 && c.polymode_front_ptype == 2 && c.polymode_back_ptype == 2)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.poly_mode != 0 && !(c.poly_mode == 1 && c.polymode_front_ptype == 2 && c.polymode_back_ptype == 2) condition ignored (continuing)\n"); }
	if (c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2 condition ignored (continuing)\n"); }
	if (c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2 condition ignored (continuing)\n"); }
	if (c.vtx_window_offset_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.vtx_window_offset_enable != false condition ignored (continuing)\n"); }
	if (c.provoking_vtx_last != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.provoking_vtx_last != false condition ignored (continuing)\n"); }
	if (c.persp_corr_dis != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.persp_corr_dis != false condition ignored (continuing)\n"); }
}

static void bc_print(const char* func, const HW::BlendControl& c, const HW::BlendColor& color, const HW::ColorControl& cc)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t color_srcblend       = %" PRIu8 "\n", c.color_srcblend);
	KYTY_LOG_DEBUG("\t color_comb_fcn       = %" PRIu8 "\n", c.color_comb_fcn);
	KYTY_LOG_DEBUG("\t color_destblend      = %" PRIu8 "\n", c.color_destblend);
	KYTY_LOG_DEBUG("\t alpha_srcblend       = %" PRIu8 "\n", c.alpha_srcblend);
	KYTY_LOG_DEBUG("\t alpha_comb_fcn       = %" PRIu8 "\n", c.alpha_comb_fcn);
	KYTY_LOG_DEBUG("\t alpha_destblend      = %" PRIu8 "\n", c.alpha_destblend);
	KYTY_LOG_DEBUG("\t separate_alpha_blend = %s\n", c.separate_alpha_blend ? "true" : "false");
	KYTY_LOG_DEBUG("\t enable               = %s\n", c.enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t red                  = %f\n", color.red);
	KYTY_LOG_DEBUG("\t green                = %f\n", color.green);
	KYTY_LOG_DEBUG("\t blue                 = %f\n", color.blue);
	KYTY_LOG_DEBUG("\t alpha                = %f\n", color.alpha);
	KYTY_LOG_DEBUG("\t cc.mode              = %" PRIu8 "\n", cc.mode);
	KYTY_LOG_DEBUG("\t cc.op                = %" PRIu8 "\n", cc.op);
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
	if (!std::isfinite(color.red)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !std::isfinite(color.red) condition ignored (continuing)\n"); }
	if (!std::isfinite(color.green)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !std::isfinite(color.green) condition ignored (continuing)\n"); }
	if (!std::isfinite(color.blue)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !std::isfinite(color.blue) condition ignored (continuing)\n"); }
	if (!std::isfinite(color.alpha)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !std::isfinite(color.alpha) condition ignored (continuing)\n"); }
	// CB_COLOR_CONTROL.MODE is a three-bit field. MODE=3 selects the
	// fixed-function resolve path; every other encoded value is a valid
	// ordinary color draw and must not be rejected during state validation.
	if (cc.op != 0xCC) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cc.op != 0xCC condition ignored (continuing)\n"); }
}

static void d_print(const char* func, const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& sm)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t stencil_enable       = %s\n", c.stencil_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_enable             = %s\n", c.z_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t z_write_enable       = %s\n", c.z_write_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t depth_bounds_enable  = %s\n", c.depth_bounds_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t zfunc                = %" PRIu8 "\n", c.zfunc);
	KYTY_LOG_DEBUG("\t backface_enable      = %s\n", c.backface_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencilfunc          = %" PRIu8 "\n", c.stencilfunc);
	KYTY_LOG_DEBUG("\t stencilfunc_bf       = %" PRIu8 "\n", c.stencilfunc_bf);
	KYTY_LOG_DEBUG("\t color_writes_on_depth_fail_enable  = %s\n", c.color_writes_on_depth_fail_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t color_writes_on_depth_pass_disable = %s\n", c.color_writes_on_depth_pass_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t stencil_fail         = %" PRIu8 "\n", s.stencil_fail);
	KYTY_LOG_DEBUG("\t stencil_zpass        = %" PRIu8 "\n", s.stencil_zpass);
	KYTY_LOG_DEBUG("\t stencil_zfail        = %" PRIu8 "\n", s.stencil_zfail);
	KYTY_LOG_DEBUG("\t stencil_fail_bf      = %" PRIu8 "\n", s.stencil_fail_bf);
	KYTY_LOG_DEBUG("\t stencil_zpass_bf     = %" PRIu8 "\n", s.stencil_zpass_bf);
	KYTY_LOG_DEBUG("\t stencil_zfail_bf     = %" PRIu8 "\n", s.stencil_zfail_bf);
	KYTY_LOG_DEBUG("\t stencil_testval      = %" PRIu8 "\n", sm.stencil_testval);
	KYTY_LOG_DEBUG("\t stencil_mask         = %" PRIu8 "\n", sm.stencil_mask);
	KYTY_LOG_DEBUG("\t stencil_writemask    = %" PRIu8 "\n", sm.stencil_writemask);
	KYTY_LOG_DEBUG("\t stencil_opval        = %" PRIu8 "\n", sm.stencil_opval);
	KYTY_LOG_DEBUG("\t stencil_testval_bf   = %" PRIu8 "\n", sm.stencil_testval_bf);
	KYTY_LOG_DEBUG("\t stencil_mask_bf      = %" PRIu8 "\n", sm.stencil_mask_bf);
	KYTY_LOG_DEBUG("\t stencil_writemask_bf = %" PRIu8 "\n", sm.stencil_writemask_bf);
	KYTY_LOG_DEBUG("\t stencil_opval_bf     = %" PRIu8 "\n", sm.stencil_opval_bf);
}

static void d_check(const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& /*sm*/)
{
	// EXIT_NOT_IMPLEMENTED(c.stencil_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_write_enable != false);
	if (c.depth_bounds_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.depth_bounds_enable != false condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(c.zfunc != 0);
	// Front and back stencil faces are translated independently below.
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc != 0);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc_bf != 0);
	if (c.color_writes_on_depth_fail_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.color_writes_on_depth_fail_enable != false condition ignored (continuing)\n"); }
	if (c.color_writes_on_depth_pass_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: c.color_writes_on_depth_pass_disable != false condition ignored (continuing)\n"); }
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
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t max_anchor_samples         = %" PRIu8 "\n", c.max_anchor_samples);
	KYTY_LOG_DEBUG("\t ps_iter_samples            = %" PRIu8 "\n", c.ps_iter_samples);
	KYTY_LOG_DEBUG("\t mask_export_num_samples    = %" PRIu8 "\n", c.mask_export_num_samples);
	KYTY_LOG_DEBUG("\t alpha_to_mask_num_samples  = %" PRIu8 "\n", c.alpha_to_mask_num_samples);
	KYTY_LOG_DEBUG("\t high_quality_intersections = %s\n", c.high_quality_intersections ? "true" : "false");
	KYTY_LOG_DEBUG("\t incoherent_eqaa_reads      = %s\n", c.incoherent_eqaa_reads ? "true" : "false");
	KYTY_LOG_DEBUG("\t interpolate_comp_z         = %s\n", c.interpolate_comp_z ? "true" : "false");
	KYTY_LOG_DEBUG("\t static_anchor_associations = %s\n", c.static_anchor_associations ? "true" : "false");
}

static void aa_print(const char* func, const HW::AaSampleControl& c, const HW::AaConfig& cf)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t centroid_priority = %016" PRIx64 "\n", c.centroid_priority);
	for (int i = 0; i < 16; i++)
	{
		KYTY_LOG_DEBUG("\t locations[%d] = %08" PRIx32 "\n", i, c.locations[i]);
	}
	KYTY_LOG_DEBUG("\t msaa_num_samples      = %" PRIu8 "\n", cf.msaa_num_samples);
	KYTY_LOG_DEBUG("\t aa_mask_centroid_dtmn = %s\n", cf.aa_mask_centroid_dtmn ? "true" : "false");
	KYTY_LOG_DEBUG("\t max_sample_dist       = %" PRIu8 "\n", cf.max_sample_dist);
	KYTY_LOG_DEBUG("\t msaa_exposed_samples  = %" PRIu8 "\n", cf.msaa_exposed_samples);
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
		KYTY_LOG_DEBUG(
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
	if (msaa_num_samples != attachment_samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: msaa_num_samples != attachment_samples condition ignored (continuing)\n"); }
	if (cf.msaa_exposed_samples != 0 && cf.msaa_exposed_samples != effective_guest_samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cf.msaa_exposed_samples != 0 && cf.msaa_exposed_samples != effective_guest_samples condition ignored (continuing)\n"); }
	if (cf.aa_mask_centroid_dtmn) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cf.aa_mask_centroid_dtmn condition ignored (continuing)\n"); }

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
			KYTY_LOG_DEBUG(
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
		KYTY_LOG_WARN("KYTY_GRAPHICS: sample locations cannot be represented: %s\n",
		             VulkanSampleLocationStatusName(sample_location_status));
		if (sample_location_status != VulkanSampleLocationStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sample_location_status != VulkanSampleLocationStatus::Success condition ignored (continuing)\n"); }
	}
}

static void vp_print(const char* func, const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t msaa_enable                    = %s\n", smc.msaa_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t vport_scissor_enable           = %s\n", smc.vport_scissor_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t line_stipple_enable            = %s\n", smc.line_stipple_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t vp[0].zmin                     = %f\n", vp.viewports[0].zmin);
	KYTY_LOG_DEBUG("\t vp[0].zmax                     = %f\n", vp.viewports[0].zmax);
	KYTY_LOG_DEBUG("\t vp[0].xscale                   = %f\n", vp.viewports[0].xscale);
	KYTY_LOG_DEBUG("\t vp[0].xoffset                  = %f\n", vp.viewports[0].xoffset);
	KYTY_LOG_DEBUG("\t vp[0].yscale                   = %f\n", vp.viewports[0].yscale);
	KYTY_LOG_DEBUG("\t vp[0].yoffset                  = %f\n", vp.viewports[0].yoffset);
	KYTY_LOG_DEBUG("\t vp[0].zscale                   = %f\n", vp.viewports[0].zscale);
	KYTY_LOG_DEBUG("\t vp[0].zoffset                  = %f\n", vp.viewports[0].zoffset);
	KYTY_LOG_DEBUG("\t vp[0].viewport_scissor_left    = %d\n", vp.viewports[0].viewport_scissor_left);
	KYTY_LOG_DEBUG("\t vp[0].viewport_scissor_top     = %d\n", vp.viewports[0].viewport_scissor_top);
	KYTY_LOG_DEBUG("\t vp[0].viewport_scissor_right   = %d\n", vp.viewports[0].viewport_scissor_right);
	KYTY_LOG_DEBUG("\t vp[0].viewport_scissor_bottom  = %d\n", vp.viewports[0].viewport_scissor_bottom);
	KYTY_LOG_DEBUG("\t transform_control              = 0x%08" PRIx32 "\n", vp.transform_control);
	KYTY_LOG_DEBUG("\t screen_scissor_left            = %d\n", vp.screen_scissor_left);
	KYTY_LOG_DEBUG("\t screen_scissor_top             = %d\n", vp.screen_scissor_top);
	KYTY_LOG_DEBUG("\t screen_scissor_right           = %d\n", vp.screen_scissor_right);
	KYTY_LOG_DEBUG("\t screen_scissor_bottom          = %d\n", vp.screen_scissor_bottom);
	KYTY_LOG_DEBUG("\t generic_scissor_left           = %d\n", vp.generic_scissor_left);
	KYTY_LOG_DEBUG("\t generic_scissor_top            = %d\n", vp.generic_scissor_top);
	KYTY_LOG_DEBUG("\t generic_scissor_right          = %d\n", vp.generic_scissor_right);
	KYTY_LOG_DEBUG("\t generic_scissor_bottom         = %d\n", vp.generic_scissor_bottom);
	KYTY_LOG_DEBUG("\t hw_offset_x                    = %u\n", vp.hw_offset_x);
	KYTY_LOG_DEBUG("\t hw_offset_y                    = %u\n", vp.hw_offset_y);
	KYTY_LOG_DEBUG("\t guard_band_horz_clip           = %f\n", vp.guard_band_horz_clip);
	KYTY_LOG_DEBUG("\t guard_band_vert_clip           = %f\n", vp.guard_band_vert_clip);
	KYTY_LOG_DEBUG("\t guard_band_horz_discard        = %f\n", vp.guard_band_horz_discard);
	KYTY_LOG_DEBUG("\t guard_band_vert_discard        = %f\n", vp.guard_band_vert_discard);
	KYTY_LOG_DEBUG("\t generic_scissor_window_offset_enable               = %s\n", vp.generic_scissor_window_offset_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t viewports[0].viewport_scissor_window_offset_enable = %s\n",
	       vp.viewports[0].viewport_scissor_window_offset_enable ? "true" : "false");
}

static void vp_check(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	bool ps5 = Config::IsNextGen();

	// This control also enables coverage processing for single-sample render
	// targets. Attachment sample counts, rather than this flag alone, select
	// Vulkan multisampling.
	// EXIT_NOT_IMPLEMENTED(smc.vport_scissor_enable);
	if (smc.line_stipple_enable) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: smc.line_stipple_enable condition ignored (continuing)\n"); }

	// zmin/zmax are the hardware depth clamp. ResolveViewportDepth folds them
	// into the Vulkan viewport depth range, so any window is translatable.
	if (!std::isfinite(vp.viewports[0].zmin) || !std::isfinite(vp.viewports[0].zmax)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !std::isfinite(vp.viewports[0].zmin) || !std::isfinite(vp.viewports[0].zmax) condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	if (vp.transform_control != 1087) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vp.transform_control != 1087 condition ignored (continuing)\n"); }
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
	if (rc.depth_clear_enable && (!std::isfinite(depth_clear) || depth_clear < 0.0f || depth_clear > 1.0f)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: rc.depth_clear_enable && (!std::isfinite(depth_clear) || depth_clear < 0.0f || depth_clear > 1.0f) condition ignored (continuing)\n"); }
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
		KYTY_LOG_DEBUG("Context\n");
		KYTY_LOG_DEBUG("\t GetRenderTargetMask()   = 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
		KYTY_LOG_DEBUG("\t GetDepthClearValue()    = %f\n", hw.GetDepthClearValue());
		KYTY_LOG_DEBUG("\t GetStencilClearValue()  = %" PRIu8 "\n", hw.GetStencilClearValue());
		KYTY_LOG_DEBUG("\t GetLineWidth()          = %f\n", hw.GetLineWidth());

		KYTY_LOG_DEBUG("%s", rt_print("RenderTraget:", rt).Concat(U"").C_Str());

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
