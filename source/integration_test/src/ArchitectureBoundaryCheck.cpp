#include "ArchitectureBoundaryCheck.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace Kyty::ArchitectureBoundary {

namespace {

namespace fs = std::filesystem;

using StringList = std::initializer_list<std::string_view>;

constexpr size_t kMaxSourceBytes = 4u * 1024u * 1024u;

// The checker reads only these concrete source files. It intentionally does not
// walk directories: this is a narrow dependency guard, not a source graph audit.
constexpr std::string_view kAudioSources[] = {
    "emulator/src/Audio.cpp",   "emulator/src/AudioAvPlayer.cpp", "emulator/src/AudioNgs2.cpp", "emulator/src/AudioPropagation.cpp",
    "emulator/src/Audio3d.cpp", "emulator/src/AudioHost.cpp",     "emulator/src/AudioPcm.cpp",
};
constexpr std::string_view kKernelSources[] = {
    "emulator/src/Kernel/AmprPort.cpp",
    "emulator/src/Kernel/GuestRuntimePort.cpp",
    "emulator/src/Kernel/EventFlag.cpp",
    "emulator/src/Kernel/EventQueue.cpp",
    "emulator/src/Kernel/Fiber.cpp",
    "emulator/src/Kernel/FileSystem.cpp",
    "emulator/src/Kernel/Memory.cpp",
    "emulator/src/Kernel/Pthread.cpp",
    "emulator/src/Kernel/Semaphore.cpp",
    "emulator/src/Kernel/SyncOnAddress.cpp",
    "emulator/src/Kernel/Time.cpp",
    "emulator/include/Emulator/Kernel/AmprPort.h",
    "emulator/include/Emulator/Kernel/Errors.h",
    "emulator/include/Emulator/Kernel/EventFlag.h",
    "emulator/include/Emulator/Kernel/EventQueue.h",
    "emulator/include/Emulator/Kernel/Fiber.h",
    "emulator/include/Emulator/Kernel/FileSystem.h",
    "emulator/include/Emulator/Kernel/GpuMappingLifecycle.h",
    "emulator/include/Emulator/Kernel/HostTime.h",
    "emulator/include/Emulator/Kernel/Memory.h",
    "emulator/include/Emulator/Kernel/Pthread.h",
    "emulator/include/Emulator/Kernel/RetailKernel.h",
    "emulator/include/Emulator/Kernel/Semaphore.h",
    "emulator/include/Emulator/Kernel/SyncOnAddress.h",
    "emulator/include/Emulator/Kernel/Time.h",
    "emulator/include/Emulator/Kernel/TimePort.h",
    "emulator/include/Emulator/Kernel/Trace.h",
};
constexpr std::string_view kKernelHostSources[] = {"emulator/src/Kernel/HostTime.cpp"};
constexpr std::string_view kHleSources[]        = {"emulator/src/Hle/LibGraphicsDriver.cpp", "emulator/src/Hle/LibVideoOut.cpp"};
constexpr std::string_view kHostSources[]       = {
    "emulator/src/Host/CaptureImageCodec.cpp",
    "emulator/src/Host/Clock.cpp",
    "emulator/src/Host/HostInputSdl.cpp",
    "emulator/src/Host/HostWindowSdl.cpp",
    "emulator/src/Host/HostVulkanWindowSdl.cpp",
    "emulator/src/Host/ImageSurfaceSdl.cpp",
    "emulator/src/Host/Platform.cpp",
    "emulator/src/Host/Png.cpp",
};
constexpr std::string_view kLibsSources[]             = {"emulator/src/Libs/LibC.cpp", "emulator/src/Libs/LibCTime.cpp",
                                                         "emulator/src/Libs/LibSaveData.cpp"};
constexpr std::string_view kGraphicsImageSources[]    = {"emulator/src/Graphics/ImageAsset.cpp"};
constexpr std::string_view kDebugOverlaySource        = "emulator/src/Graphics/DebugOverlay.cpp";
constexpr std::string_view kWindowSource              = "emulator/src/Graphics/Window.cpp";
constexpr std::string_view kGraphicsRunSource         = "emulator/src/Graphics/GraphicsRun.cpp";
constexpr std::string_view kGraphicsTimePortSources[] = {
    "emulator/src/Graphics/Graphics.cpp",
    "emulator/src/Graphics/GraphicsRenderContext.cpp",
    "emulator/src/Graphics/GraphicsRenderEop.cpp",
    "emulator/src/Graphics/VideoOut.cpp",
};
constexpr std::string_view kGraphicsErrorSources[] = {
    "emulator/src/Graphics/Graphics.cpp",
    "emulator/src/Graphics/Gen5Driver.cpp",
    "emulator/src/Graphics/GraphicsRenderContext.cpp",
    "emulator/src/Graphics/GraphicsRenderEop.cpp",
    "emulator/src/Graphics/VideoOut.cpp",
};
constexpr std::string_view kGraphicsHleSources[] = {
    "emulator/src/Graphics/GraphicsHleExports.cpp",
    "emulator/src/Graphics/VideoOutHleExports.cpp",
};
constexpr std::string_view kLoaderSources[] = {"emulator/src/Loader/SystemContent.cpp"};
// The Gen5 command-flow and shader decode layer is Vulkan-neutral: it turns
// guest packets and shader words into IR without touching GPU objects.
constexpr std::string_view kCommandFlowSources[] = {
    "emulator/src/Graphics/GraphicsRun.cpp",          "emulator/src/Graphics/GraphicsRunHelpers.cpp",
    "emulator/src/Graphics/GraphicsRunOpParsers.cpp", "emulator/src/Graphics/GraphicsRunRegisterParsers.cpp",
    "emulator/src/Graphics/GraphicsRunTrace.cpp",     "emulator/src/Graphics/Pm4.cpp",
    "emulator/src/Graphics/ShaderParse.cpp",          "emulator/src/Graphics/ShaderParseDS.cpp",
    "emulator/src/Graphics/ShaderParseEXP.cpp",       "emulator/src/Graphics/ShaderParseMIMG.cpp",
    "emulator/src/Graphics/ShaderParseMTBUF.cpp",     "emulator/src/Graphics/ShaderParseMUBUF.cpp",
    "emulator/src/Graphics/ShaderParseOperands.cpp",  "emulator/src/Graphics/ShaderParseSMEM.cpp",
    "emulator/src/Graphics/ShaderParseSMRD.cpp",      "emulator/src/Graphics/ShaderParseSOP1.cpp",
    "emulator/src/Graphics/ShaderParseSOP2.cpp",      "emulator/src/Graphics/ShaderParseSOPC.cpp",
    "emulator/src/Graphics/ShaderParseSOPK.cpp",      "emulator/src/Graphics/ShaderParseSOPP.cpp",
    "emulator/src/Graphics/ShaderParseVINTRP.cpp",    "emulator/src/Graphics/ShaderParseVOP1.cpp",
    "emulator/src/Graphics/ShaderParseVOP2.cpp",      "emulator/src/Graphics/ShaderParseVOP3.cpp",
    "emulator/src/Graphics/ShaderParseVOPC.cpp",
};
// Neutral runtime services below every domain (kyty_runtime_core). They must
// not reach up into any domain implementation.
constexpr std::string_view kRuntimeCoreSources[] = {
    "emulator/src/Log.cpp",
    "emulator/src/Config.cpp",
    "emulator/src/GuestRuntimePort.cpp",
    "emulator/src/GuestMemory.cpp",
    "emulator/src/GpuMemoryFault.cpp",
    "emulator/src/AtomicFile.cpp",
    "emulator/src/Validation/DomainValidators.cpp",
    "emulator/src/SystemContentPort.cpp",
    "emulator/src/PresentationStats.cpp",
    "emulator/src/VideoFrameMemory.cpp",
    "emulator/src/Ports/ControllerInputPort.cpp",
    "emulator/src/Ports/AudioPausePort.cpp",
};
constexpr std::string_view kRuntimeLinkerSource = "emulator/src/Loader/RuntimeLinker.cpp";
constexpr std::string_view kGen5DriverSource    = "emulator/src/Graphics/Gen5Driver.cpp";

// Audited mutable globals in the neutral runtime layer: each is service-owned
// state with a lifecycle. Any other mutable global there is rejected.
const std::map<std::string_view, std::set<std::string_view>>& AuditedRuntimeCoreGlobals()
{
	static const std::map<std::string_view, std::set<std::string_view>> globals = {
	    {"emulator/src/Log.cpp",
		 {"g_log_initialized", "g_dir", "g_file", "g_colored_printf", "g_thread_local_files", "g_file_log_bytes", "g_file_log_capped"}},
	    {"emulator/src/Config.cpp", {"g_config"}},
	    {"emulator/src/VideoFrameMemory.cpp", {"g_callbacks"}},
	};
	return globals;
}

std::vector<std::string_view> AllowedSources()
{
	std::vector<std::string_view> sources;
	auto                          append = [&](const auto& list) { sources.insert(sources.end(), std::begin(list), std::end(list)); };
	append(kCommandFlowSources);
	append(kRuntimeCoreSources);
	append(kAudioSources);
	append(kKernelSources);
	append(kKernelHostSources);
	append(kHleSources);
	append(kHostSources);
	append(kLibsSources);
	append(kGraphicsImageSources);
	sources.push_back(kDebugOverlaySource);
	sources.push_back(kWindowSource);
	append(kLoaderSources);
	append(kGraphicsTimePortSources);
	sources.push_back(kGen5DriverSource);
	append(kGraphicsHleSources);
	sources.push_back(kRuntimeLinkerSource);
	return sources;
}

template <size_t N>
bool Contains(const std::string_view (&list)[N], std::string_view value)
{
	return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

struct Violation
{
	std::string source_path;
	int         line_number = 0;
	std::string rule;
	std::string include_path;
	std::string message;

	[[nodiscard]] std::string Diagnostic() const
	{
		const std::string location = source_path + ":" + std::to_string(line_number) + ": ";
		if (!message.empty())
		{
			return location + message;
		}
		return location + "forbidden include (" + rule + "): " + include_path;
	}
};

bool StartsWith(std::string_view text, std::string_view prefix)
{
	return text.substr(0, prefix.size()) == prefix;
}

std::string ToLowerAscii(std::string_view text)
{
	std::string lower(text);
	for (auto& character: lower)
	{
		if (character >= 'A' && character <= 'Z')
		{
			character = static_cast<char>(character - 'A' + 'a');
		}
	}
	return lower;
}

// Normalizes separators and lexical segments, then lower-cases unconditionally:
// that catches the Windows spelling and fails closed for a path that would not
// compile on a case-sensitive host.
std::string CanonicalIncludePath(std::string_view include_path)
{
	std::vector<std::string> segments;
	std::string              segment;
	auto                     flush = [&]()
	{
		if (segment.empty() || segment == ".")
		{
			segment.clear();
			return;
		}
		if (segment == ".." && !segments.empty() && segments.back() != "..")
		{
			segments.pop_back();
		} else
		{
			segments.push_back(segment);
		}
		segment.clear();
	};
	for (const char character: include_path)
	{
		if (character == '/' || character == '\\')
		{
			flush();
			continue;
		}
		segment += character;
	}
	flush();
	std::string joined;
	for (size_t index = 0; index < segments.size(); index++)
	{
		joined += (index == 0 ? "" : "/") + segments[index];
	}
	return ToLowerAscii(joined);
}

bool IsAbsoluteIncludePath(std::string_view include_path)
{
	if (!include_path.empty() && (include_path[0] == '/' || include_path[0] == '\\'))
	{
		return true;
	}
	const bool drive_letter = include_path.size() >= 2 &&
	                          ((include_path[0] >= 'A' && include_path[0] <= 'Z') || (include_path[0] >= 'a' && include_path[0] <= 'z'));
	return drive_letter && include_path[1] == ':';
}

std::string_view Basename(std::string_view path)
{
	const auto slash = path.rfind('/');
	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool IsSdlInclude(std::string_view canonical_path)
{
	const auto basename = Basename(canonical_path);
	if (basename == "sdl.h" || StartsWith(basename, "sdl_"))
	{
		return true;
	}
	size_t start = 0;
	while (start <= canonical_path.size())
	{
		const auto end     = canonical_path.find('/', start);
		const auto segment = canonical_path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
		if (segment == "sdl" || segment == "sdl3")
		{
			return true;
		}
		if (end == std::string_view::npos)
		{
			break;
		}
		start = end + 1;
	}
	return false;
}

class IncludeCandidates
{
public:
	IncludeCandidates(std::string_view relative_path, std::string_view include_path)
	{
		m_paths.push_back(CanonicalIncludePath(include_path));
		if (!IsAbsoluteIncludePath(include_path))
		{
			const auto slash  = relative_path.rfind('/');
			const auto parent = relative_path.substr(0, slash);
			m_paths.push_back(CanonicalIncludePath(std::string(parent) + "/" + std::string(include_path)));
		}
	}

	[[nodiscard]] bool AnyStartsWith(StringList prefixes) const
	{
		for (const auto& path: m_paths)
		{
			for (const auto prefix: prefixes)
			{
				if (StartsWith(path, ToLowerAscii(prefix)))
				{
					return true;
				}
			}
		}
		return false;
	}

	[[nodiscard]] bool AnyEquals(StringList headers) const
	{
		for (const auto& path: m_paths)
		{
			for (const auto header: headers)
			{
				if (path == CanonicalIncludePath(header))
				{
					return true;
				}
			}
		}
		return false;
	}

	[[nodiscard]] bool AnyContains(std::string_view text) const
	{
		return std::any_of(m_paths.begin(), m_paths.end(), [&](const std::string& path) { return path.find(text) != std::string::npos; });
	}

	[[nodiscard]] bool AnySdl() const
	{
		return std::any_of(m_paths.begin(), m_paths.end(), [](const std::string& path) { return IsSdlInclude(path); });
	}

	[[nodiscard]] bool AnyBasename(std::string_view name) const
	{
		return std::any_of(m_paths.begin(), m_paths.end(), [&](const std::string& path) { return Basename(path) == name; });
	}

private:
	std::vector<std::string> m_paths;
};

constexpr StringList kGraphicsPrefixes = {"Emulator/Graphics/", "emulator/include/Emulator/Graphics/"};
constexpr StringList kLoaderPrefixes   = {"Emulator/Loader/", "emulator/include/Emulator/Loader/"};
constexpr StringList kAgentPrefixes    = {"Emulator/Agent/", "emulator/include/Emulator/Agent/"};
constexpr StringList kDevToolsPrefixes = {"Emulator/DevTools/", "Kyty/DevTools/", "emulator/include/Emulator/DevTools/",
                                          "devtools/include/Kyty/DevTools/", "lib/DevTools/include/Kyty/DevTools/"};
constexpr StringList kDomainPrefixes   = {
    "Emulator/Graphics/", "emulator/include/Emulator/Graphics/", "emulator/libs/", "emulator/include/emulator/libs/",
    "emulator/loader/",   "emulator/include/emulator/loader/",   "emulator/hle/",  "emulator/include/emulator/hle/",
    "emulator/agent/",    "emulator/include/emulator/agent/",    "emulator/host/", "emulator/include/emulator/host/",
    "emulator/kernel/",   "emulator/include/emulator/kernel/",
};

// Returns the first rule the include breaks, in contract order, or nothing.
std::optional<std::string_view> RuleForInclude(std::string_view relative_path, std::string_view include_path)
{
	const IncludeCandidates include(relative_path, include_path);
	const bool              error_source = Contains(kGraphicsErrorSources, relative_path);

	if (include.AnyStartsWith({"emulator/libs/hlesymbolregistry.h"}))
	{
		return "legacy HleSymbolRegistry header";
	}
	if (include.AnyStartsWith({"emulator/kernel/namespace.h"}))
	{
		return "legacy Kernel Namespace header";
	}
	if (Contains(kCommandFlowSources, relative_path) && (include.AnyStartsWith({"vulkan/"}) || include.AnyContains("vulkan_core")))
	{
		return "CommandFlow -> Vulkan";
	}
	if (Contains(kRuntimeCoreSources, relative_path) && include.AnyStartsWith(kDomainPrefixes))
	{
		return "RuntimeCore -> domain";
	}
	if (Contains(kAudioSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Audio -> Graphics";
	}
	if (Contains(kAudioSources, relative_path) && include.AnyStartsWith(kLoaderPrefixes))
	{
		return "Audio -> Loader";
	}
	if (Contains(kKernelSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Kernel -> Graphics";
	}
	if (Contains(kKernelSources, relative_path) &&
	    include.AnyEquals({"Emulator/Libs/Errno.h", "Emulator/Libs/Libs.h", "Emulator/Network.h"}))
	{
		return "Kernel -> Libs";
	}
	if (Contains(kKernelSources, relative_path) && include.AnyStartsWith(kLoaderPrefixes))
	{
		return "Kernel -> Loader";
	}
	if (Contains(kKernelHostSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Kernel host -> Graphics";
	}
	if (Contains(kHleSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Hle -> Graphics";
	}
	if (Contains(kHostSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Host -> Graphics";
	}
	if (Contains(kLibsSources, relative_path) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Libs -> Graphics";
	}
	if (Contains(kLibsSources, relative_path) && include.AnySdl())
	{
		return "Libs -> SDL";
	}
	if (Contains(kGraphicsImageSources, relative_path) && include.AnySdl())
	{
		return "Graphics Image -> SDL";
	}
	if (relative_path == kDebugOverlaySource && include.AnySdl())
	{
		return "Graphics DebugOverlay -> SDL";
	}
	if (relative_path == kDebugOverlaySource && include.AnyBasename("imgui_impl_sdl3.h"))
	{
		return "Graphics DebugOverlay -> SDL GUI";
	}
	if (relative_path == kWindowSource && include.AnySdl())
	{
		return "Graphics Window -> SDL";
	}
	if (relative_path == kWindowSource && include.AnyStartsWith(kLoaderPrefixes))
	{
		return "Graphics Window -> Loader";
	}
	if (relative_path == kGraphicsRunSource && include.AnyStartsWith(kAgentPrefixes))
	{
		return "GraphicsRun -> Agent";
	}
	if (Contains(kGraphicsTimePortSources, relative_path) && include.AnyEquals({"Emulator/Kernel/Time.h"}))
	{
		return "Graphics -> Kernel time";
	}
	if (error_source && include.AnyEquals({"Emulator/Libs/Errno.h"}))
	{
		return "Graphics -> Libs errno";
	}
	if ((error_source || Contains(kGraphicsHleSources, relative_path)) && include.AnyEquals({"Emulator/Libs/Libs.h"}))
	{
		return "Graphics -> Libs aggregate";
	}
	if ((Contains(kLoaderSources, relative_path) || relative_path == kRuntimeLinkerSource) && include.AnyStartsWith(kGraphicsPrefixes))
	{
		return "Loader -> Graphics";
	}
	if (relative_path == kRuntimeLinkerSource && include.AnyStartsWith(kDevToolsPrefixes))
	{
		return "RuntimeLinker -> DevTools";
	}
	if (relative_path == kRuntimeLinkerSource &&
	    include.AnyEquals({"Emulator/Profiler.h", "Emulator/Profiling.h", "emulator/include/Emulator/Profiler.h",
	                       "emulator/include/Emulator/Profiling.h"}))
	{
		return "RuntimeLinker -> Profiler";
	}
	return std::nullopt;
}

bool IsValidUtf8(std::string_view text)
{
	size_t index = 0;
	while (index < text.size())
	{
		const auto lead = static_cast<uint8_t>(text[index]);
		if (lead < 0x80u)
		{
			index++;
			continue;
		}
		size_t   length  = 0;
		uint32_t code    = 0;
		uint32_t minimum = 0;
		if ((lead & 0xE0u) == 0xC0u)
		{
			length  = 2;
			code    = lead & 0x1Fu;
			minimum = 0x80u;
		} else if ((lead & 0xF0u) == 0xE0u)
		{
			length  = 3;
			code    = lead & 0x0Fu;
			minimum = 0x800u;
		} else if ((lead & 0xF8u) == 0xF0u)
		{
			length  = 4;
			code    = lead & 0x07u;
			minimum = 0x10000u;
		} else
		{
			return false;
		}
		if (index + length > text.size())
		{
			return false;
		}
		for (size_t offset = 1; offset < length; offset++)
		{
			const auto continuation = static_cast<uint8_t>(text[index + offset]);
			if ((continuation & 0xC0u) != 0x80u)
			{
				return false;
			}
			code = (code << 6u) | (continuation & 0x3Fu);
		}
		if (code < minimum || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
		{
			return false;
		}
		index += length;
	}
	return true;
}

struct SourceText
{
	std::optional<std::string> contents;
	std::optional<std::string> error;
};

// A missing allowlisted file is not a violation: fixtures may provide only some.
SourceText ReadAllowlistedSource(const fs::path& root, std::string_view relative_path)
{
	const auto      path = root / fs::path(std::string(relative_path)).make_preferred();
	std::error_code error;
	if (!fs::exists(path, error))
	{
		if (error)
		{
			return {std::nullopt, std::string(relative_path) + ": unable to resolve source file"};
		}
		return {};
	}
	const auto resolved = fs::canonical(path, error);
	if (error)
	{
		return {std::nullopt, std::string(relative_path) + ": unable to resolve source file"};
	}
	const auto relative = resolved.lexically_relative(root);
	if (relative.empty() || *relative.begin() == "..")
	{
		return {std::nullopt, std::string(relative_path) + ": source file escapes supplied root"};
	}
	if (!fs::is_regular_file(resolved, error))
	{
		return {std::nullopt, std::string(relative_path) + ": source file is not a regular file"};
	}
	std::ifstream stream(resolved, std::ios::binary);
	if (!stream)
	{
		return {std::nullopt, std::string(relative_path) + ": unable to read source file"};
	}
	std::string contents(kMaxSourceBytes + 1u, '\0');
	stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
	if (stream.bad())
	{
		return {std::nullopt, std::string(relative_path) + ": unable to read source file"};
	}
	contents.resize(static_cast<size_t>(stream.gcount()));
	if (contents.size() > kMaxSourceBytes)
	{
		return {std::nullopt, std::string(relative_path) + ": source file exceeds " + std::to_string(kMaxSourceBytes) + " byte limit"};
	}
	if (!IsValidUtf8(contents))
	{
		return {std::nullopt, std::string(relative_path) + ": source file is not valid UTF-8"};
	}
	return {std::move(contents), std::nullopt};
}

struct SplicedText
{
	std::string      text;
	std::vector<int> line_numbers;
};

// Applies the escaped-newline translation phase and keeps physical line numbers.
SplicedText SpliceEscapedNewlines(std::string_view contents)
{
	SplicedText spliced;
	int         line  = 1;
	size_t      index = 0;
	while (index < contents.size())
	{
		const char character = contents[index];
		if (character == '\\' && index + 1 < contents.size())
		{
			const char following = contents[index + 1];
			if (following == '\n')
			{
				index += 2;
				line++;
				continue;
			}
			if (following == '\r')
			{
				index += 2;
				if (index < contents.size() && contents[index] == '\n')
				{
					index++;
				}
				line++;
				continue;
			}
		}
		spliced.text += character;
		spliced.line_numbers.push_back(line);
		if (character == '\n' || (character == '\r' && (index + 1 == contents.size() || contents[index + 1] != '\n')))
		{
			line++;
		}
		index++;
	}
	return spliced;
}

size_t SkipQuotedLiteral(std::string_view text, size_t start)
{
	const char quote = text[start];
	size_t     index = start + 1;
	while (index < text.size())
	{
		if (text[index] == '\\' && index + 1 < text.size())
		{
			index += 2;
			continue;
		}
		if (text[index] == quote)
		{
			return index + 1;
		}
		index++;
	}
	return index;
}

// Skips a C++ raw string only when its delimiter is structurally valid (at most
// 16 characters, none of them space, parentheses, backslash or line breaks).
size_t SkipRawStringLiteral(std::string_view text, size_t start)
{
	const size_t search_end    = std::min(text.size(), start + 19);
	size_t       delimiter_end = std::string_view::npos;
	for (size_t index = start + 2; index < search_end; index++)
	{
		if (text[index] == '(')
		{
			delimiter_end = index;
			break;
		}
	}
	if (delimiter_end == std::string_view::npos)
	{
		return start + 1;
	}
	const auto delimiter = text.substr(start + 2, delimiter_end - start - 2);
	if (delimiter.find_first_of(" ()\\\t\r\n") != std::string_view::npos)
	{
		return start + 1;
	}
	const std::string closing       = ")" + std::string(delimiter) + "\"";
	const auto        closing_start = text.find(closing, delimiter_end + 1);
	if (closing_start == std::string_view::npos)
	{
		return text.size();
	}
	return closing_start + closing.size();
}

bool IsHorizontalWhitespace(char character)
{
	return character == ' ' || character == '\t' || character == '\f' || character == '\v';
}

struct IncludeDirective
{
	int                        line_number = 0;
	std::optional<std::string> include_path;
};

// Finds #include directives after comment removal, failing closed (no path) for
// any include whose header name is not a literal.
class IncludeScanner
{
public:
	explicit IncludeScanner(std::string_view contents)
	{
		constexpr std::string_view bom = "\xEF\xBB\xBF";
		m_spliced                      = SpliceEscapedNewlines(StartsWith(contents, bom) ? contents.substr(bom.size()) : contents);
	}

	std::vector<IncludeDirective> Scan()
	{
		const std::string_view text = m_spliced.text;
		while (m_index < text.size())
		{
			const char character = text[m_index];
			if (character == '\r' || character == '\n')
			{
				ConsumeNewline();
				continue;
			}
			if (character == '/' && m_index + 1 < text.size() && (text[m_index + 1] == '/' || text[m_index + 1] == '*'))
			{
				SkipComment();
				continue;
			}
			if (m_state == State::AfterInclude && (character == '<' || character == '"'))
			{
				ConsumeHeaderName(character == '<' ? '>' : '"');
				m_state = State::NotDirective;
				continue;
			}
			if (character == 'R' && m_index + 1 < text.size() && text[m_index + 1] == '"')
			{
				NoteLiteral();
				m_index = SkipRawStringLiteral(text, m_index);
				continue;
			}
			if (character == '"' || character == '\'')
			{
				NoteLiteral();
				m_index = SkipQuotedLiteral(text, m_index);
				continue;
			}
			if (IsHorizontalWhitespace(character))
			{
				ConsumeWhitespace();
				m_index++;
				continue;
			}
			if (AdvanceDirective(character))
			{
				continue;
			}
			m_index++;
		}
		return m_directives;
	}

private:
	enum class State
	{
		Prefix,
		AfterHash,
		IncludeKeyword,
		AfterInclude,
		NotDirective
	};

	static constexpr std::string_view kKeyword = "include";

	void ResetLine()
	{
		m_state          = State::Prefix;
		m_keyword_index  = 0;
		m_separator_seen = false;
		m_directive_line = 0;
	}

	void ConsumeWhitespace()
	{
		if (m_state == State::IncludeKeyword)
		{
			m_state = State::NotDirective;
		} else if (m_state == State::AfterInclude)
		{
			m_separator_seen = true;
		}
	}

	void ConsumeNewline()
	{
		const std::string_view text = m_spliced.text;
		m_index += (text[m_index] == '\r' && m_index + 1 < text.size() && text[m_index + 1] == '\n') ? 2 : 1;
		ResetLine();
	}

	void SkipComment()
	{
		const std::string_view text = m_spliced.text;
		ConsumeWhitespace();
		const bool line_comment = text[m_index + 1] == '/';
		m_index += 2;
		while (m_index < text.size())
		{
			if (line_comment && (text[m_index] == '\r' || text[m_index] == '\n'))
			{
				return;
			}
			if (!line_comment && m_index + 1 < text.size() && text[m_index] == '*' && text[m_index + 1] == '/')
			{
				m_index += 2;
				return;
			}
			if (!line_comment && (text[m_index] == '\r' || text[m_index] == '\n'))
			{
				ConsumeNewline();
				continue;
			}
			m_index++;
		}
	}

	void ConsumeHeaderName(char terminator)
	{
		const std::string_view text  = m_spliced.text;
		size_t                 index = m_index + 1;
		while (index < text.size())
		{
			if (text[index] == terminator)
			{
				m_directives.push_back({m_directive_line, std::string(text.substr(m_index + 1, index - m_index - 1))});
				m_index = index + 1;
				return;
			}
			if (text[index] == '\r' || text[index] == '\n')
			{
				break;
			}
			index++;
		}
		m_directives.push_back({m_directive_line, std::nullopt});
		m_index = index;
	}

	// A string literal after `#include ` is a nonliteral include (for example a macro argument).
	void NoteLiteral()
	{
		if (m_state == State::AfterInclude && m_separator_seen)
		{
			m_directives.push_back({m_directive_line, std::nullopt});
		}
		m_state = State::NotDirective;
	}

	// Steps the directive recognizer by one character; returns true when it
	// consumed more than that character.
	bool AdvanceDirective(char character)
	{
		const std::string_view text = m_spliced.text;
		switch (m_state)
		{
			case State::Prefix:
				if (character == '#')
				{
					m_state          = State::AfterHash;
					m_directive_line = m_spliced.line_numbers[m_index];
				} else if (character == '%' && m_index + 1 < text.size() && text[m_index + 1] == ':')
				{
					m_state          = State::AfterHash;
					m_directive_line = m_spliced.line_numbers[m_index];
					m_index += 2;
					return true;
				} else
				{
					m_state = State::NotDirective;
				}
				break;
			case State::AfterHash:
				m_state         = character == kKeyword[0] ? State::IncludeKeyword : State::NotDirective;
				m_keyword_index = 1;
				break;
			case State::IncludeKeyword:
				if (m_keyword_index < kKeyword.size() && character == kKeyword[m_keyword_index])
				{
					if (++m_keyword_index == kKeyword.size())
					{
						m_state          = State::AfterInclude;
						m_separator_seen = false;
					}
				} else
				{
					m_state = State::NotDirective;
				}
				break;
			case State::AfterInclude:
				if (m_separator_seen)
				{
					m_directives.push_back({m_directive_line, std::nullopt});
				}
				m_state = State::NotDirective;
				break;
			case State::NotDirective: break;
		}
		return false;
	}

	SplicedText                   m_spliced;
	std::vector<IncludeDirective> m_directives;
	size_t                        m_index          = 0;
	State                         m_state          = State::Prefix;
	size_t                        m_keyword_index  = 0;
	bool                          m_separator_seen = false;
	int                           m_directive_line = 0;
};

int LineOf(const std::string& contents, std::ptrdiff_t offset)
{
	return static_cast<int>(std::count(contents.begin(), contents.begin() + offset, '\n')) + 1;
}

std::string Strip(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t\r\n\f\v");
	if (first == std::string::npos)
	{
		return {};
	}
	const auto last = text.find_last_not_of(" \t\r\n\f\v");
	return text.substr(first, last - first + 1);
}

struct LegacyPattern
{
	std::regex       pattern;
	std::string_view label;
	bool             strip_match;
};

const std::vector<LegacyPattern>& LegacyPatterns()
{
	// The LibKernel/Fiber views re-exported the Kernel domain through Kyty::Libs;
	// re-opening them would silently recreate the removed alias layer.
	static const std::vector<LegacyPattern> patterns = {
	    {std::regex(R"(\bLibs::HleSymbolRegistry\b)"), "legacy Libs HLE alias", false},
	    {std::regex(R"(\bLibs::HleSymbolResolve\b)"), "legacy Libs HLE alias", false},
	    {std::regex(R"(\bLibs::HleSymbolType\b)"), "legacy Libs HLE alias", false},
	    {std::regex(R"(\bnamespace\s+Kyty::Libs::LibKernel\s*\{(?:[^{}]|\{[^{}]*\})*?using\s+namespace\s+::?Kyty::Kernel)"),
		 "legacy LibKernel namespace view", true},
	    {std::regex(R"(\bnamespace\s+Kyty::Libs::Fiber\s*\{)"), "legacy Fiber namespace view", true},
	    {std::regex(R"(\busing\s+(?:::)?(?:Kyty::)?Libs::LibKernel::[A-Za-z_]\w*\s*;)"), "legacy LibKernel import", true},
	    {std::regex(R"(\busing\s+Image\s*=\s*ImageAsset\s*;)"), "legacy Image alias", true},
	};
	return patterns;
}

void CheckLegacyPatterns(std::string_view relative_path, const std::string& contents, std::vector<Violation>* violations)
{
	for (const auto& legacy: LegacyPatterns())
	{
		for (auto match = std::sregex_iterator(contents.begin(), contents.end(), legacy.pattern); match != std::sregex_iterator(); ++match)
		{
			const auto text = legacy.strip_match ? Strip(match->str()) : match->str();
			violations->push_back({std::string(relative_path),
			                       LineOf(contents, match->position()),
			                       std::string(legacy.label),
			                       {},
			                       std::string(legacy.label) + ": " + text});
		}
	}
}

void CheckRuntimeCoreGlobals(std::string_view relative_path, const std::string& contents, std::vector<Violation>* violations)
{
	static const std::regex mutable_global(
	    R"(\bstatic\s+(?!(?:const|thread_local|constexpr|inline)\b)([A-Za-z_:<>,\s*&\[\]0-9]+?)\s+(g_[A-Za-z0-9_]+)\s*(?:=|;|\())");
	const auto& audited_globals = AuditedRuntimeCoreGlobals();
	const auto  audited         = audited_globals.find(relative_path);
	for (auto match = std::sregex_iterator(contents.begin(), contents.end(), mutable_global); match != std::sregex_iterator(); ++match)
	{
		const auto type = (*match)[1].str();
		const auto name = (*match)[2].str();
		// Synchronization primitives and atomics are audited classes, not state.
		const bool synchronization = type.find("Mutex") != std::string::npos || type.find("atomic") != std::string::npos ||
		                             type.find("LockGuard") != std::string::npos;
		if (synchronization || (audited != audited_globals.end() && audited->second.count(name) != 0))
		{
			continue;
		}
		violations->push_back({std::string(relative_path),
		                       LineOf(contents, match->position()),
		                       "unowned runtime-core global",
		                       {},
		                       "unowned runtime-core mutable global: " + name});
	}
}

void CheckSource(std::string_view relative_path, const std::string& contents, std::vector<Violation>* violations)
{
	for (const auto& directive: IncludeScanner(contents).Scan())
	{
		Violation violation {std::string(relative_path), directive.line_number, {}, {}, {}};
		if (!directive.include_path.has_value())
		{
			violation.message = "uncheckable nonliteral include";
			violations->push_back(violation);
			continue;
		}
		if (IsAbsoluteIncludePath(*directive.include_path))
		{
			violation.message = "uncheckable absolute include";
			violations->push_back(violation);
			continue;
		}
		const auto rule = RuleForInclude(relative_path, *directive.include_path);
		if (!rule.has_value())
		{
			continue;
		}
		violation.rule         = std::string(*rule);
		violation.include_path = *directive.include_path;
		violations->push_back(violation);
	}
	CheckLegacyPatterns(relative_path, contents, violations);
	if (Contains(kRuntimeCoreSources, relative_path))
	{
		CheckRuntimeCoreGlobals(relative_path, contents, violations);
	}
}

using InputDiagnostic = std::pair<std::string, std::string>;

// Every kernel implementation file must belong to the guest-kernel namespace.
void CheckKernelNamespaces(const fs::path& root, std::vector<InputDiagnostic>* diagnostics)
{
	static const std::regex kernel_namespace(R"(\bnamespace\s+Kyty::Kernel(?:\s*::|\s*\{))");
	static const std::regex libs_namespace(R"(\bnamespace\s+Kyty::Libs(?:\s*::|\s*\{))");
	const std::string       kernel_root = "emulator/src/Kernel";
	std::error_code         error;
	const auto              directory = root / fs::path(kernel_root).make_preferred();
	if (!fs::is_directory(directory, error))
	{
		return;
	}
	std::vector<std::string> files;
	for (fs::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error))
	{
		if (entry->path().extension() == ".cpp" && entry->is_regular_file(error))
		{
			files.push_back(kernel_root + "/" + entry->path().filename().generic_string());
		}
	}
	if (error)
	{
		diagnostics->push_back({kernel_root, kernel_root + ": unable to enumerate kernel sources: " + error.message()});
		return;
	}
	std::sort(files.begin(), files.end());
	for (const auto& relative_path: files)
	{
		const auto source = ReadAllowlistedSource(root, relative_path);
		if (source.error.has_value())
		{
			diagnostics->push_back({relative_path, *source.error});
			continue;
		}
		if (!source.contents.has_value())
		{
			continue;
		}
		if (std::regex_search(*source.contents, libs_namespace))
		{
			diagnostics->push_back({relative_path, relative_path + ": kernel implementation must not use the Kyty::Libs namespace"});
		}
		if (!std::regex_search(*source.contents, kernel_namespace))
		{
			diagnostics->push_back({relative_path, relative_path + ": kernel implementation must declare a Kyty::Kernel namespace"});
		}
	}
}

} // namespace

CheckResult CheckSourceRoot(const fs::path& source_root, bool strict)
{
	std::error_code error;
	const auto      root = fs::canonical(source_root, error);
	if (error || !fs::is_directory(root, error))
	{
		return {1, {"source root is not a directory: " + source_root.string()}};
	}

	std::vector<InputDiagnostic> input_diagnostics;
	if (strict)
	{
		CheckKernelNamespaces(root, &input_diagnostics);
	}
	std::vector<Violation> violations;
	for (const auto relative_path: AllowedSources())
	{
		const auto source = ReadAllowlistedSource(root, relative_path);
		if (source.error.has_value())
		{
			input_diagnostics.push_back({std::string(relative_path), *source.error});
			continue;
		}
		if (source.contents.has_value())
		{
			CheckSource(relative_path, *source.contents, &violations);
		}
	}

	std::sort(input_diagnostics.begin(), input_diagnostics.end());
	std::stable_sort(violations.begin(), violations.end(),
	                 [](const Violation& a, const Violation& b)
	                 {
		                 return std::tie(a.source_path, a.line_number, a.include_path, a.rule) <
						        std::tie(b.source_path, b.line_number, b.include_path, b.rule);
	                 });
	CheckResult result;
	for (const auto& diagnostic: input_diagnostics)
	{
		result.diagnostics.push_back(diagnostic.second);
	}
	for (const auto& violation: violations)
	{
		result.diagnostics.push_back(violation.Diagnostic());
	}
	result.exit_code = result.diagnostics.empty() ? 0 : 1;
	return result;
}

} // namespace Kyty::ArchitectureBoundary
