#include "Emulator/Loader/RuntimeLinker.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Sys/SysDbg.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/Loader/Jit.h"
#include "Emulator/Loader/MissingImport.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Profiler.h"
#include "Emulator/Validation/DomainValidators.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel {
void SetProgName(const String& name);
} // namespace Kyty::Libs::LibKernel

namespace Kyty::Loader {

Vector<uint32_t> LoaderBuildModuleStartOrder(const Vector<ModuleStartDescriptor>& modules)
{
	Vector<uint32_t> order;
	Vector<uint8_t>  state(modules.Size());

	auto find_dependency = [&modules](const String& name) -> uint32_t {
		for (uint32_t i = 0; i < modules.Size(); i++)
		{
			if ((!modules[i].so_name.IsEmpty() && modules[i].so_name == name) || modules[i].file_name == name)
			{
				return i;
			}
		}
		return Vector<uint32_t>::INVALID_INDEX;
	};

	std::function<void(uint32_t)> visit = [&](uint32_t index) {
		if (state[index] == 2)
		{
			return;
		}
		if (state[index] == 1)
		{
			// Keep cyclic dependency groups stable; each member is still started once.
			return;
		}

		state[index] = 1;
		for (const auto& dependency: modules[index].needed)
		{
			const uint32_t dependency_index = find_dependency(dependency);
			if (dependency_index != Vector<uint32_t>::INVALID_INDEX)
			{
				visit(dependency_index);
			}
		}
		state[index] = 2;
		order.Add(index);
	};

	// Seed CRT providers before the generic walk. Some game PRXes import libc
	// symbols without listing libc.prx in DT_NEEDED; starting them first still
	// leaves the BSS mspace uninitialized for their constructors.
	for (uint32_t i = 0; i < modules.Size(); i++)
	{
		if (modules[i].file_name == U"libc.prx" || modules[i].so_name == U"libc.prx" ||
		    modules[i].file_name == U"libSceLibcInternal.sprx" || modules[i].so_name == U"libSceLibcInternal.sprx")
		{
			visit(i);
		}
	}

	for (uint32_t i = 0; i < modules.Size(); i++)
	{
		visit(i);
	}
	return order;
}

bool LoaderDecodeEhFrameHeader(const uint8_t* header, size_t header_size, uint64_t header_addr, uint64_t readable_end,
                               EhFrameInfo* out_info)
{
	if (header == nullptr || header_size < 8 || out_info == nullptr || header[0] != 1 || header[1] != 0x1b)
	{
		return false;
	}

	int32_t relative = 0;
	std::memcpy(&relative, header + 4, sizeof(relative));

	if (header_addr > std::numeric_limits<uint64_t>::max() - 4)
	{
		return false;
	}

	const uint64_t relative_base = header_addr + 4;
	uint64_t       frame_addr    = 0;
	if (relative >= 0)
	{
		const auto offset = static_cast<uint64_t>(relative);
		if (relative_base > std::numeric_limits<uint64_t>::max() - offset)
		{
			return false;
		}
		frame_addr = relative_base + offset;
	} else
	{
		const auto offset = static_cast<uint64_t>(-static_cast<int64_t>(relative));
		if (relative_base < offset)
		{
			return false;
		}
		frame_addr = relative_base - offset;
	}

	if (frame_addr >= readable_end)
	{
		return false;
	}

	out_info->header_addr = header_addr;
	out_info->frame_addr  = frame_addr;
	out_info->frame_size  = readable_end - frame_addr;
	return true;
}

// Missing-import registry, static StubAllocator, ImportPolicy, and diagnostics
// live in MissingImport.{h,cpp}. RuntimeLinker::Resolve only coordinates:
// validate → allocator policy or export → missing-import policy.

#pragma pack(1)

struct EntryParams
{
	int         argc;
	uint32_t    pad;
	const char* argv[3];
};

#pragma pack()

using atexit_func_t          = KYTY_SYSV_ABI void (*)();
using entry_func_t           = KYTY_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp, module_func_t func);

enum class BindType
{
	Unknown,
	Local,
	Global,
	Weak
};

struct RelocationInfo
{
	bool       resolved   = false;
	BindType   bind       = BindType::Unknown;
	SymbolType type       = SymbolType::Unknown;
	uint64_t   value      = 0;
	uint64_t   vaddr      = 0;
	uint64_t   base_vaddr = 0;
	String     name;
	String     dbg_name;
	bool       bind_self = false;
};

// The structure will be passed via the stack
// since the size of an object is larger than 16 bytes
struct RelocateHandlerStack
{
	uint64_t stack[3];
};

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t INVALID_OFFSET   = 0x040000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;
constexpr uint64_t INVALID_MEMORY   = SYSTEM_RESERVED + INVALID_OFFSET;

constexpr size_t   XSAVE_BUFFER_SIZE = 2688;
constexpr uint64_t XSAVE_CHK_GUARD   = 0xDeadBeef5533CCAAu;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
static uint64_t g_invalid_memory    = 0;
static uint64_t g_next_tls_module_id = 1;

static Program* g_tls_main_program = nullptr;
alignas(64) static uint8_t g_tls_reg_save_area[XSAVE_BUFFER_SIZE + sizeof(XSAVE_CHK_GUARD)];
static uint8_t g_tls_spinlock = 0;

static KYTY_SYSV_ABI void run_entry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func)
{
	GuestCall::Invoke(addr, reinterpret_cast<uint64_t>(params), reinterpret_cast<uint64_t>(atexit_func), 0);
}

static KYTY_SYSV_ABI int run_ini_fini(uint64_t addr, size_t args, const void* argp, module_func_t func)
{
	return static_cast<int>(GuestCall::Invoke(addr, args, reinterpret_cast<uint64_t>(argp), reinterpret_cast<uint64_t>(func)));
}

static void run_init_array(uint64_t base_vaddr, uint64_t array_vaddr, uint64_t array_size)
{
	if (array_vaddr == 0 || array_size < sizeof(uint64_t))
	{
		return;
	}

	auto*      entries = reinterpret_cast<uint64_t*>(base_vaddr + array_vaddr);
	const auto count   = static_cast<size_t>(array_size / sizeof(uint64_t));

	for (size_t i = 0; i < count; i++)
	{
		const uint64_t fn = entries[i];
		if (fn != 0)
		{
			GuestCall::Invoke(fn, 0, 0, 0);
		}
	}
}

void LoaderRunProgramInitializers(uint64_t base_vaddr, const DynamicInfo& info)
{
	if (info.preinit_array_vaddr != 0 && info.preinit_array_size != 0)
	{
		run_init_array(base_vaddr, info.preinit_array_vaddr, info.preinit_array_size);
	}

	if (info.init_vaddr != 0)
	{
		GuestCall::Invoke(base_vaddr + info.init_vaddr, 0, 0, 0);
	}

	if (info.init_array_vaddr != 0 && info.init_array_size != 0)
	{
		run_init_array(base_vaddr, info.init_array_vaddr, info.init_array_size);
	}
}

bool LoaderCodeContainsDirectCallTo(const uint8_t* code, uint64_t size, uint64_t code_vaddr, uint64_t target_vaddr)
{
	if (code == nullptr || size < 5)
	{
		return false;
	}

	for (uint64_t off = 0; off + 5 <= size; off++)
	{
		if (code[off] != 0xe8)
		{
			continue;
		}
		int32_t rel = 0;
		std::memcpy(&rel, code + off + 1, sizeof(rel));
		const uint64_t next = code_vaddr + off + 5;
		const uint64_t dest = static_cast<uint64_t>(static_cast<int64_t>(next) + static_cast<int64_t>(rel));
		if (dest == target_vaddr)
		{
			return true;
		}
	}
	return false;
}

static uint64_t get_aligned_size(const Elf64_Phdr* p)
{
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void dbg_dump_symbols(const String& folder, Elf64_Sym* symbols, uint64_t size, const char* names)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols; reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++)
	{
		f.Printf("----\n");
		f.Printf("st_name = %" PRIu32 ", %s\n", sym->st_name, names + sym->st_name);
		f.Printf("st_info = 0x%02" PRIx8 "\n", sym->st_info);
		f.Printf("st_other = 0x%02" PRIx8 "\n", sym->st_other);
		f.Printf("st_shndx = 0x%04" PRIx16 "\n", sym->st_shndx);
		f.Printf("st_value = 0x%016" PRIx64 "\n", sym->st_value);
		f.Printf("st_size = %" PRIu64 "\n", sym->st_size);
	}

	f.Close();
}

static void dbg_dump_rela(const String& folder, Elf64_Rela* records, uint64_t size, const char* /*names*/, const char* file_name)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records; reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++)
	{
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Core::VirtualMemory::Mode get_mode(Elf64_Word flags)
{
	switch (flags)
	{
		case PF_R: return Core::VirtualMemory::Mode::Read;
		case PF_W: return Core::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Core::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Core::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_R: return Core::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Core::VirtualMemory::Mode::ExecuteReadWrite;
		case PF_X | PF_W | PF_R: return Core::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Core::VirtualMemory::Mode::NoAccess;
	}
}

struct FrameS
{
	FrameS*   next;
	uintptr_t ret_addr;
};

static void KYTY_SYSV_ABI stackwalk_x86(uint64_t rbp, void** stack, int* depth, uintptr_t stack_addr, size_t stack_size,
                                        uintptr_t code_addr, size_t code_size)
{
	auto* frame = reinterpret_cast<FrameS*>(rbp);

	int d = *depth;
	int i = 0;

	for (; i < d; i++)
	{
		if (!(reinterpret_cast<uintptr_t>(frame) >= stack_addr && reinterpret_cast<uintptr_t>(frame) < stack_addr + stack_size))
		{
			break;
		}

		if (!(frame->ret_addr >= code_addr && frame->ret_addr < code_addr + code_size))
		{
			break;
		}

		stack[i] = reinterpret_cast<void*>(frame->ret_addr);

		frame = frame->next;
	}

	*depth = i;
}

void KYTY_SYSV_ABI sys_stack_walk_x86(uint64_t rbp, void** stack, int* depth)
{
	stackwalk_x86(rbp, stack, depth, 0, UINT64_MAX, SYSTEM_RESERVED + CODE_BASE_OFFSET,
	              g_desired_base_addr - (SYSTEM_RESERVED + CODE_BASE_OFFSET));
}

static void kyty_exception_handler(const Core::VirtualMemory::ExceptionHandler::ExceptionInfo* info)
{
	// This runs in a signal handler and MUST stay async-signal-safe on the common
	// path: the GPU memory watcher faults on every write to tracked graphics memory,
	// and calling printf/malloc here (as the old debug print did) dead-locks against
	// the allocator lock the faulting guest code may already hold. Handle the GPU
	// write case first and return without allocating; only the fatal paths below
	// (which terminate the process anyway) use printf.
	if (info->type == Core::VirtualMemory::ExceptionHandler::ExceptionType::AccessViolation)
	{
		// On macOS ARM/Rosetta, access-type bits are not always reliable.
		// Attempt GPU watch handling for any access violation before treating it
		// as fatal; this preserves write handling when the signal classification
		// can only be inferred from the faulting context.
		if (Libs::Graphics::GpuMemoryCheckAccessViolation(info->access_violation_vaddr))
		{
			return;
		}

		// Lazily back a reserved flexible-memory page the guest just touched.
		if (Core::VirtualMemory::TryDemandMap(info->access_violation_vaddr))
		{
			return;
		}

		// Real access violation. Report async-signal-safe and terminate: this runs in
		// a signal handler and the faulting thread may hold the allocator lock, so
		// printf/StackTrace (which allocate) would dead-lock instead of reporting.
		Core::VirtualMemory::FatalFault(info->access_violation_vaddr, info->exception_address);
	}

	Core::VirtualMemory::FatalFault(0, info->exception_address);
}

static void encode_id_64(uint16_t in_id, String* out_id)
{
	static const char32_t* str = U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u)
	{
		*out_id += str[in_id];
	} else
	{
		if (in_id < 0x1000u)
		{
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else
		{
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void get_dyn_data_os(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void get_dyn_data(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void get_dyn_value(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = dyn->d_un.d_val;
	}
}

template <class T>
static void get_dyn_values(Elf64* elf, T* out, Elf64_Sxword tag)
{
	for (const auto* dyn: elf->GetDynList(tag))
	{
		out->Add(dyn->d_un.d_val);
	}
}

template <class T>
static void get_dyn_ptr(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = dyn->d_un.d_ptr;
	}
}

static void KYTY_SYSV_ABI ProgramExitHandler()
{
	Core::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	printf("exit!!!\n");
}

template <class T>
static void get_dyn_modules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag)
{
	Vector<uint64_t> needed_modules;
	get_dyn_values(elf, &needed_modules, tag);
	for (auto need: needed_modules)
	{
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		encode_id_64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->Add(id);
	}
}

template <class T>
static void get_dyn_libs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag)
{
	Vector<uint64_t> needed_modules;
	get_dyn_values(elf, &needed_modules, tag);
	for (auto need: needed_modules)
	{
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		encode_id_64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->Add(id);
	}
}

uint64_t LoaderTlsRelocationValue(uint32_t relocation_type, uint64_t module_id, uint64_t symbol_offset, int64_t addend,
                                  uint64_t static_tls_size)
{
	const uint64_t offset = symbol_offset + static_cast<uint64_t>(addend);

	switch (relocation_type)
	{
		case R_X86_64_DTPMOD64: return module_id;
		case R_X86_64_DTPOFF64: return offset;
		case R_X86_64_TPOFF64: return offset - static_tls_size;
		default: EXIT("unknown TLS relocation type: %u\n", relocation_type);
	}
	return 0;
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program)
{
	KYTY_PROFILER_FUNCTION();

	// KYTY_PROFILER_BLOCK("1");

	RelocationInfo ret;
	// SymbolRecord   sr {};

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("2");

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("3");

	switch (type)
	{
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64:
		{
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type)
			{
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			switch (bind)
			{
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK:
				{
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, ret.type, program, &sr, &ret.bind_self);
					symbol_vaddr = sr.vaddr;
				}
				break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name;
			ret.dbg_name = sr.dbg_name;
		}
		break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
		case R_X86_64_DTPOFF64:
		case R_X86_64_TPOFF64:
		{
			uint64_t symbol_offset = 0;
			if (symbol != 0)
			{
				symbol_offset = symbols[symbol].st_value;
			}
			EXIT_IF(program->tls.module_id == 0);
			ret.value = LoaderTlsRelocationValue(type, program->tls.module_id, symbol_offset, addend, program->tls.static_offset);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = program->file_name;
			break;
		}
		default: EXIT("unknown type: %d\n", (int)type);
	}

	// KYTY_PROFILER_END_BLOCK;

	return ret;
}

static void relocate(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table)
{
	KYTY_PROFILER_FUNCTION();

	auto ri = GetRelocationInfo(r, program);

	[[maybe_unused]] bool patched = false;

	// KYTY_PROFILER_BLOCK("patch");

	if (ri.resolved)
	{
		patched = Core::VirtualMemory::PatchReplace(ri.vaddr, ri.value);
	} else
	{
		uint64_t value = 0;
		bool     weak  = (ri.bind == BindType::Weak || !program->fail_if_global_not_resolved);
		if (ri.type == SymbolType::Object && weak)
		{
			value = g_invalid_memory;
		} else if (ri.type == SymbolType::Func && jmprela_table && weak)
		{
			if (program->custom_call_plt_vaddr != 0)
			{
				EXIT_NOT_IMPLEMENTED(index >= program->custom_call_plt_num);
				value = reinterpret_cast<Jit::CallPlt*>(program->custom_call_plt_vaddr)->GetAddr(index);
			} else
			{
				value = RuntimeLinker::ReadFromElf(program, ri.vaddr) + ri.base_vaddr;
			}
		} else if ((ri.type == SymbolType::Func && !jmprela_table && weak) || (ri.type == SymbolType::NoType && weak))
		{
			value = RuntimeLinker::ReadFromElf(program, ri.vaddr) + ri.base_vaddr;
		}

		if (value != 0)
		{
			patched = Core::VirtualMemory::PatchReplace(ri.vaddr, value);
		} else
		{
			auto dbg_str = String::FromPrintf("[%016" PRIx64 "] <- %s%016" PRIx64 "%s, %s, %s, %s, %s", ri.vaddr,
			                                  ri.value == 0 ? FG_BRIGHT_RED : FG_BRIGHT_GREEN, ri.value, DEFAULT, ri.name.C_Str(),
			                                  Core::EnumName(ri.type).C_Str(), Core::EnumName(ri.bind).C_Str(), ri.dbg_name.C_Str());

			{
				const String clean = Log::IsColoredPrintf() ? dbg_str : Log::RemoveColors(dbg_str);
				Emulator::Agent::Lifecycle::EmitRelocationFailure(clean.C_Str());
				EXIT("Can't resolve: %s\n", clean.C_Str());
			}
		}
	}

	// KYTY_PROFILER_END_BLOCK;

	if (program->dbg_print_reloc)
	{
		if (/* !dbg_str.ContainsStr(U"libc_") && */ patched && !ri.bind_self &&
		    (ri.bind == BindType::Global || ri.bind == BindType::Weak || ri.type == SymbolType::TlsModule))
		{
			auto dbg_str = String::FromPrintf("[%016" PRIx64 "] <- %s%016" PRIx64 "%s, %s, %s, %s, %s", ri.vaddr,
			                                  ri.value == 0 ? FG_BRIGHT_RED : FG_BRIGHT_GREEN, ri.value, DEFAULT, ri.name.C_Str(),
			                                  Core::EnumName(ri.type).C_Str(), Core::EnumName(ri.bind).C_Str(), ri.dbg_name.C_Str());

			printf("Relocate: %s\n", dbg_str.C_Str());
		}
	}
}

static void relocate_all(Elf64_Rela* records, uint64_t size, Program* program, bool jmprela_table)
{
	KYTY_PROFILER_FUNCTION();

	uint32_t index = 0;
	for (auto* r = records; reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++, index++)
	{
		relocate(index, r, program, jmprela_table);
	}
}

static KYTY_SYSV_ABI void RelocateHandler(uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx, uint64_t r8, uint64_t r9,
                                          RelocateHandlerStack s)
{
	auto*  stack          = s.stack;
	auto*  program        = reinterpret_cast<Program*>(stack[-1]);
	auto   rel_index      = stack[0];
	auto   return_address = stack[1];
	String name           = U"<unknown function>";

	if (program != nullptr && program->dynamic_info != nullptr && program->dynamic_info->jmprela_table != nullptr)
	{
		auto ri = GetRelocationInfo(program->dynamic_info->jmprela_table + rel_index, program);

		name = String::FromPrintf(FG_BRIGHT_RED "%s" DEFAULT, ri.name.C_Str());
	}

	// Restore return address (for stack trace)
	stack[-1] = stack[1];

	Core::Singleton<Loader::RuntimeLinker>::Instance()->StackTrace(reinterpret_cast<uint64_t>(s.stack - 2));
	// Never touch return_address memory here: an unmapped page would SIGSEGV
	// before EXIT could report the missing NID (silent guest close).
	static constexpr uint8_t kCallerBytesUnknown[16] = {};

	EXIT("=== Unpatched function!!! ===\n[%d]\t%s\n"
	     "SysV arguments: rdi=%016" PRIx64 " rsi=%016" PRIx64 " rdx=%016" PRIx64 " rcx=%016" PRIx64 " r8=%016" PRIx64 " r9=%016" PRIx64
	     " return=%016" PRIx64 "\n"
	     "Caller bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	     Core::Thread::GetThreadIdUnique(), (Log::IsColoredPrintf() ? name : Log::RemoveColors(name)).C_Str(), rdi, rsi, rdx, rcx, r8, r9,
	     return_address, kCallerBytesUnknown[0], kCallerBytesUnknown[1], kCallerBytesUnknown[2], kCallerBytesUnknown[3],
	     kCallerBytesUnknown[4], kCallerBytesUnknown[5], kCallerBytesUnknown[6], kCallerBytesUnknown[7], kCallerBytesUnknown[8],
	     kCallerBytesUnknown[9], kCallerBytesUnknown[10], kCallerBytesUnknown[11], kCallerBytesUnknown[12], kCallerBytesUnknown[13],
	     kCallerBytesUnknown[14], kCallerBytesUnknown[15]);
}

static KYTY_MS_ABI uint8_t* TlsMainGetAddr()
{
	EXIT_IF(g_tls_main_program == nullptr);

	if (memcmp(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD)) != 0)
	{
		EXIT("xsave buffer is too small\n");
	}

	return RuntimeLinker::TlsGetAddr(g_tls_main_program) + g_tls_main_program->tls.image_size;
}

uint64_t LoaderRewriteTlsGdCallRexPrefix(uint8_t* code, uint64_t size)
{
	if (code == nullptr || size < 8)
	{
		return 0;
	}

	// SysV TLS GD call half is "data16 data16 rex64 call rel32":
	//   66 66 48 e8 xx xx xx xx
	// Some PS5 images encode the REX.W as a third 0x66 instead:
	//   66 66 66 e8 xx xx xx xx
	// That form is executed as a 16-bit near call on host CPUs (return push of
	// 2 bytes, IP low 16-bit wrap to ~0x3ffe). Restore REX.W.
	uint64_t       rewritten = 0;
	const uint64_t last      = size - 8;
	for (uint64_t i = 0; i <= last; i++)
	{
		if (code[i] == 0x66 && code[i + 1] == 0x66 && code[i + 2] == 0x66 && code[i + 3] == 0xe8)
		{
			code[i + 2] = 0x48;
			rewritten++;
			i += 7; // skip remainder of this 8-byte site
		}
	}
	return rewritten;
}

uint64_t LoaderPatchTlsFsBaseLoads(uint8_t* code, uint64_t size, uint64_t handler_vaddr)
{
	if (code == nullptr || size < Jit::Call9::GetSize() || handler_vaddr == 0)
	{
		return 0;
	}

	// Replace:
	//   [66 ...] mov rax, qword ptr fs:[0x00]
	// with:
	//   call <handler>
	//   mov rax,rax
	//   nop
	const uint8_t tls_pattern[9] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
	constexpr uint64_t max_data16_prefixes = 3;

	EXIT_IF(Jit::Call9::GetSize() != sizeof(tls_pattern));

	uint64_t patched = 0;
	for (uint64_t i = 0; i + Jit::Call9::GetSize() <= size; i++)
	{
		uint64_t prefix_count = 0;
		while (i + prefix_count < size && code[i + prefix_count] == 0x66)
		{
			prefix_count++;
		}
		if (prefix_count > max_data16_prefixes)
		{
			i += prefix_count - 1;
			continue;
		}

		const uint64_t instruction_size = prefix_count + sizeof(tls_pattern);
		if (i + instruction_size > size || std::memcmp(code + i + prefix_count, tls_pattern, sizeof(tls_pattern)) != 0)
		{
			continue;
		}

		auto* call = new (code + i) Jit::Call9;
		call->SetFunc(handler_vaddr);
		for (uint64_t n = Jit::Call9::GetSize(); n < instruction_size; n++)
		{
			code[i + n] = 0x90;
		}
		patched++;
		i += instruction_size - 1;
	}
	return patched;
}

uint64_t LoaderPrepareThreadTlsImage(uint8_t* tls, uint64_t image_size, uint64_t template_vaddr, uint64_t program_base,
                                     uint64_t program_size, bool (*guest_read64)(uint64_t addr, uint64_t* out, void* ctx), void* guest_ctx)
{
	if (tls == nullptr || image_size < sizeof(uint64_t))
	{
		return 0;
	}

	const uint64_t tmpl_lo  = template_vaddr;
	const uint64_t tmpl_hi  = template_vaddr + image_size;
	const uint64_t tls_base = reinterpret_cast<uint64_t>(tls);
	const uint64_t prog_lo  = program_base;
	const uint64_t prog_hi  = program_base + program_size;
	// Context layout used by guest ensure: buffer control pointer at +0x3e0.
	constexpr uint64_t kContextBufferControlOffset = 0x3e0;

	uint64_t modified = 0;
	for (uint64_t off = 0; off + sizeof(uint64_t) <= image_size; off += sizeof(uint64_t))
	{
		auto*          cell = reinterpret_cast<uint64_t*>(tls + off);
		const uint64_t v    = *cell;
		if (v == 0)
		{
			continue;
		}

		// Absolute self-pointer into the PT_TLS template → this thread's copy.
		if (v >= tmpl_lo && v < tmpl_hi)
		{
			*cell = tls_base + (v - tmpl_lo);
			modified++;
			continue;
		}

		// Absolute pointer into the main program image.
		if (guest_read64 == nullptr || v < prog_lo || v > prog_hi - sizeof(uint64_t))
		{
			continue;
		}

		uint64_t word0 = 1;
		if (!guest_read64(v, &word0, guest_ctx))
		{
			continue;
		}

		// Static type/descriptor blobs (observed at TLS +0x70/+0x78 in Gen5):
		// word0 = small size, word1 = small refcount, word2+ = function
		// pointers into guest code. They must not occupy "current context"
		// TLS slots — guest SET asserts s_pTls* == nullptr first.
		if (word0 >= 0x20 && word0 < 0x1000 && (word0 & 7u) == 0 && v + 24 <= prog_hi)
		{
			uint64_t word1 = 0;
			uint64_t word2 = 0;
			if (guest_read64(v + 8, &word1, guest_ctx) && guest_read64(v + 16, &word2, guest_ctx) && word1 < 0x100 && word2 >= prog_lo &&
			    word2 < prog_hi)
			{
				*cell = 0;
				modified++;
				continue;
			}
		}

		// Unconstructed Context (null word0 + null buffer control at +0x3e0).
		if (program_size < kContextBufferControlOffset + sizeof(uint64_t) || v > prog_hi - (kContextBufferControlOffset + sizeof(uint64_t)))
		{
			continue;
		}
		uint64_t buffer = 1;
		if (!guest_read64(v + kContextBufferControlOffset, &buffer, guest_ctx))
		{
			continue;
		}
		if (word0 == 0 && buffer == 0)
		{
			*cell = 0;
			modified++;
		}
	}
	return modified;
}

void LoaderInitializeThreadTlsImage(uint8_t* tls, uint64_t image_size, const uint8_t* template_data, uint64_t init_size)
{
	EXIT_IF(tls == nullptr && image_size != 0);
	EXIT_IF(template_data == nullptr && init_size != 0);
	EXIT_IF(init_size > image_size);

	if (image_size == 0)
	{
		return;
	}

	std::memset(tls, 0, image_size);
	if (init_size != 0)
	{
		std::memcpy(tls, template_data, init_size);
	}
}

static bool TlsGuestRead64(uint64_t addr, uint64_t* out, void* /*ctx*/)
{
	if (out == nullptr || addr == 0)
	{
		return false;
	}
	// Main-image and demand-mapped guest pages are host-addressable at the
	// guest VA after LoadProgramToMemory / VirtualMemory setup.
	*out = *reinterpret_cast<const uint64_t*>(addr);
	return true;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	// Always rewrite broken TLS GD call prefixes on executable segments, even
	// when the TLS handler is not yet published — the relative targets already
	// land on the handler slot Kyty allocates after base_size_aligned.
	if (!program->elf->IsShared() && size >= 8)
	{
		auto*          start_ptr = reinterpret_cast<uint8_t*>(address);
		const uint64_t rex_sites = LoaderRewriteTlsGdCallRexPrefix(start_ptr, size);
		if (rex_sites != 0)
		{
			std::fprintf(stderr, "Patch tls GD call REX.W at segment 0x%016" PRIx64 ": %" PRIu64 " site(s)\n", address, rex_sites);
			printf("Patch tls GD call REX.W at segment 0x%016" PRIx64 ": %" PRIu64 " site(s)\n", address, rex_sites);
		}
	}

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0)
	{
		const uint64_t tls_sites = LoaderPatchTlsFsBaseLoads(reinterpret_cast<uint8_t*>(address), size, program->tls.handler_vaddr);
		if (tls_sites != 0)
		{
			printf("Patch tls at segment 0x%016" PRIx64 ": %" PRIu64 " site(s)\n", address, tls_sites);
		}
	}
}

uint64_t RuntimeLinker::GetEntry()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		if (p->elf != nullptr && !p->elf->IsShared())
		{
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		if (p->elf != nullptr && !p->elf->IsShared())
		{
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const String& folder)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		auto folder_str = folder.FixDirectorySlash();
		folder_str += p->file_name.FilenameWithoutDirectory();

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr)
		{
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr)
			{
				dbg_dump_symbols(folder_str, p->dynamic_info->symbol_table, p->dynamic_info->symbol_table_total_size,
				                 p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr)
			{
				dbg_dump_rela(folder_str, p->dynamic_info->jmprela_table, p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				              "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr)
			{
				dbg_dump_rela(folder_str, p->dynamic_info->rela_table, p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				              "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr)
		{
			p->export_symbols->DbgDump(folder_str, U"export_symbols.txt");
		}
		if (p->import_symbols != nullptr)
		{
			p->import_symbols->DbgDump(folder_str, U"import_symbols.txt");
		}
	}
}

void RuntimeLinker::DbgDumpSymbols(const String& folder)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	if (m_symbols != nullptr)
	{
		m_symbols->DbgDump(folder.FixDirectorySlash(), U"hle_symbols.txt");
	}

	for (const auto* p: m_programs)
	{
		auto folder_str = folder.FixDirectorySlash();
		folder_str += p->file_name.FilenameWithoutDirectory();

		if (p->export_symbols != nullptr)
		{
			p->export_symbols->DbgDump(folder_str, U"export_symbols.txt");
		}
		if (p->import_symbols != nullptr)
		{
			p->import_symbols->DbgDump(folder_str, U"import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		Relocate(p);
	}

	m_relocated = true;
}

void RuntimeLinker::RelocateProgram(Program* program)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(program == nullptr);
	EXIT_IF(!m_programs.Contains(program));

	Relocate(program);
}

void RuntimeLinker::UnloadProgram(Program* program)
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	if (auto index = m_programs.Find(program); m_programs.IndexValid(index))
	{
		DeleteProgram(m_programs.At(index));
		m_programs.RemoveAt(index);
	} else
	{
		EXIT("program not found");
	}

	if (m_relocated)
	{
		RelocateAll();
	}
}

RuntimeLinker::RuntimeLinker(): m_symbols(new SymbolDatabase)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
}

RuntimeLinker::~RuntimeLinker()
{
	Clear();
}

Program* RuntimeLinker::LoadProgram(const String& elf_name)
{
	KYTY_PROFILER_FUNCTION();

	Core::LockGuard lock(m_mutex);

	// validate path shape before any ELF mutation
	const auto elf_name_utf8    = elf_name.utf8_str();
	Emulator::Validation::GuestExecutableRequest greq {};
	greq.root_path          = elf_name_utf8.GetData();
	greq.require_eboot_name = false;
	const auto path_ok      = Emulator::Validation::ValidateGuestExecutable(greq);
	if (!path_ok.Ok())
	{
		EXIT("guest executable validation failed: %s (%s)\n", path_ok.error.reason, path_ok.error.code);
	}

	// Shared modules: validate basename only (no fabricated version fields).
	const String lower = elf_name.ToLower();
	if (lower.EndsWith(U".prx") || lower.EndsWith(U".sprx"))
	{
		const String                                file = elf_name.FilenameWithoutDirectory();
		const String                                base = file.FilenameWithoutExtension();
		Emulator::Validation::ModuleMetadataRequest mreq {};
		const auto                                  base_utf8 = base.utf8_str();
		mreq.name         = base_utf8.GetData();
		const auto mod_ok = Emulator::Validation::ValidateModuleMetadata(mreq);
		if (!mod_ok.Ok())
		{
			EXIT("module metadata validation failed: %s (%s)\n", mod_ok.error.reason, mod_ok.error.code);
		}
	}

	static int32_t id_seq = 0;

	printf("Loading: %s\n", elf_name_utf8.GetData());

	auto* program = new Program;

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;

	program->elf = new Elf64;
	program->elf->Open(elf_name);

	if (program->elf->IsValid())
	{
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		CreateSymbolDatabase(program);
	} else
	{
		EXIT("elf is not valid: %s\n", elf_name_utf8.GetData());
	}

	m_programs.Add(program);

	if (!program->elf->IsShared())
	{
		program->fail_if_global_not_resolved = false;
		Libs::LibKernel::SetProgName(elf_name.FilenameWithoutDirectory());
	}

	if (/*elf_name.FilenameWithoutExtension().EndsWith(U"libc") || elf_name.FilenameWithoutExtension().EndsWith(U"Fios2") ||
	    elf_name.FilenameWithoutExtension().EndsWith(U"Fios2_debug") || elf_name.FilenameWithoutExtension().EndsWith(U"NpToolkit") ||
	    elf_name.FilenameWithoutExtension().EndsWith(U"NpToolkit2") || elf_name.FilenameWithoutExtension().EndsWith(U"JobManager")*/
	    elf_name.DirectoryWithoutFilename().EndsWith(U"_module/", String::Case::Insensitive))
	{
		program->fail_if_global_not_resolved = false;
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const String& elf_name)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared())
		{
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const String& elf_name)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	if (auto index = m_programs.Find(program); m_programs.IndexValid(index))
	{
		EXIT_IF(m_programs.At(index)->elf == nullptr);

		m_programs.At(index)->elf->Save(elf_name);
	} else
	{
		EXIT("program not found");
	}
}

void RuntimeLinker::Execute()
{
	KYTY_PROFILER_THREAD("Thread_Main");

	Libs::LibKernel::PthreadInitSelfForMainThread();

	RelocateAll();
	StartAllModules();

	// After load+reloc the main image is final. Rewrite TLS GD call prefixes
	// on executable LOAD segments only (never the whole base_size — that would
	// re-Protect the GOT/data with the first page's mode and fault PLT loads).
	{
		Core::LockGuard lock(m_mutex);
		for (auto* program: m_programs)
		{
			if (program == nullptr || program->elf == nullptr || program->elf->IsShared() || program->base_vaddr == 0)
			{
				continue;
			}
			const auto* ehdr = program->elf->GetEhdr();
			const auto* phdr = program->elf->GetPhdr();
			if (ehdr == nullptr || phdr == nullptr)
			{
				continue;
			}
			uint64_t total_sites = 0;
			for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
			{
				if (phdr[i].p_memsz == 0 || (phdr[i].p_type != PT_LOAD && phdr[i].p_type != PT_OS_RELRO))
				{
					continue;
				}
				const auto mode = get_mode(phdr[i].p_flags);
				if (!Core::VirtualMemory::IsExecute(mode))
				{
					continue;
				}
				const uint64_t            seg_addr = phdr[i].p_vaddr + program->base_vaddr;
				const uint64_t            seg_size = get_aligned_size(phdr + i);
				Core::VirtualMemory::Mode old_mode {};
				Core::VirtualMemory::Protect(seg_addr, seg_size, Core::VirtualMemory::Mode::ExecuteReadWrite, &old_mode);
				total_sites += LoaderRewriteTlsGdCallRexPrefix(reinterpret_cast<uint8_t*>(seg_addr), seg_size);
				Core::VirtualMemory::Protect(seg_addr, seg_size, old_mode);
				Core::VirtualMemory::FlushInstructionCache(seg_addr, seg_size);
			}
			if (total_sites != 0)
			{
				std::fprintf(stderr, "Patch tls GD call REX.W on main image: %" PRIu64 " site(s)\n", total_sites);
				printf("Patch tls GD call REX.W on main image: %" PRIu64 " site(s)\n", total_sites);
			}
		}
	}

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Execute: " BOLD BG_BLUE "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", "Main");
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Reserve some stack. There may be jumps over guard page. To prevent segfault we need to expand committed area.

	size_t expanded_size = 0;
	size_t expanded_max  = static_cast<size_t>(256) * 1024;

	while (expanded_size < expanded_max)
	{
		sys_dbg_stack_info_t s {};
		sys_stack_usage(s);
		*reinterpret_cast<uint32_t*>(s.guard_addr) = 0;
		expanded_size += s.guard_size;
	}
#endif

	if (auto entry = GetEntry(); entry != 0)
	{
		EntryParams p {};
		p.argc    = 1;
		p.argv[0] = "KytyEmu";

		printf("stack_addr = %" PRIx64 "\n", reinterpret_cast<uint64_t>(&p));

		Core::mem_guest_thread_enter();

		run_entry(entry, &p, ProgramExitHandler);
		Core::mem_guest_thread_leave();
		// Guest main returned (or long-running titles never reach here). Observation only.
		Emulator::Agent::Lifecycle::EmitGuestExit(0);
	}
}

void RuntimeLinker::Clear()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		DeleteProgram(p);
	}
	m_programs.Clear();
	delete m_symbols;
	m_symbols             = nullptr;
	m_relocated           = false;
	g_next_tls_module_id  = 1;
	Kyty::Libs::LibKernel::ApplicationHeap::Reset();
}

void RuntimeLinker::Resolve(const String& name, SymbolType type, Program* program, SymbolRecord* out_info, bool* bind_self)
{
	KYTY_PROFILER_FUNCTION();

	Core::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);
	EXIT_IF(program == nullptr || program->dynamic_info == nullptr);

	auto ids = name.Split(U'#');

	if (bind_self != nullptr)
	{
		*bind_self = false;
	}

	if (ids.Size() != 3)
	{
		EXIT("malformed import identity: %s\n", name.C_Str());
		return;
	}
	if (ids.At(0).IsEmpty())
	{
		EXIT("malformed import symbol identity: %s\n", name.C_Str());
		return;
	}

	const LibraryId* library = FindLibrary(*program, ids.At(1));
	const ModuleId*  module  = FindModule(*program, ids.At(2));
	if (library == nullptr || module == nullptr)
	{
		EXIT("malformed import references unknown library or module: %s\n", name.C_Str());
		return;
	}

	SymbolResolve resolve {};
	resolve.name                 = ids.At(0);
	resolve.library              = library->name;
	resolve.library_version      = library->version;
	resolve.module               = module->name;
	resolve.module_version_major = module->version_major;
	resolve.module_version_minor = module->version_minor;
	resolve.type                 = type;

	const SymbolRecord* hle_record    = m_symbols != nullptr ? m_symbols->Find(resolve) : nullptr;
	const SymbolRecord* export_record = nullptr;
	auto*               exporter      = FindProgram(*module, *library);
	if (exporter != nullptr && exporter->export_symbols != nullptr)
	{
		export_record = exporter->export_symbols->Find(resolve);
		if (export_record != nullptr && bind_self != nullptr)
		{
			*bind_self = (exporter == program);
		}
	}
	// Guest libc.prx exports the allocation family, but constructors can reach
	// those imports before its allocator initialization is complete. Its internal
	// mspace can also remain BSS-zero when the legacy ApplicationHeap table is used.
	// Prefer the HLE allocator for these NIDs so constructors do not throw
	// (DebugRaiseException 0xa0020008). Other exports still win over HLE.
	const bool prefer_hle_allocator =
	    hle_record != nullptr &&
	    (resolve.name == U"gQX+4GDQjpM" || resolve.name == U"2X5agFjKxMc" || resolve.name == U"Y7aJ1uydPMo" ||
	     resolve.name == U"tIhsqj0qsFE" || resolve.name == U"Ujf3KzMvRmI" || resolve.name == U"2Btkg8k24Zg" ||
	     resolve.name == U"cVSk9y8URbc" || resolve.name == U"fJnpuVVBbKk" || resolve.name == U"hdm0YfMa7TQ" ||
	     resolve.name == U"z+P+xCnWLBk" || resolve.name == U"MLWl90SFWNE");
	const SymbolRecord* record =
	    prefer_hle_allocator ? hle_record : (export_record != nullptr ? export_record : hle_record);

	const String canonical_name = SymbolDatabase::GenerateName(resolve);
	const auto   identity       = MissingImport::SymbolIdentity::From(resolve, canonical_name);

	Emulator::Validation::ImportResolveRequest request {};
	request.type       = type;
	request.has_export = record != nullptr;
	request.missing_function_import_enabled =
	    Core::BringUp::IsEnabled(Core::BringUp::Feature::MissingFunctionImport, Core::BringUp::Subsystem::Loader);
	const auto identity_utf8 = identity.canonical.utf8_str();
	request.identity    = identity_utf8.GetData();
	const auto decision = Emulator::Validation::ClassifyImportResolution(request);

	switch (decision.outcome)
	{
		case Emulator::Validation::ImportResolutionOutcome::Resolved:
		{
			EXIT_IF(record == nullptr);
			*out_info           = *record;
			const bool from_hle = (hle_record == record);
			Emulator::Agent::Lifecycle::EmitSymbolResolved(out_info->name.C_Str(), from_hle ? "hle" : "export");
			return;
		}
		case Emulator::Validation::ImportResolutionOutcome::DiagnosticFunctionStub:
			out_info->name     = canonical_name;
			out_info->dbg_name = U"kyty_missing_func_stub";
			printf(FG_BRIGHT_YELLOW "STUB (missing): %s [%s]" DEFAULT "\n", identity.name.C_Str(), identity.library.C_Str());
			out_info->vaddr = MissingImport::AssignFuncStubOrAbort(identity);
			EXIT_IF(out_info->vaddr == 0);
			return;
		case Emulator::Validation::ImportResolutionOutcome::StrictUnresolvedFunction:
			out_info->vaddr    = 0;
			out_info->name     = canonical_name;
			out_info->dbg_name = U"";
			return;
		case Emulator::Validation::ImportResolutionOutcome::UnresolvedNonFunction:
			// Preserve the relocation contract: weak object imports are mapped
			// to the explicit invalid-memory sentinel, while strong imports
			// fail in relocate() with their bind and relocation context. Resolve
			// must not fabricate an object address or abort before bind policy is
			// available.
			out_info->vaddr    = 0;
			out_info->name     = canonical_name;
			out_info->dbg_name = U"unresolved_non_function";
			return;
		case Emulator::Validation::ImportResolutionOutcome::Malformed:
			EXIT("=== Malformed import!!! ===\n[%d]\t%s type=%d reason=%s\n", Core::Thread::GetThreadIdUnique(), identity.canonical.C_Str(),
			     static_cast<int>(type), decision.validation.error.reason);
			return;
	}
	EXIT("unhandled import resolution outcome");
}

MissingImportDiagnostics RuntimeLinker::GetMissingImportDiagnostics() const
{
	return MissingImport::Snapshot();
}

MissingImportDiagnostics RuntimeLinker::GetGlobalMissingImportDiagnostics()
{
	return MissingImport::Snapshot();
}

uint32_t RuntimeLinker::LoadedProgramCount()
{
	Core::LockGuard lock(m_mutex);
	return m_programs.Size();
}

Vector<ProgramExportSnapshot> RuntimeLinker::SnapshotExportPrograms()
{
	Core::LockGuard               lock(m_mutex);
	Vector<ProgramExportSnapshot> snapshot;
	for (const auto* program: m_programs)
	{
		if (program == nullptr)
		{
			continue;
		}

		ProgramExportSnapshot program_snapshot {};
		program_snapshot.unique_id = program->unique_id;
		program_snapshot.file_name = program->file_name;
		if (program->export_symbols != nullptr)
		{
			const uint32_t export_count = program->export_symbols->SymbolCount();
			for (uint32_t i = 0; i < export_count; ++i)
			{
				const SymbolRecord* export_symbol = program->export_symbols->SymbolAt(i);
				if (export_symbol != nullptr && !export_symbol->name.IsEmpty())
				{
					program_snapshot.export_names.Add(export_symbol->name);
				}
			}
		}
		snapshot.Add(std::move(program_snapshot));
	}
	return snapshot;
}

Vector<LoadedModuleSnapshot> RuntimeLinker::SnapshotLoadedModules()
{
	Core::LockGuard              lock(m_mutex);
	Vector<LoadedModuleSnapshot> snapshot;
	for (const auto* program: m_programs)
	{
		if (program == nullptr || program->elf == nullptr || program->base_vaddr == 0)
		{
			continue;
		}

		LoadedModuleSnapshot module_snapshot {};
		module_snapshot.unique_id   = program->unique_id;
		module_snapshot.file_name   = program->file_name;
		module_snapshot.base_vaddr  = program->base_vaddr;
		module_snapshot.base_size   = program->base_size;
		module_snapshot.entry_point = program->elf->GetEntry() + program->base_vaddr;
		snapshot.Add(std::move(module_snapshot));
	}
	return snapshot;
}

bool RuntimeLinker::TryGetProgramUnwindInfoByAddr(uint64_t vaddr, ProgramUnwindInfo* out_info)
{
	if (out_info == nullptr)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	for (const auto* program: m_programs)
	{
		if (program == nullptr || program->elf == nullptr)
		{
			continue;
		}

		const auto* ehdr = program->elf->GetEhdr();
		const auto* phdr = program->elf->GetPhdr();
		if (ehdr == nullptr || phdr == nullptr)
		{
			continue;
		}

		bool contains_address = false;
		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
		{
			if (phdr[i].p_memsz == 0 || (phdr[i].p_type != PT_LOAD && phdr[i].p_type != PT_OS_RELRO) ||
			    phdr[i].p_vaddr > std::numeric_limits<uint64_t>::max() - program->base_vaddr)
			{
				continue;
			}
			const uint64_t begin = program->base_vaddr + phdr[i].p_vaddr;
			if (phdr[i].p_memsz <= std::numeric_limits<uint64_t>::max() - begin && vaddr >= begin && vaddr < begin + phdr[i].p_memsz)
			{
				contains_address = true;
				break;
			}
		}
		if (!contains_address)
		{
			continue;
		}

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
		{
			if (phdr[i].p_type != PT_GNU_EH_FRAME || phdr[i].p_memsz < 8 ||
			    phdr[i].p_vaddr > std::numeric_limits<uint64_t>::max() - program->base_vaddr)
			{
				continue;
			}

			const uint64_t header_addr = program->base_vaddr + phdr[i].p_vaddr;
			bool header_is_mapped = false;
			for (Elf64_Half j = 0; j < ehdr->e_phnum; j++)
			{
				if (phdr[j].p_type != PT_LOAD || (phdr[j].p_flags & PF_R) == 0 || phdr[j].p_memsz < 8 ||
				    phdr[j].p_vaddr > std::numeric_limits<uint64_t>::max() - program->base_vaddr)
				{
					continue;
				}
				const uint64_t begin = program->base_vaddr + phdr[j].p_vaddr;
				if (phdr[j].p_memsz <= std::numeric_limits<uint64_t>::max() - begin && header_addr >= begin &&
				    header_addr <= begin + phdr[j].p_memsz - 8)
				{
					header_is_mapped = true;
					break;
				}
			}
			if (!header_is_mapped || program->base_size > std::numeric_limits<uint64_t>::max() - program->base_vaddr)
			{
				continue;
			}

			EhFrameInfo decoded {};
			if (!LoaderDecodeEhFrameHeader(reinterpret_cast<const uint8_t*>(header_addr), 8, header_addr,
			                               program->base_vaddr + program->base_size, &decoded))
			{
				continue;
			}

			bool frame_is_mapped = false;
			for (Elf64_Half j = 0; j < ehdr->e_phnum; j++)
			{
				if (phdr[j].p_type != PT_LOAD || (phdr[j].p_flags & PF_R) == 0 || phdr[j].p_memsz == 0 ||
				    phdr[j].p_vaddr > std::numeric_limits<uint64_t>::max() - program->base_vaddr)
				{
					continue;
				}
				const uint64_t begin = program->base_vaddr + phdr[j].p_vaddr;
				if (phdr[j].p_memsz <= std::numeric_limits<uint64_t>::max() - begin && decoded.frame_addr >= begin &&
				    decoded.frame_addr < begin + phdr[j].p_memsz)
				{
					decoded.frame_size = begin + phdr[j].p_memsz - decoded.frame_addr;
					frame_is_mapped    = true;
					break;
				}
			}
			if (!frame_is_mapped)
			{
				continue;
			}

			out_info->file_name            = program->file_name.FilenameWithoutDirectory();
			out_info->eh_frame_header_addr = decoded.header_addr;
			out_info->eh_frame_addr        = decoded.frame_addr;
			out_info->eh_frame_size        = decoded.frame_size;
			out_info->image_addr           = program->base_vaddr;
			out_info->image_size           = program->base_size;
			return true;
		}
	}

	return false;
}

Program* RuntimeLinker::AttachSyntheticExportModule(const String& file_name)
{
	Core::LockGuard lock(m_mutex);
	static int32_t  synth_id_seq = 1'000'000;

	auto* program                        = new Program;
	program->rt                          = this;
	program->file_name                   = file_name;
	program->unique_id                   = ++synth_id_seq;
	program->dynamic_info                = new DynamicInfo;
	program->export_symbols              = new SymbolDatabase;
	program->import_symbols              = new SymbolDatabase;
	program->fail_if_global_not_resolved = false;
	// No ELF / base mapping — export table only (conflict + Resolve HLE tests).
	m_programs.Add(program);
	return program;
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
		{
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size)
			{
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret), phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->unique_id == id)
		{
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
		{
			if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
			{
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = get_aligned_size(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size)
				{
					return p;
				}
			}
		}
	}

	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr)
{
	void* stack[20];
	int   depth = 20;

	sys_stack_walk_x86(frame_ptr, stack, &depth);

	std::printf("Stack trace [thread = %d]:\n", Core::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++)
	{
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		std::printf("[%d] %016" PRIx64 ", %s\n", i, vaddr, (p == nullptr ? "???" : p->file_name.FilenameWithoutDirectory().C_Str()));
	}
}

void RuntimeLinker::StartAllModules()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	Vector<Program*>              shared_programs;
	Vector<ModuleStartDescriptor> descriptors;
	for (auto* p: m_programs)
	{
		if (p->elf->IsShared())
		{
			EXIT_IF(p->dynamic_info == nullptr);
			ModuleStartDescriptor descriptor {};
			descriptor.file_name = p->file_name.FilenameWithoutDirectory();
			if (p->dynamic_info->so_name != nullptr)
			{
				descriptor.so_name = String::FromUtf8(p->dynamic_info->so_name);
			}
			for (const auto* dependency: p->dynamic_info->needed)
			{
				if (dependency != nullptr)
				{
					descriptor.needed.Add(String::FromUtf8(dependency));
				}
			}
			shared_programs.Add(p);
			descriptors.Add(std::move(descriptor));
		}
	}

	for (const uint32_t index: LoaderBuildModuleStartOrder(descriptors))
	{
		StartModule(shared_programs[index], 0, nullptr, nullptr);
	}
}

void RuntimeLinker::StopAllModules()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->elf->IsShared())
		{
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp, module_func_t func)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(!m_programs.Contains(program));

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Start module: " BG_BLUE BOLD "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", program->file_name.C_Str());
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

	return run_ini_fini(program->dynamic_info->init_vaddr + program->base_vaddr, args, argp, func);
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(!m_programs.Contains(program));

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Stop module: " BG_BLUE BOLD "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", program->file_name.C_Str());
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

	int result = run_ini_fini(program->dynamic_info->fini_vaddr + program->base_vaddr, args, argp, func);

	Libs::LibKernel::PthreadDeleteStaticObjects(program);

	return result;
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program)
{
	EXIT_IF(program == nullptr);

	program->tls.mutex.Lock();

	auto& tls = program->tls.tlss.GetOrPutDef(Core::Thread::GetThreadIdUnique(), nullptr);

	if (tls == nullptr)
	{
		// Layout: [TLS image][TCB]. The thread pointer (fs base) is at tls + image_size
		// and points at the TCB, whose fields (self-pointer at [0], stack canary at
		// [0x28], etc.) the guest reads as fs:[0], fs:[0x28]. The TCB must therefore be
		// allocated and zero-initialised past the image, with the self-pointer set;
		// otherwise those reads land in unallocated memory (observed as 0xAAAA garbage).
		constexpr uint64_t tcb_size = 0x1000;
		tls                         = new uint8_t[program->tls.image_size + tcb_size];
		LoaderInitializeThreadTlsImage(tls, program->tls.image_size, reinterpret_cast<const uint8_t*>(program->tls.image_vaddr),
		                               program->tls.init_size);
		LoaderPrepareThreadTlsImage(tls, program->tls.image_size, program->tls.image_vaddr, program->base_vaddr, program->base_size,
		                            TlsGuestRead64, nullptr);
		auto* tcb = tls + program->tls.image_size;
		std::memset(tcb, 0, tcb_size);
		// TCB self-pointer (fs:[0] == fs base)
		*reinterpret_cast<uint64_t*>(tcb) = reinterpret_cast<uint64_t>(tcb);
	}

	uint8_t* ret = tls;

	program->tls.mutex.Unlock();

	return ret;
}

uint8_t* RuntimeLinker::TlsGetAddr(uint64_t module_id, uint64_t offset)
{
	EXIT_IF(module_id == 0);

	auto* rt = Core::Singleton<RuntimeLinker>::Instance(); // NOLINT
	EXIT_IF(rt == nullptr);

	Program* program = nullptr;
	{
		Core::LockGuard lock(rt->m_mutex);
		for (auto* p: rt->m_programs)
		{
			if (p != nullptr && p->tls.module_id == module_id)
			{
				program = p;
				break;
			}
		}
	}

	EXIT_IF(program == nullptr);
	EXIT_IF(offset > program->tls.image_size);

	return TlsGetAddr(program) + offset;
}

void RuntimeLinker::DeleteTls(Program* program, int thread_id)
{
	EXIT_IF(program == nullptr);

	program->tls.mutex.Lock();

	delete[] program->tls.tlss.Get(thread_id, nullptr);
	program->tls.tlss.Remove(thread_id);

	program->tls.mutex.Unlock();
}

static uint64_t calc_base_size(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr)
{
	uint64_t base_size = 0;
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
		{
			uint64_t last_addr = phdr[i].p_vaddr + get_aligned_size(phdr + i);
			if (last_addr > base_size)
			{
				base_size = last_addr;
			}
		}
	}
	return base_size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 || program->elf == nullptr ||
	        program->exception_handler != nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared   = program->elf->IsShared();
	bool is_next_gen = program->elf->IsNextGen();

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	if (is_next_gen && !is_shared)
	{
		Config::SetNextGen(true);
	}

	program->base_size         = calc_base_size(ehdr, phdr);
	program->base_size_aligned = (program->base_size & ~(static_cast<uint64_t>(0x1000) - 1)) + 0x1000;

	uint64_t exception_handler_size = Core::VirtualMemory::ExceptionHandler::GetSize();
	uint64_t tls_handler_size       = is_shared ? 0 : Jit::SafeCall::GetSize();
	uint64_t alloc_size             = program->base_size_aligned + exception_handler_size + tls_handler_size;

	program->base_vaddr = Core::VirtualMemory::Alloc(g_desired_base_addr, alloc_size, Core::VirtualMemory::Mode::ExecuteReadWrite);

	if (!is_shared)
	{
		program->tls.handler_vaddr = program->base_vaddr + program->base_size_aligned + exception_handler_size;
	}

	g_desired_base_addr += CODE_BASE_INCR * (1 + alloc_size / CODE_BASE_INCR);

	EXIT_IF(program->base_vaddr == 0);
	EXIT_IF(program->base_size_aligned < program->base_size);

	printf("base_vaddr             = 0x%016" PRIx64 "\n", program->base_vaddr);
	printf("base_size              = 0x%016" PRIx64 "\n", program->base_size);
	printf("base_size_aligned      = 0x%016" PRIx64 "\n", program->base_size_aligned);
	printf("exception_handler_size = 0x%016" PRIx64 "\n", exception_handler_size);
	if (!is_shared)
	{
		printf("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

	program->exception_handler = new Core::VirtualMemory::ExceptionHandler;

	if (is_shared)
	{
		program->exception_handler->Install(program->base_vaddr, program->base_vaddr + program->base_size_aligned,
		                                    program->base_size_aligned + exception_handler_size + tls_handler_size, kyty_exception_handler);
	} else
	{
		program->exception_handler->Install(SYSTEM_RESERVED, program->base_vaddr + program->base_size_aligned,
		                                    program->base_vaddr + program->base_size_aligned + exception_handler_size + tls_handler_size -
		                                        SYSTEM_RESERVED,
		                                    kyty_exception_handler);

		// if (Libs::Graphics::GpuMemoryWatcherEnabled())
		{
			Core::VirtualMemory::ExceptionHandler::InstallVectored(kyty_exception_handler);
			Libs::Graphics::GpuDirtyPageTrackerNotifyFaultHandlerInstalled();
		}
	}

	// program->elf->SetBaseVAddr(program->base_vaddr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
		{
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = get_aligned_size(phdr + i);
			auto     mode                = get_mode(phdr[i].p_flags);

			printf("[%d] addr        = 0x%016" PRIx64 "\n", i, segment_addr);
			printf("[%d] file_size   = %" PRIu64 "\n", i, segment_file_size);
			printf("[%d] memory_size = %" PRIu64 "\n", i, segment_memory_size);
			printf("[%d] mode        = %s\n", i, Core::EnumName(mode).C_Str());

			program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);

			bool skip_protect = (phdr[i].p_type == PT_LOAD && is_next_gen && mode == Core::VirtualMemory::Mode::NoAccess);

			if (Core::VirtualMemory::IsExecute(mode))
			{
				PatchProgram(program, segment_addr, segment_memory_size);
			}

			if (!skip_protect)
			{
				Core::VirtualMemory::Protect(segment_addr, segment_memory_size, mode);

				if (Core::VirtualMemory::IsExecute(mode))
				{
					Core::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size);
				}
			}
		}

		if (phdr[i].p_type == PT_TLS)
		{
			EXIT_IF(program->tls.image_vaddr != 0 || program->tls.image_size != 0 || program->tls.module_id != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.image_size  = get_aligned_size(phdr + i);
			program->tls.init_size   = std::min(phdr[i].p_filesz, program->tls.image_size);
			program->tls.static_offset = program->tls.image_size;
			program->tls.module_id     = g_next_tls_module_id++;

			printf("tls addr = 0x%016" PRIx64 "\n", program->tls.image_vaddr);
			printf("tls init   = %" PRIu64 "\n", program->tls.init_size);
			printf("tls size   = %" PRIu64 "\n", program->tls.image_size);
			printf("tls module = %" PRIu64 "\n", program->tls.module_id);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM)
		{
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
		}
	}

	if (!is_shared)
	{
		SetupTlsHandler(program);
	}

	printf("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p)
{
	if (p->base_vaddr != 0 || p->base_size != 0)
	{
		Core::VirtualMemory::Free(p->base_vaddr);
	}

	if (p->custom_call_plt_vaddr != 0 || p->custom_call_plt_num != 0)
	{
		Core::VirtualMemory::Free(p->custom_call_plt_vaddr);
	}

	delete p->elf;
	delete p->dynamic_info;
	delete p->exception_handler;
	delete p->export_symbols;
	delete p->import_symbols;

	FOR_HASH (p->tls.tlss)
	{
		delete p->tls.tlss.Value();
	}

	delete p;
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = new DynamicInfo;

	auto* elf = program->elf;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	get_dyn_data_os(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	get_dyn_value(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	get_dyn_data_os(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	get_dyn_value(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	get_dyn_value(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	get_dyn_data_os(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	get_dyn_ptr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	get_dyn_ptr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	get_dyn_ptr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	get_dyn_ptr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	get_dyn_ptr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	get_dyn_value(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	get_dyn_value(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	get_dyn_value(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTGOT) && elf->HasDynValue(DT_PLTGOT));
	get_dyn_ptr(elf, &program->dynamic_info->pltgot_vaddr, DT_OS_PLTGOT);
	get_dyn_ptr(elf, &program->dynamic_info->pltgot_vaddr, DT_PLTGOT);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	get_dyn_value(elf, &jmprel_type, DT_OS_PLTREL);
	get_dyn_value(elf, &jmprel_type, DT_PLTREL);

	EXIT_NOT_IMPLEMENTED(jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA)
	{
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		get_dyn_data_os(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		get_dyn_value(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		get_dyn_value(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	get_dyn_data_os(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	get_dyn_value(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	get_dyn_value(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	get_dyn_value(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	get_dyn_value(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);

	get_dyn_value(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	get_dyn_value(elf, &program->dynamic_info->debug, DT_DEBUG);
	get_dyn_value(elf, &program->dynamic_info->flags, DT_FLAGS);
	get_dyn_value(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	Vector<uint64_t> needed;
	get_dyn_values(elf, &needed, DT_NEEDED);
	for (auto need: needed)
	{
		program->dynamic_info->needed.Add(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	get_dyn_value(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) && elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) && elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) && elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) && elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	get_dyn_modules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table, DT_OS_NEEDED_MODULE);
	get_dyn_modules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table, DT_OS_NEEDED_MODULE_1);
	get_dyn_modules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table, DT_OS_MODULE_INFO);
	get_dyn_modules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table, DT_OS_MODULE_INFO_1);
	get_dyn_libs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table, DT_OS_IMPORT_LIB);
	get_dyn_libs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table, DT_OS_IMPORT_LIB_1);
	get_dyn_libs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table, DT_OS_EXPORT_LIB);
	get_dyn_libs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table, DT_OS_EXPORT_LIB_1);
}

static void InstallRelocateHandler(Program* program)
{
	KYTY_PROFILER_FUNCTION();

	uint64_t pltgot_vaddr = program->dynamic_info->pltgot_vaddr + program->base_vaddr;
	uint64_t pltgot_size  = static_cast<uint64_t>(3) * 8;
	void**   pltgot       = reinterpret_cast<void**>(pltgot_vaddr);

	Core::VirtualMemory::Mode old_mode {};
	Core::VirtualMemory::Protect(pltgot_vaddr, pltgot_size, Core::VirtualMemory::Mode::Write, &old_mode);

	pltgot[1] = program;
	pltgot[2] = reinterpret_cast<void*>(RelocateHandler);

	Core::VirtualMemory::Protect(pltgot_vaddr, pltgot_size, old_mode);

	if (Core::VirtualMemory::IsExecute(old_mode))
	{
		Core::VirtualMemory::FlushInstructionCache(pltgot_vaddr, pltgot_size);
	}

	// TODO(): check if this table already generated by compiler (sometimes it is missing)
	if (program->custom_call_plt_vaddr == 0)
	{
		program->custom_call_plt_num   = program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
		auto size                      = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		program->custom_call_plt_vaddr = Core::VirtualMemory::Alloc(SYSTEM_RESERVED, size, Core::VirtualMemory::Mode::Write);
		EXIT_NOT_IMPLEMENTED(program->custom_call_plt_vaddr == 0);
		auto* code = new (reinterpret_cast<void*>(program->custom_call_plt_vaddr)) Jit::CallPlt(program->custom_call_plt_num);
		code->SetPltGot(pltgot_vaddr);
		Core::VirtualMemory::Protect(program->custom_call_plt_vaddr, size, Core::VirtualMemory::Mode::Execute);
		Core::VirtualMemory::FlushInstructionCache(program->custom_call_plt_vaddr, size);
	}
}

void RuntimeLinker::Relocate(Program* program)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);

	if (g_invalid_memory == 0)
	{
		g_invalid_memory = Core::VirtualMemory::Alloc(INVALID_MEMORY, 4096, Core::VirtualMemory::Mode::NoAccess);
		EXIT_NOT_IMPLEMENTED(g_invalid_memory == 0);
	}

	printf("--- Relocate program: " FG_WHITE BOLD "%s" DEFAULT " ---\n", program->file_name.C_Str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->jmprela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->pltgot_vaddr == 0);

	InstallRelocateHandler(program);

	relocate_all(program->dynamic_info->rela_table, program->dynamic_info->rela_table_total_size, program, false);
	relocate_all(program->dynamic_info->jmprela_table, program->dynamic_info->jmprela_table_size, program, true);
}

Program* RuntimeLinker::FindProgram(const ModuleId& m, const LibraryId& l)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		const auto& export_libs    = p->dynamic_info->export_libs;
		const auto& export_modules = p->dynamic_info->export_modules;

		if (export_libs.Contains(l) && export_modules.Contains(m))
		{
			return p;
		}
	}
	return nullptr;
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const String& id)
{
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto index = import_modules.Find(id, [](auto module, auto str) { return module.id == str; }); import_modules.IndexValid(index))
	{
		return &import_modules.At(index);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto index = export_modules.Find(id, [](auto module, auto str) { return module.id == str; }); export_modules.IndexValid(index))
	{
		return &export_modules.At(index);
	}

	return nullptr;
}

// void RuntimeLinker::CreateTls() {}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const String& id)
{
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto index = import_libs.Find(id, [](auto lib, auto str) { return lib.id == str; }); import_libs.IndexValid(index))
	{
		return &import_libs.At(index);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto index = export_libs.Find(id, [](auto lib, auto str) { return lib.id == str; }); export_libs.IndexValid(index))
	{
		return &export_libs.At(index);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	program->export_symbols = new SymbolDatabase;
	program->import_symbols = new SymbolDatabase;

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export)
	{
		if (program->dynamic_info->symbol_table == nullptr || program->dynamic_info->str_table == nullptr)
		{
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) + program->dynamic_info->symbol_table_total_size;
		     sym++)
		{
			String id   = String::FromUtf8(program->dynamic_info->str_table + sym->st_name);
			auto   bind = sym->GetBind();
			auto   type = sym->GetType();
			auto   ids  = id.Split(U'#');

			if (ids.Size() == 3)
			{
				const auto* l = FindLibrary(*program, ids.At(1));
				const auto* m = FindModule(*program, ids.At(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) && (type == STT_FUNC || type == STT_OBJECT) &&
				    is_export == (sym->st_value != 0))
				{
					SymbolResolve sr {};
					sr.name                 = ids.At(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type)
					{
						case STT_NOTYPE: sr.type = SymbolType::NoType; break;
						case STT_FUNC: sr.type = SymbolType::Func; break;
						case STT_OBJECT: sr.type = SymbolType::Object; break;
						default: sr.type = SymbolType::Unknown; break;
					}
					symbols->Add(sr, (is_export ? sym->st_value + program->base_vaddr : 0));
				}
			}
		}
	};

	syms(program, program->export_symbols, true);
	syms(program, program->import_symbols, false);
}

void RuntimeLinker::SetupTlsHandler(Program* program)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(g_tls_main_program != nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	g_tls_main_program = program;

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	memset(g_tls_reg_save_area, 0, XSAVE_BUFFER_SIZE);
	std::memcpy(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD));

	code->SetFunc(TlsMainGetAddr);
	code->SetRegSaveArea(g_tls_reg_save_area);
	code->SetLockVar(&g_tls_spinlock);

	Core::VirtualMemory::Protect(program->tls.handler_vaddr, Jit::SafeCall::GetSize(), Core::VirtualMemory::Mode::Execute);
	Core::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr, Jit::SafeCall::GetSize());
}

void RuntimeLinker::DeleteTlss(int thread_id)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		DeleteTls(p, thread_id);
	}
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
