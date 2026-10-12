#include "Emulator/Graphics/RenderResolutionDemotion.h"

#include <limits>

namespace Kyty::Libs::Graphics {
namespace {

uint64_t RangeEnd(uint64_t address, uint64_t size)
{
	return size > std::numeric_limits<uint64_t>::max() - address ? std::numeric_limits<uint64_t>::max() : address + size;
}

RenderResolutionDemotionSet& Runtime()
{
	static RenderResolutionDemotionSet runtime;
	return runtime;
}

} // namespace

void RenderResolutionDemotionSet::Demote(uint64_t address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	const uint64_t              end = RangeEnd(address, size);
	for (uint32_t index = 0; index < m_count; index++)
	{
		if (m_ranges[index].begin <= address && end <= m_ranges[index].end)
		{
			return;
		}
	}
	if (m_count == RANGES_MAX)
	{
		m_saturated = true;
		return;
	}
	m_ranges[m_count++] = {address, end};
}

bool RenderResolutionDemotionSet::Demoted(uint64_t address, uint64_t size) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_saturated)
	{
		return true;
	}
	const uint64_t end = RangeEnd(address, size);
	for (uint32_t index = 0; index < m_count; index++)
	{
		if (address < m_ranges[index].end && m_ranges[index].begin < end)
		{
			return true;
		}
	}
	return false;
}

void RenderResolutionDemoteRange(uint64_t address, uint64_t size)
{
	Runtime().Demote(address, size);
}

bool RenderResolutionRangeDemoted(uint64_t address, uint64_t size)
{
	return Runtime().Demoted(address, size);
}

} // namespace Kyty::Libs::Graphics
