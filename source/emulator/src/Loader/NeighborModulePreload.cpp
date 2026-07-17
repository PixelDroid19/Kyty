#include "Emulator/Loader/NeighborModulePreload.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cstdio>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::NeighborModulePreload {
namespace {

using Core::File;
using Core::String;
using Kyty::Vector;

bool HasModuleExtension(const String& name)
{
	const String lower = name.ToLower();
	return lower.EndsWith(U".prx") || lower.EndsWith(U".sprx") || lower.EndsWith(U".elf");
}

} // namespace

bool IsMainExecutableName(const String& file_name)
{
	const String base = file_name.FilenameWithoutDirectory().ToLower();
	return base == U"eboot.bin" || base == U"main.elf" || base == U"eboot.elf";
}

Vector<String> DiscoverCandidates(const String& guest_root)
{
	Vector<String> out;
	if (guest_root.IsEmpty())
	{
		return out;
	}

	String root = guest_root;
	// Normalize trailing slash.
	if (!root.EndsWith(U"/") && !root.EndsWith(U"\\"))
	{
		root = root + U"/";
	}

	const char* subdirs[] = {"sce_module/", "Media/Modules/", "modules/"};
	for (const char* sub : subdirs)
	{
		const String dir = root + String::FromUtf8(sub);
		if (!File::IsDirectoryExisting(dir))
		{
			continue;
		}
		const auto entries = File::GetDirEntries(dir);
		for (const auto& entry : entries)
		{
			const String name = entry.name;
			if (name.IsEmpty() || name == U"." || name == U"..")
			{
				continue;
			}
			if (!entry.is_file)
			{
				continue;
			}
			if (!HasModuleExtension(name))
			{
				continue;
			}
			if (IsMainExecutableName(name))
			{
				continue;
			}
			const String full = dir + name;
			if (!File::IsFileExisting(full))
			{
				continue;
			}
			// Dedup by full path.
			bool seen = false;
			for (const auto& existing : out)
			{
				if (existing == full)
				{
					seen = true;
					break;
				}
			}
			if (!seen)
			{
				out.Add(full);
			}
		}
	}
	return out;
}

int PreloadInto(RuntimeLinker* rt, const String& main_program_path)
{
	if (rt == nullptr)
	{
		return 0;
	}
	if (!Core::BringUp::AllowPrxPreload())
	{
		return 0;
	}

	const String guest_root = main_program_path.DirectoryWithoutFilename();
	const auto   candidates = DiscoverCandidates(guest_root);
	Core::BringUp::NotePrxPreloadCandidates(static_cast<uint32_t>(candidates.Size()), 0);

	if (candidates.Size() == 0)
	{
		std::printf("KYTY_BRINGUP: prx_preload: no neighbor modules under %s\n", guest_root.C_Str());
		return 0;
	}

	std::printf("KYTY_BRINGUP: prx_preload: discovered %u candidate(s) next to %s\n",
	            static_cast<unsigned>(candidates.Size()), main_program_path.C_Str());

	int loaded = 0;
	for (const auto& path : candidates)
	{
		if (rt->HasProgramFile(path))
		{
			continue;
		}

		// Soft validity check — never hard-abort the process on a bad neighbor.
		Elf64 probe;
		probe.Open(path);
		if (!probe.IsValid())
		{
			std::printf("KYTY_BRINGUP: prx_preload: skip invalid ELF %s\n", path.C_Str());
			continue;
		}
		if (!probe.IsShared())
		{
			// Neighbor preload is for shared modules only; skip executables.
			std::printf("KYTY_BRINGUP: prx_preload: skip non-shared %s\n", path.C_Str());
			continue;
		}

		// LoadProgram will parse exports into the linker program list so later
		// Resolve prefers real exports over Func stubs.
		std::printf("KYTY_BRINGUP: prx_preload: loading %s\n", path.C_Str());
		Program* p = rt->LoadProgram(path);
		if (p != nullptr)
		{
			// Shared modules should not fail the whole process on unresolved globals.
			p->fail_if_global_not_resolved = false;
			++loaded;
		}
	}

	Core::BringUp::NotePrxPreloadCandidates(static_cast<uint32_t>(candidates.Size()),
	                                        static_cast<uint32_t>(loaded));
	std::printf("KYTY_BRINGUP: prx_preload: loaded %d / %u\n", loaded,
	            static_cast<unsigned>(candidates.Size()));
	return loaded;
}

} // namespace Kyty::Loader::NeighborModulePreload

#endif // KYTY_EMU_ENABLED
