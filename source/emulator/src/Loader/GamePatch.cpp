#include "Emulator/Loader/GamePatch.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/GuestCodeRanges.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

namespace {

constexpr uint64_t kGuestPageSize = 0x4000;
constexpr uint64_t kHostPageSize  = 0x1000;

bool IsAllZero(const std::vector<uint8_t>& bytes)
{
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0u; });
}

bool ParseHexBytes(const std::string& text, std::vector<uint8_t>* bytes)
{
	if (text.empty() || (text.size() % 2u) != 0u ||
	    text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
	{
		return false;
	}
	bytes->resize(text.size() / 2u);
	for (size_t i = 0; i < bytes->size(); i++)
	{
		(*bytes)[i] = static_cast<uint8_t>(std::strtoul(text.substr(i * 2u, 2u).c_str(), nullptr, 16));
	}
	return true;
}

bool ParseOffset(const std::string& text, uint64_t* offset)
{
	char* end = nullptr;
	errno     = 0;
	*offset   = std::strtoull(text.c_str(), &end, 16);
	return !text.empty() && errno == 0 && end == text.c_str() + text.size();
}

bool ParseWrite(const Core::Json& entry, GamePatchWrite* write)
{
	return entry.IsObject() && ParseOffset(entry.GetString("offset").C_Str(), &write->offset) &&
	       ParseHexBytes(entry.GetString("off").C_Str(), &write->off) && ParseHexBytes(entry.GetString("on").C_Str(), &write->on) &&
	       write->off.size() == write->on.size();
}

Core::String ParseMods(const Core::Json& mods, GamePatchPlan* plan)
{
	for (const auto* mod: mods.ToArray())
	{
		if (!mod->IsObject() || mod->GetItem("memory") == nullptr || !mod->GetItem("memory")->IsArray())
		{
			return U"a mod has no memory list";
		}
		if (!mod->GetBool("enabled", true))
		{
			continue;
		}
		for (const auto* entry: mod->GetItem("memory")->ToArray())
		{
			GamePatchWrite write;
			if (!ParseWrite(*entry, &write))
			{
				return Core::String(U"invalid memory entry in mod: ") + mod->GetString("name");
			}
			plan->writes.push_back(std::move(write));
		}
		plan->mods.push_back(mod->GetString("name"));
	}
	return {};
}

std::string BaseName(const Core::String& path)
{
	const std::string text = path.C_Str();
	return text.substr(text.find_last_of("/\\") + 1u);
}

bool ProgramContains(const Program& program, uint64_t address, uint64_t size)
{
	return address >= program.base_vaddr && address + size <= program.base_vaddr + program.base_size;
}

// The first write whose expected bytes are not at the address the base gives it, or null.
const GamePatchWrite* FirstMismatch(const GamePatchPlan& plan, const Program& program, uint64_t source_base)
{
	for (const auto& write: plan.writes)
	{
		const uint64_t address = program.base_vaddr + write.offset - source_base;
		// Zero expected bytes declare room for added code; they are not checked.
		if (!IsAllZero(write.off) && (!ProgramContains(program, address, write.off.size()) ||
		                              std::memcmp(reinterpret_cast<const void*>(address), write.off.data(), write.off.size()) != 0))
		{
			return &write;
		}
	}
	return nullptr;
}

// The offsets in a file count from an image base of its own choosing, so the first
// write with known contents finds that base and every other known write confirms it.
bool FindSourceBase(const GamePatchPlan& plan, const Program& program, uint64_t* source_base, const GamePatchWrite** mismatch)
{
	const auto anchor = std::find_if(plan.writes.begin(), plan.writes.end(),
	                                 [](const GamePatchWrite& write) { return !IsAllZero(write.off); });
	if (anchor == plan.writes.end())
	{
		return false;
	}
	*mismatch        = &*anchor;
	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_type != PT_LOAD && phdr[i].p_type != PT_OS_RELRO)
		{
			continue;
		}
		const auto* begin = reinterpret_cast<const uint8_t*>(program.base_vaddr + phdr[i].p_vaddr);
		const auto* end   = begin + phdr[i].p_filesz;
		for (const auto* found = std::search(begin, end, anchor->off.begin(), anchor->off.end()); found != end;
		     found             = std::search(found + 1, end, anchor->off.begin(), anchor->off.end()))
		{
			const uint64_t rva = static_cast<uint64_t>(found - begin) + phdr[i].p_vaddr;
			if (anchor->offset < rva)
			{
				continue;
			}
			*source_base = anchor->offset - rva;
			*mismatch    = FirstMismatch(plan, program, *source_base);
			if (*mismatch == nullptr)
			{
				return true;
			}
		}
	}
	return false;
}

// Mods add code in unused memory around the image, at addresses they fix themselves.
void AllocateCaves(const GamePatchPlan& plan, const Program& program, uint64_t source_base)
{
	std::vector<uint64_t> pages;
	for (const auto& write: plan.writes)
	{
		const uint64_t address = program.base_vaddr + write.offset - source_base;
		if (ProgramContains(program, address, write.on.size()))
		{
			continue;
		}
		for (uint64_t page = address & ~(kGuestPageSize - 1u); page < address + write.on.size(); page += kGuestPageSize)
		{
			if (std::find(pages.begin(), pages.end(), page) == pages.end())
			{
				pages.push_back(page);
			}
		}
	}
	for (const uint64_t page: pages)
	{
		const uint64_t got = Core::VirtualMemory::Alloc(page, kGuestPageSize, Core::VirtualMemory::Mode::ExecuteReadWrite);
		if (got != page)
		{
			EXIT("game patch: address 0x%016" PRIx64 " for added code is in use\n", page);
		}
		Emulator::GuestCodeRanges::Add(page, page + kGuestPageSize);
	}
}

void WriteBytes(uint64_t address, const std::vector<uint8_t>& bytes)
{
	const uint64_t end = address + bytes.size();
	for (uint64_t cursor = address; cursor < end;)
	{
		const uint64_t page = cursor & ~(kHostPageSize - 1u);
		const uint64_t next = std::min(page + kHostPageSize, end);
		Core::VirtualMemory::Mode old_mode {};
		EXIT_IF(!Core::VirtualMemory::Protect(page, kHostPageSize, Core::VirtualMemory::Mode::ExecuteReadWrite, &old_mode));
		std::memcpy(reinterpret_cast<void*>(cursor), bytes.data() + (cursor - address), next - cursor);
		EXIT_IF(!Core::VirtualMemory::Protect(page, kHostPageSize, old_mode));
		cursor = next;
	}
	EXIT_IF(!Core::VirtualMemory::FlushInstructionCache(address, bytes.size()));
}

const Program* FindProcess(const GamePatchPlan& plan, const Vector<Program*>& programs)
{
	for (const auto* program: programs)
	{
		if (program != nullptr && program->elf != nullptr && Core::String::FromUtf8(BaseName(program->file_name).c_str()).EqualNoCase(plan.process))
		{
			return program;
		}
	}
	return nullptr;
}

} // namespace

Core::String GamePatchParse(const Core::String& text, GamePatchPlan* plan)
{
	EXIT_IF(plan == nullptr);
	const auto* root = Core::Json::Create(text);
	if (root == nullptr)
	{
		return Core::String(U"not valid JSON: ") + Core::Json::GetError();
	}
	if (!root->IsObject() || root->GetItem("mods") == nullptr || !root->GetItem("mods")->IsArray())
	{
		delete root;
		return U"not a mods file";
	}
	plan->title_id = root->GetString("id");
	plan->version  = root->GetString("version");
	plan->process  = root->GetString("process");
	auto error     = ParseMods(*root->GetItem("mods"), plan);
	delete root;
	return error;
}

void GamePatchApply(const Core::String& file_name, const Vector<Program*>& programs)
{
	if (!Core::File::IsFileExisting(file_name))
	{
		EXIT("game patch %s: file not found\n", file_name.C_Str());
	}
	GamePatchPlan plan;
	const auto    error = GamePatchParse(Core::File::Read(file_name, Core::File::Encoding::Utf8), &plan);
	if (!error.IsEmpty())
	{
		EXIT("game patch %s: %s\n", file_name.C_Str(), error.C_Str());
	}

	Core::String title_id;
	Core::String version;
	if (!SystemContentGetMetadata(&title_id, &version) || !title_id.EqualNoCase(plan.title_id) || version != plan.version)
	{
		EXIT("game patch %s is for %s %s, the game is %s %s\n", file_name.C_Str(), plan.title_id.C_Str(), plan.version.C_Str(),
		     title_id.C_Str(), version.C_Str());
	}
	const auto* program = FindProcess(plan, programs);
	if (program == nullptr)
	{
		EXIT("game patch %s: module %s is not loaded\n", file_name.C_Str(), plan.process.C_Str());
	}
	uint64_t              source_base = 0;
	const GamePatchWrite* mismatch    = nullptr;
	if (!plan.writes.empty() && !FindSourceBase(plan, *program, &source_base, &mismatch))
	{
		EXIT("game patch %s: the bytes at offset 0x%016" PRIx64 " are not in %s\n", file_name.C_Str(),
		     mismatch != nullptr ? mismatch->offset : 0u, plan.process.C_Str());
	}

	AllocateCaves(plan, *program, source_base);
	for (const auto& write: plan.writes)
	{
		WriteBytes(program->base_vaddr + write.offset - source_base, write.on);
	}
	for (const auto& name: plan.mods)
	{
		emu_printf("Game patch applied: %s\n", name.C_Str());
	}
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
