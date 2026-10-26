#include "Kyty/UnitTest.h"

#include "emulator/ScopedTestDirectory.h"

#include "../../integration_test/src/ArchitectureBoundaryCheck.h"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

UT_BEGIN(ArchitectureBoundary);

using Kyty::ArchitectureBoundary::CheckSourceRoot;

namespace {

using Diagnostics = std::vector<std::string>;
using Files       = std::vector<std::pair<std::string, std::string>>;

void WriteFixture(const std::filesystem::path& root, const std::string& relative_path, const std::string& contents)
{
	const auto path = root / relative_path;
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << contents;
}

Diagnostics Check(const Files& files, bool strict = false)
{
	ScopedTestDirectory directory("kyty-boundary");
	EXPECT_FALSE(directory.Path().empty());
	for (const auto& [path, contents]: files)
	{
		WriteFixture(directory.Path(), path, contents);
	}
	const auto result = CheckSourceRoot(directory.Path(), strict);
	EXPECT_EQ(result.exit_code, result.diagnostics.empty() ? 0 : 1);
	return result.diagnostics;
}

struct SingleIncludeCase
{
	const char* source;
	const char* include;
	const char* rule;
};

} // namespace

TEST(ArchitectureBoundary, AllowsUnrelatedIncludes)
{
	EXPECT_EQ(Check({{"emulator/src/AudioHost.cpp", "#include \"Emulator/Kernel/Time.h\"\n"},
	                 {"emulator/src/Loader/RuntimeLinker.cpp", "#include \"Emulator/Loader/Elf.h\"\n"}}),
	          Diagnostics {});
}

TEST(ArchitectureBoundary, RejectsEachForbiddenDirection)
{
	const SingleIncludeCase cases[] = {
	    {"emulator/src/AudioHost.cpp", "Emulator/Graphics/Graphics.h", "Audio -> Graphics"},
	    {"emulator/src/AudioNgs2.cpp", "Emulator/Graphics/Graphics.h", "Audio -> Graphics"},
	    {"emulator/src/AudioAvPlayer.cpp", "Emulator/Loader/GuestCall.h", "Audio -> Loader"},
	    {"emulator/src/Kernel/FileSystem.cpp", "Emulator/Graphics/Objects/GpuMemory.h", "Kernel -> Graphics"},
	    {"emulator/src/Kernel/EventFlag.cpp", "Emulator/Libs/Libs.h", "Kernel -> Libs"},
	    {"emulator/src/Kernel/Pthread.cpp", "Emulator/Loader/GuestCall.h", "Kernel -> Loader"},
	    {"emulator/src/Kernel/HostTime.cpp", "Emulator/Graphics/Window.h", "Kernel host -> Graphics"},
	    {"emulator/src/Hle/LibGraphicsDriver.cpp", "Emulator/Graphics/Graphics.h", "Hle -> Graphics"},
	    {"emulator/src/Hle/LibGraphicsDriver.cpp", "Emulator/Libs/HleSymbolRegistry.h", "legacy HleSymbolRegistry header"},
	    {"emulator/src/Hle/LibGraphicsDriver.cpp", "Emulator/Kernel/Namespace.h", "legacy Kernel Namespace header"},
	    {"emulator/src/Host/CaptureImageCodec.cpp", "Emulator/Graphics/Utils.h", "Host -> Graphics"},
	    {"emulator/src/Host/ImageSurfaceSdl.cpp", "Emulator/Graphics/Utils.h", "Host -> Graphics"},
	    {"emulator/src/Host/HostWindowSdl.cpp", "Emulator/Graphics/Window.h", "Host -> Graphics"},
	    {"emulator/src/Ports/ControllerInputPort.cpp", "Emulator/Graphics/Window.h", "RuntimeCore -> domain"},
	    {"emulator/src/Libs/LibC.cpp", "Emulator/Graphics/Objects/GpuMemory.h", "Libs -> Graphics"},
	    {"emulator/src/Graphics/ImageAsset.cpp", "SDL_image.h", "Graphics Image -> SDL"},
	    {"emulator/src/Graphics/Window.cpp", "SDL_events.h", "Graphics Window -> SDL"},
	    {"emulator/src/Graphics/Window.cpp", "Emulator/Loader/SystemContent.h", "Graphics Window -> Loader"},
	    {"emulator/src/Graphics/DebugOverlay.cpp", "SDL_events.h", "Graphics DebugOverlay -> SDL"},
	    {"emulator/src/Graphics/DebugOverlay.cpp", "imgui_impl_sdl3.h", "Graphics DebugOverlay -> SDL GUI"},
	    {"emulator/src/Graphics/GraphicsRun.cpp", "Emulator/Agent/EventRing.h", "GraphicsRun -> Agent"},
	    {"emulator/src/Graphics/GraphicsRun.cpp", "vulkan/vulkan.h", "CommandFlow -> Vulkan"},
	    {"emulator/src/Graphics/VideoOut.cpp", "Emulator/Kernel/Time.h", "Graphics -> Kernel time"},
	    {"emulator/src/Graphics/Graphics.cpp", "Emulator/Kernel/Time.h", "Graphics -> Kernel time"},
	    {"emulator/src/Loader/SystemContent.cpp", "Emulator/Graphics/Image.h", "Loader -> Graphics"},
	    {"emulator/src/Loader/RuntimeLinker.cpp", "Emulator/Graphics/Objects/GpuMemory.h", "Loader -> Graphics"},
	    {"emulator/src/Loader/RuntimeLinker.cpp", "Emulator/Profiler.h", "RuntimeLinker -> Profiler"},
	    {"emulator/src/Loader/RuntimeLinker.cpp", "Emulator/Profiling.h", "RuntimeLinker -> Profiler"},
	};
	for (const auto& test: cases)
	{
		const std::string expected = std::string(test.source) + ":1: forbidden include (" + test.rule + "): " + test.include;
		const Files       files    = {{test.source, std::string("#include \"") + test.include + "\"\n"}};
		EXPECT_EQ(Check(files), Diagnostics {expected}) << test.source;
	}
}

TEST(ArchitectureBoundary, RejectsLibsHeadersFromGraphicsErrorAndHleSources)
{
	const char* error_sources[] = {"emulator/src/Graphics/Graphics.cpp", "emulator/src/Graphics/Gen5Driver.cpp",
	                               "emulator/src/Graphics/GraphicsRenderContext.cpp", "emulator/src/Graphics/GraphicsRenderEop.cpp",
	                               "emulator/src/Graphics/VideoOut.cpp"};
	for (const auto* source: error_sources)
	{
		EXPECT_EQ(Check({{source, "#include \"Emulator/Libs/Errno.h\"\n"}}),
		          Diagnostics {std::string(source) + ":1: forbidden include (Graphics -> Libs errno): Emulator/Libs/Errno.h"});
	}
	const char* aggregate_sources[] = {"emulator/src/Graphics/Graphics.cpp",
	                                   "emulator/src/Graphics/Gen5Driver.cpp",
	                                   "emulator/src/Graphics/GraphicsRenderContext.cpp",
	                                   "emulator/src/Graphics/GraphicsRenderEop.cpp",
	                                   "emulator/src/Graphics/VideoOut.cpp",
	                                   "emulator/src/Graphics/GraphicsHleExports.cpp",
	                                   "emulator/src/Graphics/VideoOutHleExports.cpp"};
	for (const auto* source: aggregate_sources)
	{
		EXPECT_EQ(Check({{source, "#include \"Emulator/Libs/Libs.h\"\n"}}),
		          Diagnostics {std::string(source) + ":1: forbidden include (Graphics -> Libs aggregate): Emulator/Libs/Libs.h"});
	}
}

TEST(ArchitectureBoundary, RejectsEverySdlSpellingFromLibs)
{
	EXPECT_EQ(Check({{"emulator/src/Libs/LibSaveData.cpp", "#include \"SDL.h\"\n#include \"SDL_filesystem.h\"\n#include <SDL_stdinc.h>\n"
	                                                       "#include <SDL3/SDL_begin_code.h>\n"
	                                                       "#include \"../../../3rdparty/sdl3/sdl3/include/SDL3/SDL.h\"\n"
	                                                       "#include \"../../../3rdparty/sdl3/sdl3/include/SDL3/SDL_filesystem.h\"\n"}}),
	          (Diagnostics {
	              "emulator/src/Libs/LibSaveData.cpp:1: forbidden include (Libs -> SDL): SDL.h",
	              "emulator/src/Libs/LibSaveData.cpp:2: forbidden include (Libs -> SDL): SDL_filesystem.h",
	              "emulator/src/Libs/LibSaveData.cpp:3: forbidden include (Libs -> SDL): SDL_stdinc.h",
	              "emulator/src/Libs/LibSaveData.cpp:4: forbidden include (Libs -> SDL): SDL3/SDL_begin_code.h",
	              "emulator/src/Libs/LibSaveData.cpp:5: forbidden include (Libs -> SDL): ../../../3rdparty/sdl3/sdl3/include/SDL3/SDL.h",
	              "emulator/src/Libs/LibSaveData.cpp:6: forbidden include (Libs -> SDL): "
	              "../../../3rdparty/sdl3/sdl3/include/SDL3/SDL_filesystem.h",
	          }));
}

TEST(ArchitectureBoundary, RejectsLegacyAliasesAndViews)
{
	EXPECT_EQ(Check({{"emulator/src/Hle/LibGraphicsDriver.cpp",
	                  "void Register(::Kyty::Libs::HleSymbolRegistry* symbols)\n{\n"
	                  "    Libs::HleSymbolResolve s {};\n    s.type = Libs::HleSymbolType::Func;\n}\n"}}),
	          (Diagnostics {
	              "emulator/src/Hle/LibGraphicsDriver.cpp:1: legacy Libs HLE alias: Libs::HleSymbolRegistry",
	              "emulator/src/Hle/LibGraphicsDriver.cpp:3: legacy Libs HLE alias: Libs::HleSymbolResolve",
	              "emulator/src/Hle/LibGraphicsDriver.cpp:4: legacy Libs HLE alias: Libs::HleSymbolType",
	          }));
	EXPECT_EQ(Check({{"emulator/src/Hle/LibGraphicsDriver.cpp",
	                  "namespace Kyty::Libs::LibKernel {\nusing namespace ::Kyty::Kernel;\n} // namespace Kyty::Libs::LibKernel\n"}}),
	          Diagnostics {"emulator/src/Hle/LibGraphicsDriver.cpp:1: legacy LibKernel namespace view: "
	                       "namespace Kyty::Libs::LibKernel {\nusing namespace ::Kyty::Kernel"});
	EXPECT_EQ(
	    Check({{"emulator/src/Hle/LibGraphicsDriver.cpp", "namespace Kyty::Libs::Fiber {\nusing namespace ::Kyty::Kernel::Fiber;\n}\n"}}),
	    Diagnostics {"emulator/src/Hle/LibGraphicsDriver.cpp:1: legacy Fiber namespace view: namespace Kyty::Libs::Fiber {"});
	EXPECT_EQ(Check({{"emulator/include/Emulator/Kernel/EventQueue.h", "namespace Kyty::Kernel::EventQueue {\n"
	                                                                   "using ::Kyty::Libs::LibKernel::KernelUseconds;\n"
	                                                                   "void Wait(const KernelUseconds* t);\n}\n"}}),
	          Diagnostics {"emulator/include/Emulator/Kernel/EventQueue.h:2: legacy LibKernel import: "
	                       "using ::Kyty::Libs::LibKernel::KernelUseconds;"});
	EXPECT_EQ(Check({{"emulator/src/Hle/LibGraphicsDriver.cpp", "namespace Kyty::Libs::Graphics {\nusing Image = ImageAsset;\n}\n"}}),
	          Diagnostics {"emulator/src/Hle/LibGraphicsDriver.cpp:2: legacy Image alias: using Image = ImageAsset;"});
}

TEST(ArchitectureBoundary, RuntimeCoreAcceptsOnlyAuditedMutableGlobals)
{
	EXPECT_EQ(Check({{"emulator/src/Ports/ControllerInputPort.cpp", "static int g_extra_state = 0;\n"}}),
	          Diagnostics {"emulator/src/Ports/ControllerInputPort.cpp:1: unowned runtime-core mutable global: g_extra_state"});
	EXPECT_EQ(Check({{"emulator/src/Config.cpp", "static Config* g_config = nullptr;\n"}}), Diagnostics {});
	EXPECT_EQ(
	    Check({{"emulator/src/Ports/AudioPausePort.cpp", "static std::atomic<bool> g_paused {false};\nstatic const int g_limit = 1;\n"}}),
	    Diagnostics {});
}

TEST(ArchitectureBoundary, RecognizesIncludeDirectiveSpellings)
{
	EXPECT_EQ(Check({{"emulator/src/AudioHost.cpp", "\xEF\xBB\xBF#include \"Emulator/Graphics/Graphics.h\"\n"}}),
	          Diagnostics {"emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h"});
	EXPECT_EQ(Check({{"emulator/src/AudioHost.cpp", "%:include \"Emulator/Graphics/Graphics.h\"\n"}}),
	          Diagnostics {"emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h"});
	EXPECT_EQ(Check({
	              {"emulator/src/Audio.cpp", "#include\"Emulator/Graphics/Shader.h\"\n/**/#include \"Emulator/Graphics/Color.h\"\n"},
	              {"emulator/src/AudioHost.cpp", "#/**/include/**/\"Emulator/Graphics/Graphics.h\"\n"},
	              {"emulator/src/AudioPcm.cpp", "#include \\\n\"Emulator/Graphics/Window.h\"\n"},
	          }),
	          (Diagnostics {
	              "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Shader.h",
	              "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator/Graphics/Color.h",
	              "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",
	              "emulator/src/AudioPcm.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
	          }));
	EXPECT_EQ(Check({{"emulator/src/Audio.cpp", "#include <Emulator/Graphics//Graphics.h>\n#include\v\"Emulator/Graphics/Color.h\"\n"
	                                            "#include\f\"Emulator/Graphics/Window.h\"\n"}}),
	          (Diagnostics {
	              "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics//Graphics.h",
	              "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator/Graphics/Color.h",
	              "emulator/src/Audio.cpp:3: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
	          }));
}

TEST(ArchitectureBoundary, IgnoresIncludesInCommentsAndLiterals)
{
	EXPECT_EQ(Check({{"emulator/src/Audio.cpp", "/*\n#include \"Emulator/Graphics/Graphics.h\"\n*/\n"
	                                            "// #include \"Emulator/Graphics/Window.h\"\n"
	                                            "const char* text = R\"x(\n#include \"Emulator/Graphics/Color.h\"\n)x\";\n"}}),
	          Diagnostics {});
}

TEST(ArchitectureBoundary, CanonicalizesIncludePaths)
{
	EXPECT_EQ(
	    Check({
	        {"emulator/src/Audio.cpp", "#include \"Emulator\\Graphics\\Graphics.h\"\n#include \"Emulator//Graphics/Color.h\"\n"
			                           "#include \"Emulator/Other/../Graphics/Window.h\"\n#include \"emulator/graphics/Shader.h\"\n"
			                           "#include \"Emulator\\Graphics\\GuestTextureLayout.h\"\n"},
	        {"emulator/src/Loader/RuntimeLinker.cpp",
			 "#include \"Kyty\\DevTools\\Telemetry\\Progress.h\"\n#include \"Kyty/Tools/../DevTools/Telemetry/WriterRegistry.h\"\n"},
	    }),
	    (Diagnostics {
	        "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator\\Graphics\\Graphics.h",
	        "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator//Graphics/Color.h",
	        "emulator/src/Audio.cpp:3: forbidden include (Audio -> Graphics): Emulator/Other/../Graphics/Window.h",
	        "emulator/src/Audio.cpp:4: forbidden include (Audio -> Graphics): emulator/graphics/Shader.h",
	        "emulator/src/Audio.cpp:5: forbidden include (Audio -> Graphics): Emulator\\Graphics\\GuestTextureLayout.h",
	        "emulator/src/Loader/RuntimeLinker.cpp:1: forbidden include (RuntimeLinker -> DevTools): Kyty\\DevTools\\Telemetry\\Progress.h",
	        "emulator/src/Loader/RuntimeLinker.cpp:2: forbidden include (RuntimeLinker -> DevTools): "
	        "Kyty/Tools/../DevTools/Telemetry/WriterRegistry.h",
	    }));
	EXPECT_EQ(Check({
	              {"emulator/src/AudioHost.cpp", "#include \"../include/Emulator/Graphics/Graphics.h\"\n"},
	              {"emulator/src/Loader/RuntimeLinker.cpp", "#include \"../../../devtools/include/Kyty/DevTools/Telemetry/Progress.h\"\n"},
	          }),
	          (Diagnostics {
	              "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): ../include/Emulator/Graphics/Graphics.h",
	              "emulator/src/Loader/RuntimeLinker.cpp:1: forbidden include (RuntimeLinker -> DevTools): "
	              "../../../devtools/include/Kyty/DevTools/Telemetry/Progress.h",
	          }));
}

TEST(ArchitectureBoundary, FailsClosedOnUncheckableIncludes)
{
	EXPECT_EQ(Check({{"emulator/src/AudioPcm.cpp", "#include \"/host/private/Emulator/Graphics/Graphics.h\"\n"}}),
	          Diagnostics {"emulator/src/AudioPcm.cpp:1: uncheckable absolute include"});
	EXPECT_EQ(
	    Check({
	        {"emulator/src/Audio.cpp", "#include \"C:..\\..\\source\\emulator\\include\\Emulator\\Graphics\\GuestTextureLayout.h\"\n"},
	        {"emulator/src/Loader/RuntimeLinker.cpp",
			 "#include \"D:..\\..\\source\\devtools\\include\\Kyty\\DevTools\\Telemetry\\Progress.h\"\n"},
	    }),
	    (Diagnostics {"emulator/src/Audio.cpp:1: uncheckable absolute include",
		              "emulator/src/Loader/RuntimeLinker.cpp:1: uncheckable absolute include"}));
	EXPECT_EQ(Check({{"emulator/src/Audio.cpp", "#define GFX \"Emulator/Graphics/Graphics.h\"\n#include GFX\n"}}),
	          Diagnostics {"emulator/src/Audio.cpp:2: uncheckable nonliteral include"});
}

TEST(ArchitectureBoundary, RejectsMissingSourceRoot)
{
	ScopedTestDirectory directory("kyty-boundary");
	const auto          missing = directory.Path() / "missing-source";
	const auto          result  = CheckSourceRoot(missing, false);
	EXPECT_EQ(result.exit_code, 1);
	EXPECT_EQ(result.diagnostics, Diagnostics {"source root is not a directory: " + missing.string()});
}

TEST(ArchitectureBoundary, SortsDiagnosticsBySourceAndLine)
{
	EXPECT_EQ(
	    Check({
	        {"emulator/src/AudioHost.cpp", "#include \"Emulator/Graphics/Window.h\"\n"},
	        {"emulator/src/Loader/RuntimeLinker.cpp", "\n#include \"Kyty/DevTools/Telemetry/Progress.h\"\n"},
	        {"emulator/src/Audio.cpp", std::string(10, '\n') + "#include \"Emulator/Graphics/GuestTextureLayout.h\"\n"
			                                                   "#include \"Emulator/Graphics/Graphics.h\"\n"},
	    }),
	    (Diagnostics {
	        "emulator/src/Audio.cpp:11: forbidden include (Audio -> Graphics): Emulator/Graphics/GuestTextureLayout.h",
	        "emulator/src/Audio.cpp:12: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",
	        "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
	        "emulator/src/Loader/RuntimeLinker.cpp:2: forbidden include (RuntimeLinker -> DevTools): Kyty/DevTools/Telemetry/Progress.h",
	    }));
}

TEST(ArchitectureBoundary, StrictModeRequiresKernelNamespace)
{
	EXPECT_EQ(Check({{"emulator/src/Kernel/EventFlag.cpp", "namespace Kyty::Libs {}\nnamespace Kyty::Kernel {}\n"}}, true),
	          Diagnostics {"emulator/src/Kernel/EventFlag.cpp: kernel implementation must not use the Kyty::Libs namespace"});
	EXPECT_EQ(Check({{"emulator/src/Kernel/EventFlag.cpp", "int kernel_fixture;\n"}}, true),
	          Diagnostics {"emulator/src/Kernel/EventFlag.cpp: kernel implementation must declare a Kyty::Kernel namespace"});
	EXPECT_EQ(Check({{"emulator/src/Kernel/EventFlag.cpp", "int kernel_fixture;\n"}}, false), Diagnostics {});
}

UT_END();
