#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONDEMOTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONDEMOTION_H_

#include <cstdint>
#include <mutex>

namespace Kyty::Libs::Graphics {

// Guest ranges whose render targets returned to the guest extent after a use
// that addresses guest texels. A demoted range never scales again, so a target
// is demoted at most once. The set is bounded; once full, every range counts as
// demoted and new targets stay native.
class RenderResolutionDemotionSet
{
public:
	static constexpr uint32_t RANGES_MAX = 256;

	void               Demote(uint64_t address, uint64_t size);
	[[nodiscard]] bool Demoted(uint64_t address, uint64_t size) const;

private:
	struct Range
	{
		uint64_t begin = 0;
		uint64_t end   = 0;
	};

	mutable std::mutex m_mutex;
	Range              m_ranges[RANGES_MAX] {};
	uint32_t           m_count     = 0;
	bool               m_saturated = false;
};

void               RenderResolutionDemoteRange(uint64_t address, uint64_t size);
[[nodiscard]] bool RenderResolutionRangeDemoted(uint64_t address, uint64_t size);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONDEMOTION_H_ */
