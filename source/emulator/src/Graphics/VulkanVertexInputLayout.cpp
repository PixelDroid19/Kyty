#include "Emulator/Graphics/VulkanVertexInputLayout.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/VulkanVertexInputFormat.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool VulkanBuildVertexInputLayout(const ShaderVertexInputInfo& input, VulkanVertexInputLayout* layout)
{
	if (layout == nullptr || input.resources_num < 0 || input.resources_num > static_cast<int>(VulkanVertexInputLayout::MAX_ATTRIBUTES) ||
	    input.buffers_num < 0 || input.buffers_num > static_cast<int>(VulkanVertexInputLayout::MAX_ATTRIBUTES))
	{
		return false;
	}

	*layout = {};
	bool resource_seen[VulkanVertexInputLayout::MAX_ATTRIBUTES] = {};
	const bool gen5 = Config::IsNextGen();

	for (int buffer_index = 0; buffer_index < input.buffers_num; buffer_index++)
	{
		const auto& buffer = input.buffers[buffer_index];
		if (buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX)
		{
			return false;
		}

		auto& binding       = layout->bindings[buffer_index];
		binding.binding     = static_cast<uint32_t>(buffer_index);
		binding.stride      = buffer.stride;
		binding.inputRate   = VK_VERTEX_INPUT_RATE_VERTEX;

		for (int attribute_index = 0; attribute_index < buffer.attr_num; attribute_index++)
		{
			const int resource_index = buffer.attr_indices[attribute_index];
			if (resource_index < 0 || resource_index >= input.resources_num || resource_seen[resource_index])
			{
				return false;
			}

			const auto& resource   = input.resources[resource_index];
			const auto& destination = input.resources_dst[resource_index];
			const auto format = gen5 ? VulkanResolveGen5VertexInputFormat(resource.Format())
			                         : VulkanResolveLegacyVertexInputFormat(resource.Dfmt(), resource.Nfmt());
			if (format.format == VK_FORMAT_UNDEFINED || format.component_count == 0 || resource.AddTid() || resource.SwizzleEnabled())
			{
				return false;
			}

			switch (destination.registers_num)
			{
				case 1:
					if (resource.DstSelX() != DstSel(4))
					{
						return false;
					}
					break;
				case 2:
					// Identity XY (4,5) on a 1-component format selects missing Y=0.
					if ((format.component_count == 1 && resource.DstSelXY() != DstSel(4, 0) &&
					     resource.DstSelXY() != DstSel(4, 5)) ||
					    (format.component_count >= 2 && resource.DstSelXY() != DstSel(4, 5)))
					{
						return false;
					}
					break;
				case 3:
					// Identity XYZ (4,5,6) on a 1/2-component format selects
					// missing Z=0, same as the explicit 0 constant.
					if ((format.component_count == 1 && resource.DstSelXYZ() != DstSel(4, 0, 0) &&
					     resource.DstSelXYZ() != DstSel(4, 5, 6)) ||
					    (format.component_count == 2 && resource.DstSelXYZ() != DstSel(4, 5, 0) &&
					     resource.DstSelXYZ() != DstSel(4, 5, 6)) ||
					    (format.component_count >= 3 && resource.DstSelXYZ() != DstSel(4, 5, 6)))
					{
						return false;
					}
					break;
				case 4:
					// Identity DST_SEL 0xFAC=(4,5,6,7). Hardware fills missing
					// channels (0 for Y/Z, 1 for W on float), so identity is
					// the same as the explicit constant forms.
					if ((format.component_count == 1 && resource.DstSelXYZW() != DstSel(4, 0, 0, 1) &&
					     resource.DstSelXYZW() != DstSel(4, 5, 6, 7)) ||
					    (format.component_count == 2 && resource.DstSelXYZW() != DstSel(4, 5, 0, 1) &&
					     resource.DstSelXYZW() != DstSel(4, 5, 6, 7)) ||
					    (format.component_count == 3 && resource.DstSelXYZW() != DstSel(4, 5, 6, 1) &&
					     resource.DstSelXYZW() != DstSel(4, 5, 6, 7)) ||
					    (format.component_count == 4 && resource.DstSelXYZW() != DstSel(4, 5, 6, 7)))
					{
						return false;
					}
					break;
				default: return false;
			}

			auto& attribute      = layout->attributes[resource_index];
			attribute.binding    = static_cast<uint32_t>(buffer_index);
			attribute.location   = static_cast<uint32_t>(resource_index);
			attribute.offset     = buffer.attr_offsets[attribute_index];
			attribute.format     = format.format;
			resource_seen[resource_index] = true;
		}
	}

	for (int resource_index = 0; resource_index < input.resources_num; resource_index++)
	{
		if (!resource_seen[resource_index])
		{
			return false;
		}
	}

	layout->binding_count   = static_cast<uint32_t>(input.buffers_num);
	layout->attribute_count = static_cast<uint32_t>(input.resources_num);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
