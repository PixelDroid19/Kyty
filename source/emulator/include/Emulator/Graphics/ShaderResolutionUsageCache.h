#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERRESOLUTIONUSAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERRESOLUTIONUSAGECACHE_H_

#include "Emulator/Graphics/AttachmentResolutionCohort.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Kyty::Libs::Graphics {

struct ShaderResolutionUsageKey
{
	uint64_t address            = 0;
	uint64_t checksum           = 0;
	uint32_t translator_version = 0;

	[[nodiscard]] bool operator==(const ShaderResolutionUsageKey& other) const;
};

struct ShaderResolutionAnalysis
{
	ResolutionShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
};

struct ShaderResolutionUsageResult
{
	ResolutionShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
	bool                            hit     = false;
	bool                            evicted = false;
};

class ShaderResolutionUsageCache final
{
public:
	using Analyzer = std::function<ShaderResolutionAnalysis()>;

	explicit ShaderResolutionUsageCache(size_t max_entries);
	~ShaderResolutionUsageCache();
	ShaderResolutionUsageCache(const ShaderResolutionUsageCache&)            = delete;
	ShaderResolutionUsageCache& operator=(const ShaderResolutionUsageCache&) = delete;

	[[nodiscard]] ShaderResolutionUsageResult GetOrAnalyze(const ShaderResolutionUsageKey& key, const Analyzer& analyzer);

private:
	struct State;
	State* m_state = nullptr;
};

} // namespace Kyty::Libs::Graphics

#endif
