#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Log.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool ShaderPixelInputMaskSupported(uint32_t enable_mask, uint32_t address_mask)
{
	constexpr uint32_t kPerspectiveCenter   = 1u << 1u;
	constexpr uint32_t kPerspectiveCentroid = 1u << 2u;
	constexpr uint32_t kLinearCenter        = 1u << 5u;
	constexpr uint32_t kLinearCentroid      = 1u << 6u;
	constexpr uint32_t kPositionXy        = (1u << 8u) | (1u << 9u);
	constexpr uint32_t kInterpolation      = kPerspectiveCenter | kPerspectiveCentroid | kLinearCenter | kLinearCentroid;
	constexpr uint32_t kSupported          = kInterpolation | kPositionXy;

	if (enable_mask != address_mask || (enable_mask & ~kSupported) != 0)
	{
		return false;
	}
	// An all-zero mask is a pixel shader that reads no barycentrics and no
	// position: it exports a constant, discards, or samples with shader-supplied
	// coordinates only. It has no interpolation instructions to translate, so
	// there is nothing to reject.
	if (enable_mask == 0)
	{
		return true;
	}
	const uint32_t interpolation = enable_mask & kInterpolation;
	const uint32_t position      = enable_mask & kPositionXy;
	const uint32_t perspective   = enable_mask & (kPerspectiveCenter | kPerspectiveCentroid);
	const uint32_t linear        = enable_mask & (kLinearCenter | kLinearCentroid);
	if (perspective != 0 && linear != 0)
	{
		return false;
	}
	return interpolation != 0 && (position == 0 || position == kPositionXy);
}

bool ShaderPixelPositionEnabled(uint32_t enable_mask, uint32_t address_mask)
{
	constexpr uint32_t kPositionXy = (1u << 8u) | (1u << 9u);
	return (enable_mask & kPositionXy) == kPositionXy && (address_mask & kPositionXy) == kPositionXy;
}

uint32_t ShaderResolvePixelInterpolatorSetting(uint32_t stored_setting, uint32_t written_mask, uint32_t index)
{
	EXIT_IF(index >= 32u);
	return ((written_mask & (1u << index)) != 0 ? stored_setting : index);
}

uint32_t ShaderPixelCanonicalInterpolator(const ShaderPixelInputInfo& info, uint32_t index)
{
	EXIT_IF(index >= info.input_num);
	const uint32_t setting = info.interpolator_settings[index];
	for (uint32_t i = 0; i < index; ++i)
	{
		if (info.interpolator_settings[i] == setting)
		{
			return i;
		}
	}
	return index;
}

bool ShaderDecodePixelInterpolator(uint32_t setting, ShaderPixelInterpolator* interpolator)
{
	constexpr uint32_t kOffsetMask       = 0x3fu;
	constexpr uint32_t kDefaultOffset    = 0x20u;
	constexpr uint32_t kDefaultValueMask = 0x300u;
	constexpr uint32_t kFlatMask         = 0x400u;
	constexpr uint32_t kKnownMask        = kOffsetMask | kDefaultValueMask | kFlatMask;

	EXIT_IF(interpolator == nullptr);
	if ((setting & ~kKnownMask) != 0)
	{
		return false;
	}

	const uint32_t offset = setting & kOffsetMask;
	if (offset < kDefaultOffset)
	{
		if ((setting & kDefaultValueMask) != 0)
		{
			return false;
		}
		interpolator->source        = ShaderPixelInterpolatorSource::Parameter;
		interpolator->location      = offset;
		interpolator->flat          = (setting & kFlatMask) != 0;
		interpolator->default_value = 0;
		return true;
	}

	// OFFSET=0x20 selects a fixed-function default vector. The flat form is
	// ambiguous with a parameter-cache mode and requires producer metadata.
	if (offset != kDefaultOffset || (setting & kFlatMask) != 0)
	{
		return false;
	}

	interpolator->source        = ShaderPixelInterpolatorSource::Default;
	interpolator->location      = 0;
	interpolator->flat          = false;
	interpolator->default_value = (setting & kDefaultValueMask) >> 8u;
	return true;
}

float ShaderPixelInterpolatorDefaultComponent(const ShaderPixelInterpolator& interpolator, uint32_t component)
{
	EXIT_IF(interpolator.source != ShaderPixelInterpolatorSource::Default);
	EXIT_IF(component >= 4u);

	const bool one = (component == 3u ? (interpolator.default_value & 0x1u) != 0 : (interpolator.default_value & 0x2u) != 0);
	return (one ? 1.0f : 0.0f);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
