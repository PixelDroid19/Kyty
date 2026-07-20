#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERTRANSLATIONCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERTRANSLATIONCACHE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"

#include <cstddef>
#include <cstdint>
#include <functional>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kShaderTranslatorVersion = 2;

enum class ShaderModuleStage : uint8_t
{
	Vertex,
	Pixel,
	Compute
};

struct ShaderModuleKey
{
	ShaderId                       shader_id;
	ShaderModuleStage              stage              = ShaderModuleStage::Vertex;
	Config::ShaderOptimizationType optimization       = Config::ShaderOptimizationType::None;
	bool                           next_gen            = false;
	uint32_t                       translator_version = kShaderTranslatorVersion;

	[[nodiscard]] static ShaderModuleKey Create(const ShaderId& shader_id, ShaderModuleStage stage,
	                                            Config::ShaderOptimizationType optimization, bool next_gen);
	[[nodiscard]] bool operator==(const ShaderModuleKey& other) const;
	[[nodiscard]] bool operator!=(const ShaderModuleKey& other) const { return !(*this == other); }
};

struct ShaderTranslationCacheResult
{
	Vector<uint32_t> binary;
	bool             hit     = false;
	bool             evicted = false;
};

class ShaderTranslationCache final
{
public:
	using Compiler = std::function<Vector<uint32_t>()>;

	explicit ShaderTranslationCache(size_t max_entries);
	~ShaderTranslationCache();
	KYTY_CLASS_NO_COPY(ShaderTranslationCache);

	[[nodiscard]] ShaderTranslationCacheResult GetOrCompile(const ShaderModuleKey& key, const Compiler& compiler);
	[[nodiscard]] size_t                       Size() const;

private:
	struct State;
	State* m_state = nullptr;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERTRANSLATIONCACHE_H_ */
