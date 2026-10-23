#ifndef EMULATOR_INCLUDE_EMULATOR_CONFIG_H_
#define EMULATOR_INCLUDE_EMULATOR_CONFIG_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/GuestPlatform.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Config {

class ConfigSource;

class ConfigSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<ConfigSubsystem>::Instance(); }
	const char*       Id() override { return "Config"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


enum class ShaderOptimizationType
{
	None,
	Size,
	Performance
};

enum class ShaderLogDirection
{
	Silent,
	Console,
	File
};

enum class ProfilerDirection
{
	None,
	File,
	Network,
	FileAndNetwork
};

enum class RenderResolutionMode
{
	Native,
	Fixed
};

// Which guest render targets a Fixed render resolution may scale. Display
// covers the display buffers and their depth; RenderTargets also covers
// offscreen color targets at the guest display extent.
enum class RenderResolutionScope
{
	Display,
	RenderTargets
};

enum class PresentationFilter
{
	Linear,
	Nearest,
	Fsr
};

// Host texture filtering override. Guest keeps each sampler's own anisotropy;
// a level raises linear-minified samplers to at least that ratio.
enum class TextureAnisotropy
{
	Guest,
	X2,
	X4,
	X8,
	X16
};

void Load(const ConfigSource& cfg);

// The primary guest executable establishes this once per runtime session.
// A different platform is rejected so PS4 cannot remain an implicit default.
[[nodiscard]] bool  SetGuestPlatform(GuestPlatform platform);
[[nodiscard]] GuestPlatform GetGuestPlatform();
void                 ResetGuestPlatform();

// Compatibility adapter for existing tests and callers. Production loading
// must use SetGuestPlatform so a session cannot be silently reclassified.
void SetNextGen(bool mode);

bool     IsInitialized();
uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
RenderResolutionMode GetRenderResolutionMode();
RenderResolutionScope GetRenderResolutionScope();
uint32_t GetRenderResolutionWidth();
uint32_t GetRenderResolutionHeight();
PresentationFilter GetPresentationFilter();
uint32_t GetPresentationFsrSharpness();
uint32_t GetPresentFpsLimit();
TextureAnisotropy GetTextureAnisotropy();
bool     IsNeo();
bool     IsNextGen();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
String                 GetShaderLogFolder();

bool   CommandBufferDumpEnabled();
String GetCommandBufferDumpFolder();

Log::Direction GetPrintfDirection();
Log::Level     GetPrintfLevel();
String         GetPrintfOutputFile();
String         GetPrintfOutputFolder();

ProfilerDirection GetProfilerDirection();
String            GetProfilerOutputFile();

bool SpirvDebugPrintfEnabled();

bool   PipelineDumpEnabled();
String GetPipelineDumpFolder();

} // namespace Kyty::Config

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_CONFIG_H_ */
