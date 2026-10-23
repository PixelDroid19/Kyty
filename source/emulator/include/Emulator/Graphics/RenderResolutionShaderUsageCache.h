#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONSHADERUSAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONSHADERUSAGECACHE_H_

#include "Emulator/Graphics/RenderResolutionPlanner.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Kyty::Libs::Graphics {

class ShaderNativeWaveVerdict;
struct ShaderImmutablePixelAnalysis;
struct ShaderScalarFlow;
class ShaderCodeResourceMemo;

struct RenderResolutionShaderUsageKey
{
	uint64_t address                = 0;
	uint64_t checksum               = 0;
	uint32_t translator_version     = 0;
	uint32_t pixel_extra_lds_dwords = 0;
	int      pixel_parameter_sgpr   = -1;

	[[nodiscard]] bool operator==(const RenderResolutionShaderUsageKey& other) const;
};

struct RenderResolutionShaderAnalysis
{
	RenderShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
	// Per-program native-wave verdict, shared by every draw of this program.
	std::shared_ptr<const ShaderNativeWaveVerdict> native_wave;
	std::shared_ptr<const ShaderImmutablePixelAnalysis> pixel_analysis;
	// Immutable SGPR provenance for the cached code; descriptor values stay per draw.
	std::shared_ptr<const ShaderScalarFlow> scalar_flow;
	// Code-only resource analyses of this program, shared by its draws.
	std::shared_ptr<const ShaderCodeResourceMemo> resource_memo;
};

struct RenderResolutionShaderUsageResult
{
	RenderShaderCoordinateUsage usage;
	std::shared_ptr<const ShaderCode> code;
	bool                            hit     = false;
	bool                            evicted = false;
	std::shared_ptr<const ShaderNativeWaveVerdict> native_wave;
	std::shared_ptr<const ShaderImmutablePixelAnalysis> pixel_analysis;
	std::shared_ptr<const ShaderScalarFlow> scalar_flow;
	std::shared_ptr<const ShaderCodeResourceMemo> resource_memo;
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
