#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONSHADERUSAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONSHADERUSAGECACHE_H_

#include "Emulator/Graphics/RenderResolutionPlanner.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Kyty::Libs::Graphics {

struct RenderResolutionShaderUsageKey
{
	uint64_t address            = 0;
	uint64_t checksum           = 0;
	uint32_t translator_version = 0;

	[[nodiscard]] bool operator==(const RenderResolutionShaderUsageKey& other) const;
};

struct RenderResolutionShaderAnalysis
{
	RenderShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
};

struct RenderResolutionShaderUsageResult
{
	RenderShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
	bool                            hit     = false;
	bool                            evicted = false;
};

class RenderResolutionShaderUsageCache final
{
public:
	using Analyzer = std::function<RenderResolutionShaderAnalysis()>;

	explicit RenderResolutionShaderUsageCache(size_t max_entries);
	~RenderResolutionShaderUsageCache();
	RenderResolutionShaderUsageCache(const RenderResolutionShaderUsageCache&)            = delete;
	RenderResolutionShaderUsageCache& operator=(const RenderResolutionShaderUsageCache&) = delete;

	[[nodiscard]] RenderResolutionShaderUsageResult GetOrAnalyze(const RenderResolutionShaderUsageKey& key, const Analyzer& analyzer);

private:
	struct State;
	State* m_state = nullptr;
};

} // namespace Kyty::Libs::Graphics

#endif
