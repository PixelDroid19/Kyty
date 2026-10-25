// Verifies that the emulator archive graph is acyclic and that each archive
// owns exactly the objects of its domain sources. Reads the archives with the
// host nm and ar tools.
//
// Usage: kyty_archive_dag_integration <build_dir> [<source_dir>]

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Domain
{
	const char*              archive;
	std::vector<const char*> source_globs; // under source/emulator, mirrored from emulator/CMakeLists.txt
};

const std::vector<Domain>& Domains()
{
	static const std::vector<Domain> domains = {
	    {"kyty_runtime_core",
		 {"src/Log.cpp", "src/Config.cpp", "src/GuestRuntimePort.cpp", "src/GuestCodeRanges.cpp", "src/GuestMemory.cpp",
		  "src/GpuMemoryFault.cpp", "src/AtomicFile.cpp", "src/Validation/DomainValidators.cpp", "src/SystemContentPort.cpp",
		  "src/PresentationStats.cpp", "src/VideoFrameMemory.cpp", "src/Ports/*.cpp"}},
	    {"kyty_kernel", {"src/Kernel/*.cpp", "src/Kernel/*.S"}},
	    {"kyty_diagnostics", {"src/Agent/EventRing.cpp", "src/Agent/AgentLifecycle.cpp"}},
	    {"kyty_loader", {"src/Loader/*.cpp"}},
	    {"kyty_host", {"src/Host/*.cpp"}},
	    {"kyty_graphics", {"src/Graphics/*.cpp", "src/Graphics/Objects/*.cpp", "src/Hle/*.cpp"}},
	    {"kyty_hle",
		 {"src/Libs/*.cpp", "src/Network.cpp", "src/NetworkHttpUri.cpp", "src/Audio.cpp", "src/Audio3d.cpp", "src/AudioPropagation.cpp",
		  "src/AudioAjm.cpp", "src/AudioAvPlayer.cpp", "src/AudioHost.cpp", "src/AudioNgs2.cpp", "src/AudioNgs2Reverb.cpp",
		  "src/AudioNgs2Sampler.cpp", "src/Dialog.cpp", "src/Controller.cpp"}},
	    {"kyty_audio_video_backend", {"src/AudioVideoBackend.cpp"}},
	    {"kyty_audio_pcm", {"src/AudioPcm.cpp"}},
	    {"emulator",
		 {"src/Kyty.cpp", "src/Emulator.cpp", "src/Profiler.cpp", "src/Agent/AgentServer.cpp", "src/Agent/AgentSubsystem.cpp",
		  "src/Agent/StallWatch.cpp", "src/Agent/FrameScore.cpp", "src/Agent/DebugSnapshot.cpp", "src/Agent/Protocol.cpp"}},
	};
	return domains;
}

// Objects compiled from outside source/emulator (third-party backends).
constexpr const char* kForeignObject        = "imgui_impl_sdl3.cpp.o";
constexpr const char* kForeignObjectArchive = "kyty_host";

std::optional<fs::path> ArchivePath(const fs::path& build_dir, const std::string& archive)
{
	const std::string file = "lib" + archive + ".a";
	for (const auto& candidate: {build_dir / "emulator" / file, build_dir / "audio_pcm" / file, build_dir / "lib" / file, build_dir / file})
	{
		if (fs::is_regular_file(candidate))
		{
			return candidate;
		}
	}
	return std::nullopt;
}

// Runs a host tool on one archive and returns its stdout lines, or nullopt when it fails.
std::optional<std::vector<std::string>> ToolLines(const std::string& tool, const fs::path& archive)
{
	const std::string path = archive.string();
	if (path.find('\'') != std::string::npos)
	{
		return std::nullopt;
	}
	FILE* pipe = popen((tool + " '" + path + "' 2>/dev/null").c_str(), "r");
	if (pipe == nullptr)
	{
		return std::nullopt;
	}
	std::vector<std::string> lines;
	std::string              line;
	for (int c = std::fgetc(pipe); c != EOF; c = std::fgetc(pipe))
	{
		if (c != '\n')
		{
			line.push_back(static_cast<char>(c));
			continue;
		}
		lines.push_back(line);
		line.clear();
	}
	if (!line.empty())
	{
		lines.push_back(line);
	}
	if (pclose(pipe) != 0)
	{
		return std::nullopt;
	}
	return lines;
}

// Parses "archive:member.o:<value> <type> <symbol>" lines from nm -A.
std::optional<std::set<std::string>> ArchiveSymbols(const fs::path& archive, bool defined_only)
{
	const auto lines = ToolLines(defined_only ? "nm -A --defined-only" : "nm -A -u", archive);
	if (!lines)
	{
		return std::nullopt;
	}
	std::set<std::string> symbols;
	for (const auto& line: *lines)
	{
		const auto first  = line.find(':');
		const auto second = first == std::string::npos ? first : line.find(':', first + 1);
		if (second == std::string::npos)
		{
			continue;
		}
		const auto field = line.substr(second + 1, line.find(':', second + 1) - second - 1);
		const auto start = field.find_last_of(" \t");
		const auto name  = start == std::string::npos ? field : field.substr(start + 1);
		if (!name.empty())
		{
			symbols.insert(name);
		}
	}
	return symbols;
}

bool Intersects(const std::set<std::string>& left, const std::set<std::string>& right)
{
	const auto& small = left.size() < right.size() ? left : right;
	const auto& large = left.size() < right.size() ? right : left;
	for (const auto& symbol: small)
	{
		if (large.count(symbol) != 0)
		{
			return true;
		}
	}
	return false;
}

void CheckDag(const fs::path& build_dir, std::vector<std::string>* diagnostics)
{
	std::map<std::string, std::set<std::string>> defined;
	std::map<std::string, std::set<std::string>> undefined;
	for (const auto& domain: Domains())
	{
		const auto path = ArchivePath(build_dir, domain.archive);
		if (!path)
		{
			diagnostics->push_back("missing archive: cannot locate lib" + std::string(domain.archive) + ".a under " + build_dir.string());
			continue;
		}
		auto defined_symbols   = ArchiveSymbols(*path, true);
		auto undefined_symbols = ArchiveSymbols(*path, false);
		if (!defined_symbols || !undefined_symbols)
		{
			diagnostics->push_back("missing archive: nm failed on " + path->string());
			continue;
		}
		defined[domain.archive]   = std::move(*defined_symbols);
		undefined[domain.archive] = std::move(*undefined_symbols);
	}

	std::map<std::string, std::set<std::string>> edges;
	for (const auto& [target, target_undefined]: undefined)
	{
		for (const auto& [dependency, dependency_defined]: defined)
		{
			if (target != dependency && Intersects(target_undefined, dependency_defined))
			{
				edges[target].insert(dependency);
			}
		}
	}

	// Topological sort; a cycle leaves some archive unprocessed.
	std::set<std::string>    remaining;
	std::vector<std::string> order;
	for (const auto& [name, symbols]: defined)
	{
		remaining.insert(name);
	}
	while (!remaining.empty())
	{
		std::vector<std::string> ready;
		for (const auto& name: remaining)
		{
			const auto& dependencies = edges[name];
			if (!Intersects(dependencies, remaining))
			{
				ready.push_back(name);
			}
		}
		if (ready.empty())
		{
			std::string members;
			for (const auto& name: remaining)
			{
				members += (members.empty() ? "" : ", ") + name;
			}
			diagnostics->push_back("archive link cycle among: " + members);
			return;
		}
		for (const auto& name: ready)
		{
			remaining.erase(name);
			order.push_back(name);
		}
	}

	std::string chain;
	for (auto it = order.rbegin(); it != order.rend(); ++it)
	{
		chain += (chain.empty() ? "" : " -> ") + *it;
	}
	std::printf("archive order (consumers first): %s\n", chain.c_str());
}

std::set<std::string> OwnedSources(const fs::path& emulator_dir, const Domain& domain)
{
	std::set<std::string> owned;
	for (const std::string glob: domain.source_globs)
	{
		const auto star = glob.find('*');
		if (star == std::string::npos)
		{
			if (fs::exists(emulator_dir / glob))
			{
				owned.insert(fs::path(glob).filename().string());
			}
			continue;
		}
		const auto      directory = emulator_dir / glob.substr(0, star);
		const auto      suffix    = glob.substr(star + 1);
		std::error_code error;
		for (const auto& entry: fs::directory_iterator(directory, error))
		{
			const auto name = entry.path().filename().string();
			if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				owned.insert(name);
			}
		}
	}
	return owned;
}

void CheckOwnership(const fs::path& build_dir, const fs::path& source_dir, std::vector<std::string>* diagnostics)
{
	for (const auto& domain: Domains())
	{
		const auto path = ArchivePath(build_dir, domain.archive);
		if (!path)
		{
			continue;
		}
		const auto listed = ToolLines("ar t", *path);
		if (!listed)
		{
			diagnostics->push_back(std::string(domain.archive) + " is missing object list: ar failed on " + path->string());
			continue;
		}
		const std::set<std::string> members(listed->begin(), listed->end());
		std::set<std::string>       expected;
		for (const auto& source: OwnedSources(source_dir / "emulator", domain))
		{
			expected.insert(source + ".o");
		}
		for (const auto& member: *listed)
		{
			const bool foreign = member == kForeignObject && std::string(domain.archive) == kForeignObjectArchive;
			if (expected.count(member) == 0 && !foreign)
			{
				diagnostics->push_back(std::string(domain.archive) + " owns unexpected object " + member);
			}
		}
		for (const auto& member: expected)
		{
			if (member != kForeignObject && members.count(member) == 0)
			{
				diagnostics->push_back(std::string(domain.archive) + " is missing object " + member);
			}
		}
	}
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2 && argc != 3)
	{
		std::fprintf(stderr, "usage: %s <build_dir> [<source_dir>]\n", argv[0]);
		return 2;
	}
	std::vector<std::string> diagnostics;
	CheckDag(argv[1], &diagnostics);
	if (argc == 3)
	{
		CheckOwnership(argv[1], argv[2], &diagnostics);
	}
	for (const auto& diagnostic: diagnostics)
	{
		std::printf("%s\n", diagnostic.c_str());
	}
	return diagnostics.empty() ? 0 : 1;
}
