#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/VulkanVertexInputFormat.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include "ShaderStorageAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	if (code[0] == 0xBEEB03FF)
	{
		return reinterpret_cast<const ShaderBinaryInfo*>(code + static_cast<size_t>(code[1] + 1) * 2);
	}

	return nullptr;
}

ShaderUsageInfo GetUsageSlots(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	const auto* binary_info = GetBinaryInfo(code);

	ShaderUsageInfo ret;

	if (binary_info != nullptr)
	{
		if (binary_info->chunk_usage_base_offset_dw == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: binary_info->chunk_usage_base_offset_dw == 0 condition ignored (continuing)\n"); }

		ret.usage_masks = (binary_info->chunk_usage_base_offset_dw == 0
		                       ? nullptr
		                       : reinterpret_cast<const uint32_t*>(binary_info) - binary_info->chunk_usage_base_offset_dw);
		ret.slots_num   = binary_info->num_input_usage_slots;
		ret.slots       = (ret.slots_num == 0 ? nullptr : reinterpret_cast<const ShaderUsageSlot*>(ret.usage_masks) - ret.slots_num);
		ret.valid       = true;
	}

	return ret;
}

void ShaderDetectBuffers(ShaderVertexInputInfo* info, bool ps5)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr);

	info->buffers_num = 0;

	for (int ri = 0; ri < info->resources_num; ri++)
	{
		const auto& r = info->resources[ri];
		// Empty V# descriptors occur in Gen5 metadata for attributes that are
		// inactive in the current draw. They have no backing range and cannot be
		// represented as a Vulkan vertex-buffer binding.
		if (r.NumRecords() == 0)
		{
			continue;
		}

		bool merged = false;
		for (int bi = 0; bi < info->buffers_num; bi++)
		{
			auto& b = info->buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride())
			{
				uint64_t rbase   = (ps5 ? r.Base48() : r.Base44());
				uint64_t base    = std::min(rbase, b.addr);
				uint64_t offset1 = rbase - base;
				uint64_t offset2 = b.addr - base;

				if (offset1 < stride && offset2 < stride)
				{
					if (b.num_records != r.NumRecords()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: b.num_records != r.NumRecords() condition ignored (continuing)\n"); }
					b.addr = base;
					if (b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX condition ignored (continuing)\n"); }
					b.attr_indices[b.attr_num++] = ri;
					merged                       = true;
					break;
				}
			}
		}

		if (!merged)
		{
			if (info->buffers_num >= ShaderVertexInputInfo::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->buffers_num >= ShaderVertexInputInfo::RES_MAX condition ignored (continuing)\n"); }
			int bi                            = info->buffers_num++;
			info->buffers[bi].addr            = (ps5 ? r.Base48() : r.Base44());
			info->buffers[bi].stride          = r.Stride();
			info->buffers[bi].num_records     = r.NumRecords();
			info->buffers[bi].attr_num        = 1;
			info->buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info->buffers_num; bi++)
	{
		auto& b = info->buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++)
		{
			b.attr_offsets[ri] =
			    (ps5 ? info->resources[b.attr_indices[ri]].Base48() : info->resources[b.attr_indices[ri]].Base44()) - b.addr;
		}
	}
}

void ShaderParseFetch(ShaderVertexInputInfo* info, const uint32_t* fetch, const uint32_t* buffer, uint32_t user_sgpr_num)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || fetch == nullptr || buffer == nullptr);

	KYTY_PROFILER_BLOCK("ShaderParseFetch::parse_code");

	ShaderCode code;
	code.SetType(ShaderType::Fetch);
	// shader_parse(0, fetch, nullptr, &code);
	{
		DebugStatsScopedTimer timer(RecordShaderInputAnalysis);
		ShaderParse(fetch, &code);
	}
	info->vertex_offset_sgpr = ShaderDetectVertexOffsetSgpr(code, 0, user_sgpr_num);

	KYTY_PROFILER_END_BLOCK;

	// KYTY_LOG_DEBUG("%s", code.DbgDump().c_str());

	KYTY_PROFILER_BLOCK("ShaderParseFetch::check_insts");

	const auto& insts = code.GetInstructions();
	uint32_t    size  = insts.Size();
	// int         temp_register = 0;
	uint32_t temp_value[104] = {0};
	int      s_num           = 0;
	int      v_num           = 0;

	for (uint32_t i = 0; i < size; i++)
	{
		const auto& inst = insts.At(i);

		if (inst.type == ShaderInstructionType::SLoadDwordx4)
		{
			if (inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0 condition ignored (continuing)\n"); }
			if (inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != 2 condition ignored (continuing)\n"); }
			if (inst.dst.type != ShaderOperandType::Sgpr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.type != ShaderOperandType::Sgpr condition ignored (continuing)\n"); }

			uint32_t index    = inst.src[1].constant.u >> 2u;
			int      t        = inst.dst.register_id;
			temp_value[t + 0] = buffer[index + 0];
			temp_value[t + 1] = buffer[index + 1];
			temp_value[t + 2] = buffer[index + 2];
			temp_value[t + 3] = buffer[index + 3];

			s_num++;
		}

		bool load_inst     = true;
		int  registers_num = 0;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadFormatX: registers_num = 1; break;
			case ShaderInstructionType::BufferLoadFormatXy: registers_num = 2; break;
			case ShaderInstructionType::BufferLoadFormatXyz: registers_num = 3; break;
			case ShaderInstructionType::BufferLoadFormatXyzw: registers_num = 4; break;
			default: load_inst = false;
		}

		if (load_inst && registers_num > 0)
		{
			// EXIT_NOT_IMPLEMENTED(!(i >= 2 && insts.At(i - 1).type == ShaderInstructionType::SWaitcnt &&
			//                       insts.At(i - 2).type == ShaderInstructionType::SLoadDwordx4));
			if (inst.dst.type != ShaderOperandType::Vgpr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.type != ShaderOperandType::Vgpr condition ignored (continuing)\n"); }
			if (inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0 condition ignored (continuing)\n"); }
			if (inst.src[1].type != ShaderOperandType::Sgpr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[1].type != ShaderOperandType::Sgpr condition ignored (continuing)\n"); }
			if (inst.src[2].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].constant.i != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[2].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].constant.i != 0 condition ignored (continuing)\n"); }

			if (info->resources_num >= ShaderVertexInputInfo::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->resources_num >= ShaderVertexInputInfo::RES_MAX condition ignored (continuing)\n"); }

			int t = inst.src[1].register_id;

			auto& r           = info->resources[info->resources_num];
			auto& rd          = info->resources_dst[info->resources_num];
			rd.register_start = inst.dst.register_id;
			rd.registers_num  = registers_num;
			rd.semantic       = info->resources_num;
			r.fields[0]       = temp_value[t + 0];
			r.fields[1]       = temp_value[t + 1];
			r.fields[2]       = temp_value[t + 2];
			r.fields[3]       = temp_value[t + 3];

			info->resources_num++;

			v_num++;
		}
	}

	KYTY_PROFILER_END_BLOCK;

	if (s_num != v_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_num != v_num condition ignored (continuing)\n"); }
}

void ShaderParseAttrib(ShaderVertexInputInfo* info, const ShaderSemantic* input_semantics, uint32_t num_input_semantics,
                              const uint32_t* attrib, const uint32_t* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || attrib == nullptr || buffer == nullptr);

	info->fetch_attrib_data_num = 0;

	uint32_t max_semantic = 0;
	for (uint32_t i = 0; i < num_input_semantics; i++)
	{
		if (input_semantics[i].semantic + 1u > max_semantic)
		{
			max_semantic = input_semantics[i].semantic + 1u;
		}
	}
	if (max_semantic > static_cast<uint32_t>(ShaderVertexInputInfo::RES_MAX)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: max_semantic > static_cast<uint32_t>(ShaderVertexInputInfo::RES_MAX) condition ignored (continuing)\n"); }
	for (uint32_t i = 0; i < max_semantic; i++)
	{
		info->fetch_attrib_data[i] = attrib[i];
	}
	info->fetch_attrib_data_num = static_cast<int>(max_semantic);
	static const bool vertex_attr_trace = std::getenv("KYTY_VERTEX_ATTR_TRACE") != nullptr;

	for (uint32_t i = 0; i < num_input_semantics; i++)
	{
		const auto& in = input_semantics[i];

		if (in.static_vb_index == 1 || in.static_attribute == 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: in.static_vb_index == 1 || in.static_attribute == 1 condition ignored (continuing)\n"); }

		uint32_t reg  = in.hardware_mapping;
		uint32_t size = in.size_in_elements;

		if (vertex_attr_trace)
		{
			KYTY_LOG_DEBUG("reg = %u, size = %u, va[%u] = 0x%08" PRIx32 "\n", reg, size, i, attrib[in.semantic]);
		}

		size_t   index       = attrib[in.semantic] & 0x1fu;
		uint32_t format      = (attrib[in.semantic] >> 5u) & 0x1ffu;
		uint32_t offset      = (attrib[in.semantic] >> 14u) & 0xfffu;
		uint32_t fetch_index = (attrib[in.semantic] >> 26u) & 0x1u;

		if (fetch_index != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fetch_index != 0 condition ignored (continuing)\n"); }

		if (index >= ShaderVertexInputInfo::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: index >= ShaderVertexInputInfo::RES_MAX condition ignored (continuing)\n"); }

		const auto* sharp = &buffer[index * 4];
		if (vertex_attr_trace)
		{
			static std::atomic_uint32_t vertex_attr_logs {0};
			const auto                  vertex_attr_log = vertex_attr_logs.fetch_add(1, std::memory_order_relaxed);
			if (vertex_attr_log < 32u)
			{
				KYTY_LOG_DEBUG(
				             "KYTY_VERTEX_ATTR semantic=%u raw=0x%08" PRIx32 " index=%zu format=0x%03" PRIx32 " offset=0x%03" PRIx32
				             " attrib=%p buffer=%p sharp=%p words=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
				             in.semantic, attrib[in.semantic], index, format, offset, static_cast<const void*>(attrib),
				             static_cast<const void*>(buffer), static_cast<const void*>(sharp), sharp[0], sharp[1], sharp[2], sharp[3]);
			}
		}

		if (info->resources_num >= ShaderVertexInputInfo::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->resources_num >= ShaderVertexInputInfo::RES_MAX condition ignored (continuing)\n"); }

		auto& r           = info->resources[info->resources_num];
		auto& rd          = info->resources_dst[info->resources_num];
		rd.register_start = static_cast<int>(reg);
		rd.semantic       = static_cast<int>(in.semantic);
		r.fields[0]       = sharp[0];
		r.fields[1]       = sharp[1];
		r.fields[2]       = sharp[2];
		r.fields[3]       = sharp[3];
		if (format != 0)
		{
			const auto     input_format    = VulkanResolveGen5VertexAttribInputFormat(static_cast<uint16_t>(format));
			const uint32_t component_count = input_format.component_count;
			if (input_format.format == VK_FORMAT_UNDEFINED || component_count == 0)
			{
				// Keep the V# format/DST_SEL. Overwriting with unified 0 makes
				// VulkanBuildVertexInputLayout reject a live RGB32F stream.
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: unknown vertex attrib format 0x%x; keeping buffer format\n", format);
			} else
			{
				if (size == 0 || size > 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size == 0 || size > 4 condition ignored (continuing)\n"); }
				uint32_t swizzle = DstSel(4, 0, 0, 1);
				switch (component_count)
				{
					case 2: swizzle = DstSel(4, 5, 0, 1); break;
					case 3: swizzle = DstSel(4, 5, 6, 1); break;
					case 4: swizzle = DstSel(4, 5, 6, 7); break;
					default: break;
				}
				r.fields[3] = (r.fields[3] & ~((0x7fu << 12u) | 0xfffu)) |
				              (static_cast<uint32_t>(input_format.unified_format) << 12u) | swizzle;
			}
			// The semantic controls the shader input width; the backing format
			// controls storage and descriptor swizzles. Either may have more
			// components: shaders can ignore stored components or consume the
			// descriptor's default channels.
			rd.registers_num = static_cast<int>(size);
		} else
		{
			rd.registers_num = static_cast<int>(size);
		}
		if (offset != 0)
		{
			const uint64_t base = r.Base48();
			if (base > UINT64_MAX - offset) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base > UINT64_MAX - offset condition ignored (continuing)\n"); }
			r.UpdateAddress48(base + offset);
		}

		info->resources_num++;
	}
}

bool ShaderGetStorageBuffer(ShaderStorageResources* info, bool* direct_sgprs, int start_index, int slot, ShaderStorageUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer,
                                   ShaderStorageBindingSource source)
{
	EXIT_IF(info == nullptr);

	if (info->buffers_num < 0 || info->buffers_num >= ShaderStorageResources::BUFFERS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->buffers_num < 0 || info->buffers_num >= ShaderStorageResources::BUFFERS_MAX condition ignored (continuing)\n"); }

	int  index    = info->buffers_num;
	bool extended = (extended_buffer != nullptr);

	// With Gen5 32-user-SGPR windows, slots 16..31 are direct user SGPRs (not
	// necessarily a separate EUD pointer). Only require extended when an
	// extended_buffer is supplied.
	if (extended)
	{
		if (start_index < 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 16 condition ignored (continuing)\n"); }
	} else
	{
		if (start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
	}

	ShaderBufferResource resource;
	resource.fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	resource.fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	resource.fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	resource.fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);

	// Fully zeroed sharp, or zero address+records with residual flag bits, is a
	// null buffer descriptor (Gen5 titles leave unused slots that way).
	if ((resource.fields[0] == 0 && resource.fields[1] == 0 && resource.fields[2] == 0 && resource.fields[3] == 0) ||
	    (resource.Base48() == 0 && resource.NumRecords() == 0))
	{
		return false;
	}

	info->start_register[index] = start_index;
	info->slots[index]          = slot;
	info->usages[index]         = usage;
	info->sources[index]        = source;
	info->extended[index]       = extended;
	info->buffers[index]        = resource;
	// info->extended_index[index] = extended_index;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			// Region/Vsharp markers may be unset when SGPRs were bulk-written;
			// Unknown is accepted for Gen5 full-window loads (reg_num=30).
			if (type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown condition ignored (continuing)\n"); }

			direct_sgprs[start_index + j] = false;
		}
	}

	info->buffers_num++;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
