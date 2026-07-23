#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Math/Rand.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/RetailKernel.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Kernel/SyncOnAddress.h"
#include "Emulator/Kernel/Time.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Network.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/Elf.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <mutex>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <arpa/inet.h>
#include <unistd.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

namespace LibKernel {

using KernelModule                   = int32_t;
using get_thread_atexit_count_func_t = KYTY_SYSV_ABI int (*)(KernelModule);
using thread_atexit_report_func_t    = KYTY_SYSV_ABI void (*)(KernelModule);

#pragma pack(1)

struct KernelLoadModuleOpt
{
	size_t size;
};

struct KernelUnloadModuleOpt
{
	size_t size;
};

struct TlsInfo
{
	uint64_t module_id;
	uint64_t offset;
};

struct MallocReplace
{
	uint64_t size               = sizeof(MallocReplace);
	void*    malloc_initialize  = nullptr;
	void*    malloc_finalize    = nullptr;
	void*    malloc             = nullptr;
	void*    free               = nullptr;
	void*    calloc             = nullptr;
	void*    realloc            = nullptr;
	void*    memalign           = nullptr;
	void*    reallocalign       = nullptr;
	void*    posix_memalign     = nullptr;
	void*    malloc_stats       = nullptr;
	void*    malloc_stats_fast  = nullptr;
	void*    malloc_usable_size = nullptr;
	void*    aligned_alloc      = nullptr;
};

struct NewReplace
{
	uint64_t size                           = sizeof(NewReplace);
	void*    new_p                          = nullptr;
	void*    new_nothrow                    = nullptr;
	void*    new_array                      = nullptr;
	void*    new_array_nothrow              = nullptr;
	void*    delete_p                       = nullptr;
	void*    delete_nothrow                 = nullptr;
	void*    delete_array                   = nullptr;
	void*    delete_array_nothrow           = nullptr;
	void*    delete_with_size               = nullptr;
	void*    delete_with_size_nothrow       = nullptr;
	void*    delete_array_with_size         = nullptr;
	void*    delete_array_with_size_nothrow = nullptr;
};

struct ModuleInfo
{
	uint64_t     size;
	uint64_t     info[32];
	KernelModule handle;
	uint8_t      pad[156];
};

#pragma pack()

constexpr size_t PROGNAME_MAX_SIZE = 511;

static uint64_t    g_stack_chk_guard                     = 0xDeadBeef5533CCAA;
static char        g_progname_buf[PROGNAME_MAX_SIZE + 1] = {0};
static const char* g_progname                            = g_progname_buf;

static get_thread_atexit_count_func_t g_get_thread_atexit_count_func = nullptr;
static thread_atexit_report_func_t    g_thread_atexit_report_func    = nullptr;

void SetProgName(const String& name)
{
	strncpy(g_progname_buf, name.C_Str(), PROGNAME_MAX_SIZE);
}

static KYTY_SYSV_ABI int* get_error_addr()
{
	PRINT_NAME();

	return Posix::GetErrorAddr();
}

static KYTY_SYSV_ABI void stack_chk_fail()
{
	PRINT_NAME();

	EXIT("stack fail!!!");
}

static KYTY_SYSV_ABI int sigprocmask(int /*how*/, const void* /*set*/, void* /*oset*/)
{
	// PRINT_NAME();

	// printf("\t how = %d\n", how);
	// printf("\t set = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(set));
	// printf("\t oset = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(oset));

	return 0;
}

static KYTY_SYSV_ABI KernelModule KernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags,
                                                        const KernelLoadModuleOpt* opt, int* res)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	printf("\tmodule_file_name = %s\n", (module_file_name != nullptr ? module_file_name : "<null>"));

	if (module_file_name == nullptr || module_file_name[0] == '\0' || flags != 0 || opt != nullptr)
	{
		if (res != nullptr)
		{
			*res = KERNEL_ERROR_EINVAL;
		}
		return KERNEL_ERROR_EINVAL;
	}

	const auto module_path = FileSystem::GetRealFilename(String::FromUtf8(module_file_name));
	if (!Core::File::IsFileExisting(module_path))
	{
		if (res != nullptr)
		{
			*res = KERNEL_ERROR_ENOENT;
		}
		return KERNEL_ERROR_ENOENT;
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(module_path);

	auto handle = program->unique_id;

	program->dbg_print_reloc = true;

	rt->RelocateProgram(program);

	int result = rt->StartModule(program, args, argp, nullptr);

	printf("\tmodule_start() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr)
	{
		*res = result;
	}

	return static_cast<KernelModule>(handle);
}

static int KYTY_SYSV_ABI KernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags,
                                                const KernelUnloadModuleOpt* opt, int* res)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* program = rt->FindProgramById(handle);

	EXIT_NOT_IMPLEMENTED(program == nullptr);

	if (g_get_thread_atexit_count_func != nullptr && g_get_thread_atexit_count_func(program->unique_id) > 0)
	{
		printf("KernelStopUnloadModule: cannot unload %s\n", program->file_name.C_Str());
		if (g_thread_atexit_report_func != nullptr)
		{
			g_thread_atexit_report_func(program->unique_id);
		}
		return KERNEL_ERROR_EBUSY;
	}

	int result = rt->StopModule(program, args, argp, nullptr);

	printf("\tmodule_stop() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr)
	{
		*res = result;
	}

	rt->UnloadProgram(program);

	return OK;
}

static void* KYTY_SYSV_ABI tls_get_addr(TlsInfo* info)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	EXIT_IF(info == nullptr);
	return Loader::RuntimeLinker::TlsGetAddr(info->module_id, info->offset);
}

static void* KYTY_SYSV_ABI KernelGetProcParam()
{
	PRINT_NAME();

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance(); // NOLINT

	return reinterpret_cast<void*>(rt->GetProcParam());
}

static int KYTY_SYSV_ABI KernelDlsym(KernelModule handle, const char* symbol, void** address)
{
	PRINT_NAME();
	printf("\t handle = %d\n", handle);
	printf("\t symbol = %s\n", symbol != nullptr ? symbol : "(null)");

	if (address == nullptr || symbol == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* runtime = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = runtime->FindProgramById(handle);
	if (program == nullptr || program->export_symbols == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	const String nid = Loader::EncodeNameAsNid(symbol);
	for (const auto type: {Loader::SymbolType::Func, Loader::SymbolType::Object, Loader::SymbolType::NoType})
	{
		if (const auto* record = program->export_symbols->FindByNid(nid, type); record != nullptr)
		{
			*address = reinterpret_cast<void*>(record->vaddr);
			return OK;
		}
	}

	return KERNEL_ERROR_ESRCH;
}

static void KYTY_SYSV_ABI KernelRtldSetApplicationHeapAPI(void* api[])
{
	PRINT_NAME();

	if (api == nullptr)
	{
		return;
	}

	for (int i = 0; i < 10; i++)
	{
		printf("\tapi[%d] = 0x%016" PRIx64 "\n", i, reinterpret_cast<uint64_t>(api[i]));
	}

	ApplicationHeap::RegisterApi(api);
}

static int64_t KYTY_SYSV_ABI write(int d, const char* str, int64_t size)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(d < 0);

	if (d > 2)
	{
		return POSIX_N_CALL(FileSystem::KernelWrite(d, str, size));
	}

	int size_int = static_cast<int>(size);

	emu_printf(FG_BRIGHT_MAGENTA "%.*s" DEFAULT, size_int, str);

	return size_int;
}

static int KYTY_SYSV_ABI open(const char* path, int flags, int mode)
{
	return POSIX_N_CALL(FileSystem::KernelOpen(path, flags, mode));
}

static int KYTY_SYSV_ABI close(int d)
{
	EXIT_NOT_IMPLEMENTED(d <= 2);

	return POSIX_CALL(FileSystem::KernelClose(d));
}

static int64_t KYTY_SYSV_ABI lseek(int d, int64_t offset, int whence)
{
	return POSIX_N_CALL(FileSystem::KernelLseek(d, offset, whence));
}

static int64_t KYTY_SYSV_ABI read(int d, void* buf, uint64_t nbytes)
{
	// PRINT_NAME();

	if (d > 2)
	{
		return POSIX_N_CALL(FileSystem::KernelRead(d, buf, nbytes));
	}

	EXIT_NOT_IMPLEMENTED(d != 0);

	return static_cast<int64_t>(strlen(std::fgets(static_cast<char*>(buf), static_cast<int>(nbytes), stdin)));
}

static int64_t KYTY_SYSV_ABI pread(int d, void* buf, size_t nbytes, int64_t offset)
{
	return POSIX_N_CALL(FileSystem::KernelPread(d, buf, nbytes, offset));
}

static int64_t KYTY_SYSV_ABI pwrite(int d, const void* buf, size_t nbytes, int64_t offset)
{
	return POSIX_N_CALL(FileSystem::KernelPwrite(d, buf, nbytes, offset));
}

static int KYTY_SYSV_ABI dup(int old_d)
{
	return POSIX_N_CALL(FileSystem::KernelDup(old_d));
}

static int KYTY_SYSV_ABI dup2(int old_d, int new_d)
{
	return POSIX_N_CALL(FileSystem::KernelDup2(old_d, new_d));
}

static int KYTY_SYSV_ABI poll(FileSystem::KernelPollFd* fds, uint32_t count, int timeout)
{
	return POSIX_N_CALL(FileSystem::KernelPoll(fds, count, timeout));
}

namespace {

constexpr size_t kSceKernelModuleInfoSize = 0x160;

bool WriteSceKernelModuleInfo(void* out_info, const Loader::LoadedModuleSnapshot& module)
{
	if (out_info == nullptr)
	{
		return false;
	}

	auto* out = static_cast<uint8_t*>(out_info);
	std::memset(out, 0, kSceKernelModuleInfoSize);
	*reinterpret_cast<uint64_t*>(out) = kSceKernelModuleInfoSize;

	const String basename = module.file_name.FixFilenameSlash().FilenameWithoutDirectory();
	std::strncpy(reinterpret_cast<char*>(out + 0x08), basename.C_Str(), 255);

	*reinterpret_cast<uint64_t*>(out + 0x108) = module.base_vaddr;
	const uint32_t segment_size =
	    static_cast<uint32_t>(std::min(module.base_size, static_cast<uint64_t>(UINT32_MAX)));
	*reinterpret_cast<uint32_t*>(out + 0x110) = segment_size;
	*reinterpret_cast<int32_t*>(out + 0x114)  = 5;
	*reinterpret_cast<uint32_t*>(out + 0x148) = 1;
	return true;
}

const Loader::LoadedModuleSnapshot* FindLoadedModuleSnapshot(const Kyty::Vector<Loader::LoadedModuleSnapshot>& modules,
                                                             int32_t handle)
{
	for (const auto& module: modules)
	{
		if (module.unique_id == handle)
		{
			return &module;
		}
	}
	return nullptr;
}

} // namespace

static int KYTY_SYSV_ABI KernelGetModuleInfo(KernelModule handle, void* out_info)
{
	PRINT_NAME();

	printf("\t handle = %" PRId32 "\n", handle);
	printf("\t out_info = %p\n", out_info);

	if (out_info == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const uint64_t caller_size = *reinterpret_cast<const uint64_t*>(out_info);
	if (caller_size != kSceKernelModuleInfoSize)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	const auto modules = rt->SnapshotLoadedModules();
	const auto* module = FindLoadedModuleSnapshot(modules, handle);
	if (module == nullptr)
	{
		return KERNEL_ERROR_ENOENT;
	}

	if (!WriteSceKernelModuleInfo(out_info, *module))
	{
		return KERNEL_ERROR_EFAULT;
	}

	return OK;
}

static int KYTY_SYSV_ABI KernelGetModuleList(int32_t* handles, uint64_t capacity, uint64_t* out_count)
{
	PRINT_NAME();

	printf("\t handles = %p capacity = %" PRIu64 " out_count = %p\n", static_cast<void*>(handles), capacity,
	       static_cast<void*>(out_count));

	if (out_count == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	const auto modules = rt->SnapshotLoadedModules();

	*out_count = modules.Size();

	if (handles == nullptr || capacity == 0 || modules.IsEmpty())
	{
		return OK;
	}

	const uint64_t writable_count = std::min(capacity, static_cast<uint64_t>(modules.Size()));
	for (uint64_t i = 0; i < writable_count; i++)
	{
		handles[i] = modules[static_cast<uint32_t>(i)].unique_id;
	}

	if (static_cast<uint64_t>(modules.Size()) > capacity)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return OK;
}

static int KYTY_SYSV_ABI KernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r)
{
	PRINT_NAME();

	printf("\taddr = %016" PRIx64 "\n", addr);
	printf("\tn = %d\n", n);

	EXIT_NOT_IMPLEMENTED(n != 2);
	EXIT_NOT_IMPLEMENTED(r == nullptr);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	auto* p = rt->FindProgramByAddr(addr);

	if (p == nullptr)
	{
		printf("\thandle: not found\n");
		r->handle = 0;
		return -1;
	}

	r->handle = p->unique_id;

	printf("\thandle: %d\n", r->handle);

	return 0;
}

namespace {

#pragma pack(push, 1)
struct ModuleInfoForUnwind
{
	uint64_t st_size;
	char     name[256];
	uint64_t eh_frame_hdr_addr;
	uint64_t eh_frame_addr;
	uint64_t eh_frame_size;
	uint64_t seg0_addr;
	uint64_t seg0_size;
};
#pragma pack(pop)

static_assert(sizeof(ModuleInfoForUnwind) == 0x130);

constexpr Loader::Elf64_Word kPtGnuEhFrame = 0x6474e550;

bool DecodeEhFramePointer(const uint8_t* data, const uint8_t* end, uint8_t encoding, uint64_t field_addr,
                          uint64_t* value)
{
	if (value == nullptr || encoding == 0xff)
	{
		return false;
	}

	uint64_t out       = 0;
	const auto format  = static_cast<uint8_t>(encoding & 0x0fu);
	const auto app     = static_cast<uint8_t>(encoding & 0x70u);
	bool       is_signed = false;

	switch (format)
	{
		case 0x00:
		case 0x04:
			if (data + sizeof(uint64_t) > end)
			{
				return false;
			}
			std::memcpy(&out, data, sizeof(uint64_t));
			break;
		case 0x03:
			if (data + sizeof(uint32_t) > end)
			{
				return false;
			}
			{
				uint32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out = tmp;
			}
			break;
		case 0x0b:
			if (data + sizeof(int32_t) > end)
			{
				return false;
			}
			{
				int32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out       = static_cast<uint64_t>(static_cast<int64_t>(tmp));
				is_signed = true;
			}
			break;
		case 0x0c:
			if (data + sizeof(int64_t) > end)
			{
				return false;
			}
			std::memcpy(&out, data, sizeof(int64_t));
			is_signed = true;
			break;
		default: return false;
	}

	if (app == 0x10)
	{
		out = (is_signed ? static_cast<uint64_t>(static_cast<int64_t>(field_addr) + static_cast<int64_t>(out))
		                 : field_addr + out);
	}
	else if (app != 0)
	{
		return false;
	}

	*value = out;
	return true;
}

} // namespace

int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info)
{
	PRINT_NAME();

	printf("\t addr = 0x%016" PRIx64 " flags = %d info = %p\n", addr, flags, static_cast<void*>(info));

	if (flags < 0 || flags >= 3)
	{
		if (info != nullptr)
		{
			std::memset(info, 0, sizeof(ModuleInfoForUnwind));
		}
		return KERNEL_ERROR_EINVAL;
	}

	if (info == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (info->st_size < sizeof(ModuleInfoForUnwind))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt      = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramByAddr(addr);
	if (program == nullptr || program->elf == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	uint64_t eh_frame_hdr_size = 0;

	std::memset(info, 0, sizeof(ModuleInfoForUnwind));
	info->st_size = sizeof(ModuleInfoForUnwind);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0')
	{
		std::snprintf(info->name, sizeof(info->name), "%s", program->dynamic_info->so_name);
	}
	else
	{
		const String fallback = program->file_name.FixFilenameSlash().FilenameWithoutDirectory();
		std::snprintf(info->name, sizeof(info->name), "%s", fallback.C_Str());
	}

	uint64_t image_start = UINT64_MAX;
	uint64_t image_end   = 0;

	for (Loader::Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz == 0)
		{
			continue;
		}

		if (phdr[i].p_type == Loader::PT_LOAD || phdr[i].p_type == Loader::PT_OS_RELRO)
		{
			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			image_start      = std::min(image_start, start);
			image_end        = std::max(image_end, end);
		}

		if (phdr[i].p_type == kPtGnuEhFrame)
		{
			info->eh_frame_hdr_addr = program->base_vaddr + phdr[i].p_vaddr;
			eh_frame_hdr_size       = phdr[i].p_memsz;
		}
	}

	const auto* shdr = program->elf->GetShdr();
	if (shdr != nullptr && program->elf->GetStrTable() != nullptr)
	{
		for (Loader::Elf64_Half i = 0; i < ehdr->e_shnum; i++)
		{
			const char* section_name = program->elf->GetSectionName(static_cast<int>(i));
			if (section_name != nullptr && std::strcmp(section_name, ".eh_frame") == 0)
			{
				info->eh_frame_addr = program->base_vaddr + shdr[i].sh_addr;
				info->eh_frame_size = shdr[i].sh_size;
				break;
			}
		}
	}

	if (info->eh_frame_hdr_addr != 0 && eh_frame_hdr_size >= 4)
	{
		const auto* hdr = reinterpret_cast<const uint8_t*>(info->eh_frame_hdr_addr);
		const auto* end = hdr + eh_frame_hdr_size;
		if (hdr[0] == 1)
		{
			uint64_t eh_frame_addr = 0;
			if (DecodeEhFramePointer(hdr + 4, end, hdr[1], info->eh_frame_hdr_addr + 4, &eh_frame_addr))
			{
				info->eh_frame_addr = eh_frame_addr;
				if (info->eh_frame_size == 0 && info->eh_frame_hdr_addr > eh_frame_addr)
				{
					info->eh_frame_size = info->eh_frame_hdr_addr - eh_frame_addr;
				}
				else if (info->eh_frame_size == 0 &&
				         program->base_size_aligned > eh_frame_addr - program->base_vaddr)
				{
					// eh_frame sits after the header (common): size is the rest of the
					// image from the frame address, not from the header.
					info->eh_frame_size =
					    program->base_size_aligned - (eh_frame_addr - program->base_vaddr);
				}
			}
		}
	}

	if (image_start != UINT64_MAX && image_end > image_start)
	{
		info->seg0_addr = image_start;
		info->seg0_size = image_end - image_start;
	}

	if (info->eh_frame_addr != 0 && info->eh_frame_size == 0)
	{
		for (Loader::Elf64_Half i = 0; i < ehdr->e_phnum; i++)
		{
			if (phdr[i].p_memsz == 0 ||
			    (phdr[i].p_type != Loader::PT_LOAD && phdr[i].p_type != Loader::PT_OS_RELRO))
			{
				continue;
			}

			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			if (info->eh_frame_addr >= start && info->eh_frame_addr < end)
			{
				info->eh_frame_size = end - info->eh_frame_addr;
				break;
			}
		}
	}

	std::fprintf(stderr,
	             "GetModuleInfoForUnwind: addr=0x%016" PRIx64 " flags=%d name=%s "
	             "eh_hdr=0x%016" PRIx64 " eh=0x%016" PRIx64 " eh_sz=0x%016" PRIx64
	             " seg0=0x%016" PRIx64 " seg0_sz=0x%016" PRIx64 "\n",
	             addr, flags, info->name, info->eh_frame_hdr_addr, info->eh_frame_addr, info->eh_frame_size,
	             info->seg0_addr, info->seg0_size);
	std::fflush(stderr);

	return OK;
}

static void KYTY_SYSV_ABI KernelDebugRaiseExceptionOnReleaseMode(int /*c1*/, int /*c2*/)
{
	PRINT_NAME();
}

static void KYTY_SYSV_ABI KernelDebugRaiseException(uint64_t c1, uint64_t c2)
{
	PRINT_NAME();
	// Always emit on stderr: Silent PrintfDirection would otherwise hide the
	// codes, and returning here lands on the guest `ud2` after a noreturn call
	// (SIGILL). Keep this a structured EXIT so the next frontier is the raise
	// itself, not an opaque illegal-instruction trap.
	std::fprintf(stderr,
	             "KernelDebugRaiseException: error=0x%016" PRIx64 " arg=0x%016" PRIx64
	             ", return addr=%p\n",
	             c1, c2, __builtin_return_address(0));
	std::fflush(stderr);
	EXIT("KernelDebugRaiseException: error=0x%016" PRIx64 " arg=0x%016" PRIx64 "\n", c1, c2);
}

static bool is_allowed_exception_signal(int signum)
{
	return signum == 1 || signum == 4 || signum == 8 || signum == 10 || signum == 11 || signum == 30;
}

static std::array<void*, 128> g_exception_handlers {};
static std::mutex             g_exception_handlers_mutex;

struct SignalMcontext
{
	uint64_t mc_onstack;
	uint64_t mc_rdi;
	uint64_t mc_rsi;
	uint64_t mc_rdx;
	uint64_t mc_rcx;
	uint64_t mc_r8;
	uint64_t mc_r9;
	uint64_t mc_rax;
	uint64_t mc_rbx;
	uint64_t mc_rbp;
	uint64_t mc_r10;
	uint64_t mc_r11;
	uint64_t mc_r12;
	uint64_t mc_r13;
	uint64_t mc_r14;
	uint64_t mc_r15;
	int      mc_trapno;
	uint16_t mc_fs;
	uint16_t mc_gs;
	uint64_t mc_addr;
	int      mc_flags;
	uint16_t mc_es;
	uint16_t mc_ds;
	uint64_t mc_err;
	uint64_t mc_rip;
	uint64_t mc_cs;
	uint64_t mc_rflags;
	uint64_t mc_reserved[8];
	uint64_t mc_rsp;
	uint64_t mc_ss;
	uint64_t mc_len;
	uint64_t mc_fpformat;
	uint64_t mc_ownedfp;
	uint64_t mc_lbrfrom;
	uint64_t mc_lbrto;
	uint64_t mc_aux1;
	uint64_t mc_aux2;
	uint64_t mc_fpstate[104];
	uint64_t mc_fsbase;
	uint64_t mc_gsbase;
	uint64_t mc_spare[6];
};

struct SignalUcontext
{
	SignalMcontext uc_mcontext;
};

static_assert(offsetof(SignalMcontext, mc_rip) == 0xa0);
static_assert(offsetof(SignalMcontext, mc_rsp) == 0xf8);

static int KYTY_SYSV_ABI KernelInstallExceptionHandler(int signum, void* handler)
{
	PRINT_NAME();
	if (!is_allowed_exception_signal(signum) || handler == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	std::lock_guard lock(g_exception_handlers_mutex);
	if (g_exception_handlers[static_cast<size_t>(signum)] != nullptr)
	{
		return KERNEL_ERROR_EAGAIN;
	}
	g_exception_handlers[static_cast<size_t>(signum)] = handler;
	return OK;
}

static int KYTY_SYSV_ABI KernelRaiseException(Pthread thread, int signum)
{
	if (thread == nullptr || signum != 30)
	{
		return KERNEL_ERROR_EINVAL;
	}

	void* handler = nullptr;
	{
		std::lock_guard lock(g_exception_handlers_mutex);
		handler = g_exception_handlers[static_cast<size_t>(signum)];
	}
	if (handler == nullptr)
	{
		return OK;
	}

	SignalUcontext context {};
	context.uc_mcontext.mc_len = sizeof(SignalMcontext);
	context.uc_mcontext.mc_rsp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
	context.uc_mcontext.mc_rbp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
	context.uc_mcontext.mc_rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	using handler_func_t       = KYTY_SYSV_ABI void (*)(int, SignalUcontext*);
	reinterpret_cast<handler_func_t>(handler)(signum, &context);
	return OK;
}

static KYTY_SYSV_ABI int KernelIsAddressSanitizerEnabled()
{
	PRINT_NAME();
	return 0;
}

static void KYTY_SYSV_ABI exit(int code)
{
	PRINT_NAME();

	::exit(code);
}

static KYTY_SYSV_ABI MallocReplace* KernelGetSanitizerMallocReplaceExternal()
{
	PRINT_NAME();

	return nullptr;
}

static KYTY_SYSV_ABI NewReplace* KernelGetSanitizerNewReplaceExternal()
{
	PRINT_NAME();

	return nullptr;
}

static KYTY_SYSV_ABI int elf_phdr_match_addr(ModuleInfo* m, uint64_t dtor_vaddr)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(m == nullptr);

	auto* rt     = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto* p      = rt->FindProgramByAddr(dtor_vaddr);
	int   result = (p != nullptr && p->unique_id == m->handle) ? 1 : 0;

	printf("\thandle     = %" PRId32 "\n", m->handle);
	printf("\tdtor_vaddr = %016" PRIx64 "\n", dtor_vaddr);
	printf("\tmatch      = %s\n", result == 1 ? "true" : "false");

	return result;
}

int KYTY_SYSV_ABI KernelUuidCreate(uint32_t* uuid)
{
	PRINT_NAME();

	if (uuid == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uuid[0] = Kyty::Math::Rand::Uint();
	uuid[1] = Kyty::Math::Rand::Uint();
	uuid[2] = Kyty::Math::Rand::Uint();
	uuid[3] = Kyty::Math::Rand::Uint();

	return OK;
}

static KYTY_SYSV_ABI void pthread_cxa_finalize(void* /*p*/)
{
	PRINT_NAME();
}

void KYTY_SYSV_ABI KernelSetThreadAtexitCount(get_thread_atexit_count_func_t func)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_get_thread_atexit_count_func != nullptr);

	g_get_thread_atexit_count_func = func;
}

void KYTY_SYSV_ABI KernelSetThreadAtexitReport(thread_atexit_report_func_t func)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_thread_atexit_report_func != nullptr);

	g_thread_atexit_report_func = func;
}

int KYTY_SYSV_ABI KernelRtldThreadAtexitIncrement(uint64_t* /*c*/)
{
	PRINT_NAME();

	//__sync_fetch_and_add(c, 1);

	return 0;
}

int KYTY_SYSV_ABI KernelRtldThreadAtexitDecrement(uint64_t* /*c*/)
{
	PRINT_NAME();

	//__sync_fetch_and_sub(c, 1);

	return 0;
}

int KYTY_SYSV_ABI KernelIsNeoMode()
{
	PRINT_NAME();

	return (Config::IsNeo() ? 1 : 0);
}

int KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

void KYTY_SYSV_ABI KernelSetGPO(uint32_t bits)
{
	PRINT_NAME();

	printf("\t bits = %08" PRIx32 "\n", bits);
}

// sceKernelGetGPI — NID 4oXYe9Xmk0Q.
// On non-devkit retail consoles this is a no-op success (returns 0). Captured as
// The first strict Unpatched import from libkernel_v1.1 during early Main.
// Do not invent GPI state; no SetGPI pairing required for the observed open path.
static int KYTY_SYSV_ABI KernelGetGPI()
{
	PRINT_NAME();
	return KernelRetailGetGpiResult();
}

// sceKernelIsTrinityMode — NID tU5e3f9gSiU. Base PS5 mode reports 0.
static int KYTY_SYSV_ABI KernelIsTrinityMode()
{
	PRINT_NAME();
	return 0;
}

// sceKernelFsync — NID fTx66l5iWIA. Host has no guest-fd flush; accept valid fd.
static int KYTY_SYSV_ABI KernelFsync(int fd)
{
	PRINT_NAME();
	printf("\t fd = %d\n", fd);
	if (fd < 0)
	{
		return KERNEL_ERROR_EBADF;
	}
	return OK;
}

static int KYTY_SYSV_ABI PosixFsync(int fd)
{
	return POSIX_CALL(KernelFsync(fd));
}

static int KYTY_SYSV_ABI PosixFdatasync(int fd)
{
	return POSIX_CALL(KernelFsync(fd));
}

static int64_t KYTY_SYSV_ABI PosixFstat(int fd, FileSystem::FileStat* stat)
{
	return POSIX_N_CALL(FileSystem::KernelFstat(fd, stat));
}

static int KYTY_SYSV_ABI PosixFtruncate(int fd, int64_t length)
{
	return POSIX_CALL(FileSystem::KernelFtruncate(fd, length));
}

static int KYTY_SYSV_ABI PosixUnlink(const char* path)
{
	return POSIX_N_CALL(FileSystem::KernelUnlink(path));
}

static int KYTY_SYSV_ABI PosixRename(const char* from, const char* to)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelRename(from, to));
}

static int KYTY_SYSV_ABI PosixInetPton(int af, const char* src, void* dst)
{
	PRINT_NAME();
	if (src == nullptr || dst == nullptr)
	{
		return -1;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return ::inet_pton(af, src, dst);
#else
	EXIT_NOT_IMPLEMENTED(af != 2);
	(void)dst;
	return -1;
#endif
}

static void KYTY_SYSV_ABI PosixBzero(void* s, size_t n)
{
	if (s != nullptr && n != 0)
	{
		std::memset(s, 0, n);
	}
}

static int KYTY_SYSV_ABI PosixBind(int id, const void* addr, int len)
{
	return POSIX_NET_CALL(Network::Net::NetBind(id, addr, len));
}

static int KYTY_SYSV_ABI PosixConnect(int id, const void* addr, int len)
{
	return POSIX_NET_CALL(Network::Net::NetConnect(id, addr, len));
}

// Posix socket() is (family, type, protocol); sceNet_socket also takes a name.
static int KYTY_SYSV_ABI PosixSocket(int family, int type, int protocol)
{
	const auto result = Network::Net::NetSocket(nullptr, family, type, protocol);
	if (result < 0)
	{
		*Posix::GetErrorAddr() = Network::NetToPosix(result);
		return -1;
	}
	return result;
}

static int KYTY_SYSV_ABI PosixListen(int id, int backlog)
{
	return POSIX_NET_CALL(Network::Net::NetListen(id, backlog));
}

static int KYTY_SYSV_ABI PosixAccept(int id, void* addr, int* len)
{
	const auto result = Network::Net::NetAccept(id, addr, len);
	if (result < 0)
	{
		*Posix::GetErrorAddr() = Network::NetToPosix(result);
		return -1;
	}
	return result;
}

static int KYTY_SYSV_ABI PosixSetsockopt(int id, int level, int option, const void* value, int value_len)
{
	return POSIX_NET_CALL(Network::Net::NetSetsockopt(id, level, option, value, value_len));
}

static int64_t KYTY_SYSV_ABI PosixSend(int id, const void* buf, uint64_t len, int flags)
{
	const auto result = Network::Net::NetSend(id, buf, len, flags);
	if (result < 0)
	{
		*Posix::GetErrorAddr() = Network::NetToPosix(static_cast<int>(result));
		return -1;
	}
	return result;
}

static int64_t KYTY_SYSV_ABI PosixRecv(int id, void* buf, uint64_t len, int flags)
{
	const auto result = Network::Net::NetRecv(id, buf, len, flags);
	if (result < 0)
	{
		*Posix::GetErrorAddr() = Network::NetToPosix(static_cast<int>(result));
		return -1;
	}
	return result;
}

static int KYTY_SYSV_ABI PosixGetsockname(int id, void* addr, int* len)
{
	return POSIX_NET_CALL(Network::Net::NetGetsockname(id, addr, len));
}

static int KYTY_SYSV_ABI PosixGetsockopt(int id, int level, int option, void* value, int* value_len)
{
	return POSIX_NET_CALL(Network::Net::NetGetsockopt(id, level, option, value, value_len));
}

static int64_t KYTY_SYSV_ABI PosixRecvfrom(int socket, void* buffer, uint64_t length, int flags, void* address,
                                           uint32_t* address_len)
{
	PRINT_NAME();
	printf("\t socket      = %d\n", socket);
	printf("\t buffer      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	printf("\t length      = %" PRIu64 "\n", length);
	printf("\t flags       = %d\n", flags);
	printf("\t address     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t address_len = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address_len));
	EXIT("POSIX recvfrom is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI PosixSendmsg(int socket, const void* message, int flags)
{
	PRINT_NAME();
	printf("\t socket  = %d\n", socket);
	printf("\t message = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(message));
	printf("\t flags   = %d\n", flags);
	EXIT("POSIX sendmsg is not implemented\n");
	return -1;
}

static int64_t KYTY_SYSV_ABI PosixRecvmsg(int socket, void* message, int flags)
{
	PRINT_NAME();
	printf("\t socket  = %d\n", socket);
	printf("\t message = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(message));
	printf("\t flags   = %d\n", flags);
	EXIT("POSIX recvmsg is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI PosixSelect(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout)
{
	return POSIX_N_CALL(Network::Net::NetSelect(nfds, readfds, writefds, exceptfds, timeout));
}

static const char* KYTY_SYSV_ABI PosixInetNtop(int af, const void* src, char* dst, int size)
{
	return Network::Net::NetInetNtop(af, src, dst, size);
}

static uint16_t KYTY_SYSV_ABI PosixHtons(uint16_t hostshort)
{
	return Network::Net::NetHtons(hostshort);
}

static int KYTY_SYSV_ABI PthreadSemInit(void* sem, int pshared, unsigned int value, const char* /*name*/)
{
	if (pshared != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	return Posix::sem_init(sem, pshared, value);
}

} // namespace LibKernel

namespace Posix {

LIB_VERSION("Posix", 1, "libkernel", 1, 1);

int KYTY_SYSV_ABI getpid()
{
	constexpr int guest_process_id = 0xBAD1;
	return guest_process_id;
}

// Gen5 Posix_v1 pthread_self — NID EotR8a3ASf4. Same guest thread object as
// scePthreadSelf; Astro stores the returned pthread_t into audio context state.
LibKernel::Pthread KYTY_SYSV_ABI pthread_self()
{
	return LibKernel::PthreadSelf();
}

int KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int KYTY_SYSV_ABI clock_getres(int clock_id, LibKernel::KernelTimespec* res)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGetres(clock_id, res));
}

int KYTY_SYSV_ABI getpagesize()
{
	PRINT_NAME();

	return 0x4000;
}

// Gen5 Posix_v1 gettimeofday — NID n88vx3C5nW8. Optional timezone zero-filled.
struct KernelTimezone
{
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};

int KYTY_SYSV_ABI gettimeofday(LibKernel::KernelTimeval* time, KernelTimezone* timezone)
{
	PRINT_NAME();

	if (time != nullptr)
	{
		const int status = POSIX_CALL(LibKernel::KernelGettimeofday(time));
		if (status != 0)
		{
			return status;
		}
	}

	if (timezone != nullptr)
	{
		timezone->tz_minuteswest = 0;
		timezone->tz_dsttime     = 0;
	}

	return 0;
}

int KYTY_SYSV_ABI nanosleep(const LibKernel::KernelTimespec* rqtp, LibKernel::KernelTimespec* rmtp)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelNanosleep(rqtp, rmtp));
}

// Gen5 Posix_v1 usleep — NID QcteRwbsnV0 (Astro after Setschedparam; rdi=µs).
int KYTY_SYSV_ABI usleep(unsigned int microseconds)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelUsleep(microseconds));
}

int KYTY_SYSV_ABI stat(const char* path, LibKernel::FileSystem::FileStat* sb)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelStat(path, sb));
}

int KYTY_SYSV_ABI mkdir(const char* path, uint16_t mode)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelMkdir(path, mode));
}

int KYTY_SYSV_ABI flock(int descriptor, int operation)
{
	constexpr int kLockShared    = 0x01;
	constexpr int kLockExclusive = 0x02;
	constexpr int kLockNonBlock  = 0x04;
	constexpr int kLockUnlock    = 0x08;

	const int lock_mode = operation & ~kLockNonBlock;
	if (lock_mode != kLockShared && lock_mode != kLockExclusive && lock_mode != kLockUnlock)
	{
		*GetErrorAddr() = POSIX_EINVAL;
		return -1;
	}

	// Guest file descriptors are emulator-managed handles, so validating the
	// descriptor is sufficient for this single-process runtime. The lock has no
	// additional host-visible effect until multi-process guest execution exists.
	LibKernel::FileSystem::FileStat stat {};
	const int                       result = LibKernel::FileSystem::KernelFstat(descriptor, &stat);
	if (result != OK)
	{
		*GetErrorAddr() = LibKernel::KernelToPosix(result);
		return -1;
	}

	return 0;
}

// Gen5 import NID cfwBSQyr5Ys: public name unresolved. Log args and return 0 so
// loaders that import it can continue.
uint64_t KYTY_SYSV_ABI cfwBSQyr5Ys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                   uint64_t a6)
{
	PRINT_NAME();
	printf("\t a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 "\n"
	       "\t a4=0x%016" PRIx64 " a5=0x%016" PRIx64 " a6=0x%016" PRIx64 "\n",
	       a1, a2, a3, a4, a5, a6);
	return 0;
}

LIB_DEFINE(InitLibKernel_1_Posix)
{
	LIB_FUNC("lLMT9vJAck0", clock_gettime);
	LIB_FUNC("smIj7eqzZE8", clock_getres);
	// Gen5 Posix_v1 gettimeofday — n88vx3C5nW8.
	LIB_FUNC("n88vx3C5nW8", gettimeofday);
	LIB_FUNC("yS8U2TGCe1A", nanosleep);
	LIB_FUNC("NhpspxdjEKU", nanosleep);
	// Gen5 Posix_v1 usleep — QcteRwbsnV0 after Setschedparam assert fix.
	LIB_FUNC("QcteRwbsnV0", usleep);
	LIB_FUNC("k+AXqu2-eBc", getpagesize);
	LIB_FUNC("E6ao34wPw+U", stat);
	LIB_FUNC("JGMio+21L4c", mkdir);
	LIB_FUNC("9eMlfusH4sU", flock);
	LIB_FUNC("HoLVWNanBBc", getpid);
	// Gen5 Posix_v1 pthread_self — EotR8a3ASf4 (Astro audio path after Acm).
	LIB_FUNC("EotR8a3ASf4", pthread_self);
	// Gen5 Posix_v1 pthread_attr_* (Astro after package path bring-up).
	LIB_FUNC("wtkt-teR1so", Posix::pthread_attr_init);
	LIB_FUNC("zHchY8ft5pk", Posix::pthread_attr_destroy);
	LIB_FUNC("vQm4fDEsWi8", Posix::pthread_attr_getstack);
	LIB_FUNC("2Q0z6rnBrTE", Posix::pthread_attr_setstacksize);
	LIB_FUNC("0qOtCR-ZHck", Posix::pthread_attr_getstacksize);
	LIB_FUNC("Ucsu-OK+els", Posix::pthread_attr_get_np);
	LIB_FUNC("RtLRV-pBTTY", Posix::pthread_attr_getschedpolicy);
	LIB_FUNC("JarMIy8kKEY", Posix::pthread_attr_setschedpolicy);
	LIB_FUNC("E+tyo3lp5Lw", Posix::pthread_attr_setdetachstate);
	LIB_FUNC("VUT1ZSrHT0I", Posix::pthread_attr_getdetachstate);
	LIB_FUNC("euKRgm0Vn2M", Posix::pthread_attr_setschedparam);
	LIB_FUNC("qlk9pSLsUmM", Posix::pthread_attr_getschedparam);
	LIB_FUNC("FXPWHNk8Of0", Posix::pthread_attr_getschedparam);
	LIB_FUNC("7ZlAakEf0Qg", Posix::pthread_attr_setinheritsched);
	LIB_FUNC("JKyG3SWyA10", Posix::pthread_attr_setguardsize);
	LIB_FUNC("JNkVVsVDmOk", Posix::pthread_attr_getguardsize);

	LIB_FUNC("OxhIB8LB-PQ", Posix::pthread_create);
	LIB_FUNC("Jmi+9w9u0E4", Posix::pthread_create_name_np);
	LIB_FUNC("h9CcP3J0oVM", Posix::pthread_join);
	// Gen5 Posix_v1 thread control (Astro after attr_setstacksize).
	LIB_FUNC("+U1R4WtXvoc", Posix::pthread_detach);
	LIB_FUNC("FJrT5LuUBAU", Posix::pthread_exit);
	LIB_FUNC("B5GmVDKwpn0", Posix::pthread_yield);
	LIB_FUNC("6XG4B33N09g", Posix::sched_yield);
	LIB_FUNC("7Xl257M4VNI", Posix::pthread_equal);
	LIB_FUNC("lZzFeSxPl08", Posix::pthread_setcancelstate);
	LIB_FUNC("a2P9wYGeZvc", Posix::pthread_setprio);
	LIB_FUNC("0TyVk4MSLt0", Posix::pthread_cond_init);
	LIB_FUNC("RXXqi4CtF8w", Posix::pthread_cond_destroy);
	LIB_FUNC("2MOy+rUfuhQ", Posix::pthread_cond_signal);
	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("K-jXhbt2gn4", Posix::pthread_mutex_trylock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("ttHNfU+qDBU", Posix::pthread_mutex_init);
	LIB_FUNC("dQHWEsJtoE4", Posix::pthread_mutexattr_init);
	LIB_FUNC("mDmgMOGVUqg", Posix::pthread_mutexattr_settype);
	LIB_FUNC("HF7lK46xzjY", Posix::pthread_mutexattr_destroy);
	LIB_FUNC("ltCfaGr2JGE", Posix::pthread_mutex_destroy);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
	LIB_FUNC("27bAgiJmOh0", Posix::pthread_cond_timedwait);
	LIB_FUNC("Z4QosVuAsA0", Posix::pthread_once);
	LIB_FUNC("mqULNdimTn0", Posix::pthread_key_create);
	LIB_FUNC("6BpEZuDT7YI", Posix::pthread_key_delete);
	LIB_FUNC("WrOLvHU0yQM", Posix::pthread_setspecific);
	LIB_FUNC("0-KXaS70xy4", Posix::pthread_getspecific);

	// Posix pthread_rwlock_* / attr NIDs (libkernel + libScePosix shared NID table).
	LIB_FUNC("1471ajPzxh0", Posix::pthread_rwlock_destroy);
	LIB_FUNC("ytQULN-nhL4", Posix::pthread_rwlock_init);
	LIB_FUNC("iGjsr1WAtI0", Posix::pthread_rwlock_rdlock);
	LIB_FUNC("lb8lnYo-o7k", Posix::pthread_rwlock_timedrdlock);
	LIB_FUNC("9zklzAl9CGM", Posix::pthread_rwlock_timedwrlock);
	LIB_FUNC("SFxTMOfuCkE", Posix::pthread_rwlock_tryrdlock);
	LIB_FUNC("XhWHn6P5R7U", Posix::pthread_rwlock_trywrlock);
	LIB_FUNC("EgmLo6EWgso", Posix::pthread_rwlock_unlock);
	LIB_FUNC("sIlRvQqsN2Y", Posix::pthread_rwlock_wrlock);
	LIB_FUNC("qsdmgXjqSgk", Posix::pthread_rwlockattr_destroy);
	LIB_FUNC("VqEMuCv-qHY", Posix::pthread_rwlockattr_getpshared);
	LIB_FUNC("l+bG5fsYkhg", Posix::pthread_rwlockattr_gettype_np);
	LIB_FUNC("xFebsA4YsFI", Posix::pthread_rwlockattr_init);
	LIB_FUNC("OuKg+kRDD7U", Posix::pthread_rwlockattr_setpshared);
	LIB_FUNC("8NuOHiTr1Vw", Posix::pthread_rwlockattr_settype_np);

	// Gen5 Posix_v1 semaphore NIDs (Astro hard-abort pDuPEf3m4fI = sem_init).
	LIB_FUNC("pDuPEf3m4fI", Posix::sem_init);
	LIB_FUNC("cDW233RAwWo", Posix::sem_destroy);
	LIB_FUNC("YCV5dGGBcCo", Posix::sem_wait);
	LIB_FUNC("WBWzsRifCEA", Posix::sem_trywait);
	LIB_FUNC("4SbrhCozqQU", Posix::sem_reltimedwait_np);
	LIB_FUNC("w5IHyvahg-o", Posix::sem_timedwait);
	LIB_FUNC("Bq+LRV-N6Hk", Posix::sem_getvalue);
	LIB_FUNC("IKP8typ0QUk", Posix::sem_post);

	// Posix FS dual NIDs (same ABI as sceKernelOpen/Close/Read/Write/Pread/Pwrite).
	LIB_FUNC("wuCroIGjt2g", LibKernel::open);
	LIB_FUNC("bY-PO6JhzhQ", LibKernel::close);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("mqQMh1zPPT8", LibKernel::PosixFstat);
	LIB_FUNC("ih4CD9-gghM", LibKernel::PosixFtruncate);
	LIB_FUNC("VAzswvTOCzI", LibKernel::PosixUnlink);
	LIB_FUNC("FN4gaPmuFV8", LibKernel::write);
	LIB_FUNC("ezv-RSBNKqI", LibKernel::pread);
	LIB_FUNC("C2kJ-byS5rM", LibKernel::pwrite);
	LIB_FUNC("iiQjzvfWDq0", LibKernel::dup);
	LIB_FUNC("wdUufa9g-D8", LibKernel::dup2);
	LIB_FUNC("ku7D4q1Y9PI", LibKernel::poll);
	// Gen5 Posix rename / sync / inet / bzero dual NIDs.
	LIB_FUNC("NN01qLRhiqU", LibKernel::PosixRename);
	LIB_FUNC("juWbTNM+8hw", LibKernel::PosixFsync);
	LIB_FUNC("KIbJFQ0I1Cg", LibKernel::PosixFdatasync);
	LIB_FUNC("4n51s0zEf0c", LibKernel::PosixInetPton);
	LIB_FUNC("9oiX1kyeedA", LibKernel::PosixBzero);
	// Gen5 Posix_v1 socket dual NIDs (libkernel; ABI matches sceNet stubs).
	LIB_FUNC("TU-d9PfIHPM", LibKernel::PosixSocket);
	LIB_FUNC("KuOmgKoqCdY", LibKernel::PosixBind);
	LIB_FUNC("XVL8So3QJUk", LibKernel::PosixConnect);
	LIB_FUNC("pxnCmagrtao", LibKernel::PosixListen);
	LIB_FUNC("3e+4Iv7IJ8U", LibKernel::PosixAccept);
	LIB_FUNC("RenI1lL1WFk", LibKernel::PosixGetsockname);
	LIB_FUNC("6O8EwYOgH9Y", LibKernel::PosixGetsockopt);
	LIB_FUNC("lUk6wrGXyMw", LibKernel::PosixRecvfrom);
	LIB_FUNC("aNeavPDNKzA", LibKernel::PosixSendmsg);
	LIB_FUNC("hI7oVeOluPM", LibKernel::PosixRecvmsg);
	LIB_FUNC("fFxGkxF2bVo", LibKernel::PosixSetsockopt);
	LIB_FUNC("fZOeZIOEmLw", LibKernel::PosixSend);
	LIB_FUNC("Ez8xjo9UF4E", LibKernel::PosixRecv);
	LIB_FUNC("T8fER+tIGgk", LibKernel::PosixSelect);
	LIB_FUNC("5jRCs2axtr4", LibKernel::PosixInetNtop);
	LIB_FUNC("jogUIsOV3-U", LibKernel::PosixHtons);
	// Gen5 Posix residual: name unresolved; callers treat 0 as success (Unity/IL2CPP path).
	LIB_FUNC("cfwBSQyr5Ys", Posix::cfwBSQyr5Ys);
	// Gen5 pthread_getthreadid_np / rename / schedparam / mutexattr_setprotocol.
	LIB_FUNC("3eqs37G74-s", Posix::pthread_getthreadid_np);
	LIB_FUNC("9vyP6Z7bqzc", Posix::pthread_rename_np);
	LIB_FUNC("FIs3-UQT9sg", Posix::pthread_getschedparam);
	LIB_FUNC("Xs9hdiD7sAA", Posix::pthread_setschedparam);
	LIB_FUNC("5txKfcMUAok", Posix::pthread_mutexattr_setprotocol);
}

} // namespace Posix

namespace FileSystem = LibKernel::FileSystem;
namespace Memory     = LibKernel::Memory;
namespace SyncOnAddress = LibKernel::SyncOnAddress;
namespace EventQueue = LibKernel::EventQueue;
namespace EventFlag  = LibKernel::EventFlag;
namespace Semaphore  = LibKernel::Semaphore;

LIB_DEFINE(InitLibKernel_1_FS)
{
	LIB_FUNC("1G3lF1Gg1k8", FileSystem::KernelOpen);
	LIB_FUNC("UK2Tl2DWUns", FileSystem::KernelClose);
	LIB_FUNC("Cg4srZ6TKbU", FileSystem::KernelRead);
	LIB_FUNC("4wSze92BhLI", FileSystem::KernelWrite);
	LIB_FUNC("+r3rMFwItV4", FileSystem::KernelPread);
	LIB_FUNC("nKWi-N2HBV4", FileSystem::KernelPwrite);
	LIB_FUNC("eV9wAD2riIA", FileSystem::KernelStat);
	LIB_FUNC("kBwCPsYX-m4", FileSystem::KernelFstat);
	LIB_FUNC("VW3TVZiM4-E", FileSystem::KernelFtruncate);
	LIB_FUNC("AUXVxWeJU-A", FileSystem::KernelUnlink);
	LIB_FUNC("52NcYU9+lEo", FileSystem::KernelRename);
	LIB_FUNC("taRWhTJFTgE", FileSystem::KernelGetdirentries);
	LIB_FUNC("oib76F-12fk", FileSystem::KernelLseek);
	LIB_FUNC("Oy6IpwgtYOk", LibKernel::lseek);
	LIB_FUNC("j2AIqSqJP0w", FileSystem::KernelGetdents);
	LIB_FUNC("1-LFLmRFxxM", FileSystem::KernelMkdir);
	LIB_FUNC("naInUjYt3so", FileSystem::KernelRmdir);
	// Gen5 APR path resolution / submit / wait (libkernel APR family).
	LIB_FUNC("gEpBkcwxUjw", FileSystem::KernelAprResolveFilepathsToIdsAndFileSizes);
	LIB_FUNC("WT-5NKy42fw", FileSystem::KernelAprResolveFilepathsToIds);
	LIB_FUNC("i3HWvW35jao", FileSystem::KernelAprResolveFilepathsWithPrefixToIds);
	LIB_FUNC("w5fcCG+t31g", FileSystem::KernelAprResolveFilepathsWithPrefixToIdsAndFileSizes);
	LIB_FUNC("eYAh2vlCY-U", FileSystem::KernelAprResolveFilepathsToIdsForEach);
	LIB_FUNC("QzB4O+bJQyA", FileSystem::KernelAprResolveFilepathsToIdsAndFileSizesForEach);
	LIB_FUNC("VB-BtuIW8Xc", FileSystem::KernelAprResolveFilepathsWithPrefixToIdsForEach);
	LIB_FUNC("C+Khtbbx2g8", FileSystem::KernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach);
	LIB_FUNC("ApkYaHb8Sek", FileSystem::KernelAprGetFileStat);
	LIB_FUNC("WvEu7yl3Ivg", FileSystem::KernelAprGetFileSize);
	LIB_FUNC("eE4Szl8sil8", FileSystem::KernelAprSubmitCommandBuffer);
	LIB_FUNC("ASoW5WE-UPo", FileSystem::KernelAprSubmitCommandBufferAndGetResult);
	LIB_FUNC("qvMUCyyaCSI", FileSystem::KernelAprSubmitCommandBufferAndGetId);
	LIB_FUNC("rqwFKI4PAiM", FileSystem::KernelAprWaitCommandBuffer);

	// Gen5 kernel mode / fd flush.
	LIB_FUNC("tU5e3f9gSiU", LibKernel::KernelIsTrinityMode);
	LIB_FUNC("fTx66l5iWIA", LibKernel::KernelFsync);
}

LIB_DEFINE(InitLibKernel_1_Mem)
{
	LIB_FUNC("mL8NDH86iQI", Memory::KernelMapNamedFlexibleMemory);
	LIB_FUNC("IWIBBdTHit4", Memory::KernelMapFlexibleMemory);
	LIB_FUNC("cQke9UuBQOk", Memory::KernelMunmap);
	LIB_FUNC("pO96TwzOm5E", Memory::KernelGetDirectMemorySize);
	LIB_FUNC("rTXw65xmLIA", Memory::KernelAllocateDirectMemory);
	LIB_FUNC("B+vc2AO2Zrc", Memory::KernelAllocateMainDirectMemory);
	LIB_FUNC("L-Q3LEjIbgA", Memory::KernelMapDirectMemory);
	LIB_FUNC("BQQniolj9tQ", Memory::KernelMapDirectMemory2);
	LIB_FUNC("NcaWUxfMNIQ", Memory::KernelMapNamedDirectMemory);
	LIB_FUNC("MBuItvba6z8", Memory::KernelReleaseDirectMemory);
	LIB_FUNC("hwVSPCmp5tM", Memory::KernelCheckedReleaseDirectMemory);
	LIB_FUNC("WFcfL2lzido", Memory::KernelQueryMemoryProtection);
	LIB_FUNC("BHouLQzh0X0", Memory::KernelDirectMemoryQuery);
	LIB_FUNC("C0f7TJcbfac", Memory::KernelAvailableDirectMemorySize);
	LIB_FUNC("kBJzF8x4SyE", Memory::KernelBatchMap2);
	LIB_FUNC("aNz11fnnzi4", Memory::KernelAvailableFlexibleMemorySize);
	LIB_FUNC("n1-v6FgU7MQ", Memory::KernelConfiguredFlexibleMemorySize);
	LIB_FUNC("DGMG3JshrZU", Memory::KernelSetVirtualRangeName);
	LIB_FUNC("mkgXxsoxWHg", Memory::KernelClearVirtualRangeName);
	LIB_FUNC("rVjRvHJ0X6c", Memory::KernelVirtualQuery);
	LIB_FUNC("yDBwVAolDgg", Memory::KernelIsStack);
	LIB_FUNC("7oxv3PPCumo", Memory::KernelReserveVirtualRange);
	LIB_FUNC("vSMAm3cxYTY", Memory::KernelMprotect);
	// Posix dual NIDs for the same Orbis memory helpers.
	LIB_FUNC("YQOfxL4QfeU", Memory::KernelMprotect);
	LIB_FUNC("UqDGjXA5yUM", Memory::KernelMunmap);
}

LIB_DEFINE(InitLibKernel_1_Equeue)
{
	LIB_FUNC("D0OdFMjp46I", EventQueue::KernelCreateEqueue);
	LIB_FUNC("jpFjmgAC5AE", EventQueue::KernelDeleteEqueue);
	LIB_FUNC("fzyMKs9kim0", EventQueue::KernelWaitEqueue);
	LIB_FUNC("vz+pg2zdopI", EventQueue::KernelGetEventUserData);
	LIB_FUNC("mJ7aghmgvfc", EventQueue::KernelGetEventId);
	LIB_FUNC("kwGyyjohI50", EventQueue::KernelGetEventData);
	LIB_FUNC("23CPPI1tyBY", EventQueue::KernelGetEventFilter);
	LIB_FUNC("Q0qr9AyqJSk", EventQueue::KernelGetEventFflags);
	LIB_FUNC("Uu-iDFC9aUc", EventQueue::KernelGetEventError);
	LIB_FUNC("4R6-OvI2cEA", EventQueue::KernelAddUserEvent);
	LIB_FUNC("WDszmSbWuDk", EventQueue::KernelAddUserEventEdge);
	LIB_FUNC("F6e0kwo4cnk", EventQueue::KernelTriggerUserEvent);
	LIB_FUNC("LJDwdSNTnDg", EventQueue::KernelDeleteUserEvent);
	// Gen5 Ampr completion equeue (sceKernelAdd/DeleteAmprEvent).
	LIB_FUNC("bBfz7kMF2Ho", EventQueue::KernelAddAmprEvent);
	LIB_FUNC("bMmid3pfyjo", EventQueue::KernelDeleteAmprEvent);
}

LIB_DEFINE(InitLibKernel_1_EventFlag)
{
	LIB_FUNC("BpFoboUJoZU", EventFlag::KernelCreateEventFlag);
	LIB_FUNC("JTvBflhYazQ", EventFlag::KernelWaitEventFlag);
	LIB_FUNC("IOnSvHzqu6A", EventFlag::KernelSetEventFlag);
	// Gen5 NID for delete (strict Unpatched otherwise).
	LIB_FUNC("8mql9OcQnd4", EventFlag::KernelDeleteEventFlag);
	LIB_FUNC("9lvj5DjHZiA", EventFlag::KernelPollEventFlag);
	LIB_FUNC("7uhBFWRAS60", EventFlag::KernelClearEventFlag);
	LIB_FUNC("PZku4ZrXJqg", EventFlag::KernelCancelEventFlag);
}

LIB_DEFINE(InitLibKernel_1_Semaphore)
{
	LIB_FUNC("188x57JYp0g", Semaphore::KernelCreateSema);
	LIB_FUNC("R1Jvn8bSCW8", Semaphore::KernelDeleteSema);
	LIB_FUNC("Zxa0VhQVTsk", Semaphore::KernelWaitSema);
	LIB_FUNC("4czppHBiriw", Semaphore::KernelSignalSema);
	LIB_FUNC("12wOHk8ywb0", Semaphore::KernelPollSema);
	LIB_FUNC("4DM06U2BNEY", Semaphore::KernelCancelSema);
}

LIB_DEFINE(InitLibKernel_1_Pthread)
{
	LIB_FUNC("9UK1vLZQft4", LibKernel::PthreadMutexLock);
	LIB_FUNC("tn3VlD0hG60", LibKernel::PthreadMutexUnlock);
	LIB_FUNC("2Of0f+3mhhE", LibKernel::PthreadMutexDestroy);
	LIB_FUNC("cmo1RIYva9o", LibKernel::PthreadMutexInit);
	LIB_FUNC("upoVrzMHFeE", LibKernel::PthreadMutexTrylock);
	LIB_FUNC("smWEktiyyG0", LibKernel::PthreadMutexattrDestroy);
	LIB_FUNC("F8bUHwAG284", LibKernel::PthreadMutexattrInit);
	LIB_FUNC("iMp8QpE+XO4", LibKernel::PthreadMutexattrSettype);
	LIB_FUNC("1FGvU0i9saQ", LibKernel::PthreadMutexattrSetprotocol);

	LIB_FUNC("aI+OeCz8xrQ", LibKernel::PthreadSelf);
	LIB_FUNC("6UgtwV+0zb4", LibKernel::PthreadCreate);
	LIB_FUNC("3PtV6p3QNX4", LibKernel::PthreadEqual);
	LIB_FUNC("onNY9Byn-W8", LibKernel::PthreadJoin);
	LIB_FUNC("3kg7rT0NQIs", LibKernel::PthreadExit);
	LIB_FUNC("4qGrR6eoP9Y", LibKernel::PthreadDetach);
	LIB_FUNC("How7B8Oet6k", LibKernel::PthreadGetname);
	LIB_FUNC("GBUY7ywdULE", LibKernel::PthreadRename);
	LIB_FUNC("rcrVFJsQWRY", LibKernel::PthreadGetaffinity);
	LIB_FUNC("P41kTWUS3EI", LibKernel::PthreadGetschedparam);
	LIB_FUNC("oIRFTjoILbg", LibKernel::PthreadSetschedparam);
	LIB_FUNC("bt3CTBKmGyI", LibKernel::PthreadSetaffinity);
	LIB_FUNC("1tKyG7RlMJo", LibKernel::PthreadGetprio);
	LIB_FUNC("W0Hpm2X0uPE", LibKernel::PthreadSetprio);
	LIB_FUNC("T72hz6ffq08", LibKernel::PthreadYield);
	LIB_FUNC("EI-5-jlq2dE", LibKernel::PthreadGetthreadid);

	LIB_FUNC("62KCwEMmzcM", LibKernel::PthreadAttrDestroy);
	LIB_FUNC("x1X76arYMxU", LibKernel::PthreadAttrGet);
	LIB_FUNC("8+s5BzZjxSg", LibKernel::PthreadAttrGetaffinity);
	LIB_FUNC("nsYoNRywwNg", LibKernel::PthreadAttrInit);
	LIB_FUNC("JaRMy+QcpeU", LibKernel::PthreadAttrGetdetachstate);
	LIB_FUNC("FXPWHNk8Of0", LibKernel::PthreadAttrGetschedparam);
	LIB_FUNC("Ru36fiTtJzA", LibKernel::PthreadAttrGetstackaddr);
	LIB_FUNC("-fA+7ZlGDQs", LibKernel::PthreadAttrGetstacksize);
	// Captured post-TLS REX fix on Gen5 eboot: worker thread after Attr set/get
	// stack fields calls a 3-arg PLT (attr, void**, size_t*) matching
	// PthreadAttrGetstack; import NID -quPa4SEJUw (strict Unpatched otherwise).
	LIB_FUNC("-quPa4SEJUw", LibKernel::PthreadAttrGetstack);
	LIB_FUNC("txHtngJ+eyc", LibKernel::PthreadAttrGetguardsize);
	LIB_FUNC("UTXzJbWhhTE", LibKernel::PthreadAttrSetstacksize);
	// Gen5 NID (stack addr+size in one call).
	LIB_FUNC("Bvn74vj6oLo", LibKernel::PthreadAttrSetstack);
	LIB_FUNC("-Wreprtu0Qs", LibKernel::PthreadAttrSetdetachstate);
	LIB_FUNC("El+cQ20DynU", LibKernel::PthreadAttrSetguardsize);
	LIB_FUNC("eXbUSpEaTsA", LibKernel::PthreadAttrSetinheritsched);
	LIB_FUNC("DzES9hQF4f4", LibKernel::PthreadAttrSetschedparam);
	LIB_FUNC("4+h9EzwKF4I", LibKernel::PthreadAttrSetschedpolicy);
	LIB_FUNC("3qxgM4ezETA", LibKernel::PthreadAttrSetaffinity);

	LIB_FUNC("6ULAa0fq4jA", LibKernel::PthreadRwlockInit);
	LIB_FUNC("BB+kb08Tl9A", LibKernel::PthreadRwlockDestroy);
	LIB_FUNC("Ox9i0c7L5w0", LibKernel::PthreadRwlockRdlock);
	LIB_FUNC("+L98PIbGttk", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC("mqdNorrB+gI", LibKernel::PthreadRwlockWrlock);
	// Gen5 rwlock / attr NIDs observed as strict Unpatched imports.
	LIB_FUNC("bIHoZCTomsI", LibKernel::PthreadRwlockTrywrlock);
	// Gen5 scePthreadRwlockTryrdlock — NID XD3mDeybCnk.
	LIB_FUNC("XD3mDeybCnk", LibKernel::PthreadRwlockTryrdlock);
	// Orbis timedrd/wr NIDs (iPtZRWICjrM / adh--6nIqTk) stay unregistered until
	// usec-vs-abstime ABI is evidenced; Kyty Timed* helpers currently take usec.
	LIB_FUNC("i2ifZ3fS2fo", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC("yOfGg-I1ZII", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC("LcOZBHGqbFk", LibKernel::PthreadRwlockattrGetpshared);
	LIB_FUNC("Kyls1ChFyrc", LibKernel::PthreadRwlockattrGettype);
	LIB_FUNC("-ZvQH18j10c", LibKernel::PthreadRwlockattrSetpshared);
	LIB_FUNC("h-OifiouBd8", LibKernel::PthreadRwlockattrSettype);

	LIB_FUNC("2Tb92quprl0", LibKernel::PthreadCondInit);
	LIB_FUNC("g+PZd2hiacg", LibKernel::PthreadCondDestroy);
	LIB_FUNC("WKAXJ4XBPQ4", LibKernel::PthreadCondWait);
	LIB_FUNC("JGgj7Uvrl+A", LibKernel::PthreadCondBroadcast);
	LIB_FUNC("kDh-NfxgMtE", LibKernel::PthreadCondSignal);
	LIB_FUNC("o69RpYO-Mu0", LibKernel::PthreadCondSignalto);
	LIB_FUNC("BmMjYxmew1w", LibKernel::PthreadCondTimedwait);
	LIB_FUNC("14bOACANTBo", LibKernel::PthreadOnce);
	LIB_FUNC("m5-2bsNfv7s", LibKernel::PthreadCondattrInit);
	LIB_FUNC("waPcxYiR3WA", LibKernel::PthreadCondattrDestroy);

	// Gen5 TLS key + timed mutex NIDs.
	LIB_FUNC("geDaqgH9lTg", LibKernel::PthreadKeyCreate);
	LIB_FUNC("PrdHuuDekhY", LibKernel::PthreadKeyDelete);
	LIB_FUNC("+BzXYkqYeLE", LibKernel::PthreadSetspecific);
	LIB_FUNC("eoht7mQOCmo", LibKernel::PthreadGetspecific);
	LIB_FUNC("IafI2PxcPnQ", LibKernel::PthreadMutexTimedlock);

	// Gen5 scePthreadSem* aliases the Posix semaphore ABI.
	LIB_FUNC("GEnUkDZoUwY", LibKernel::PthreadSemInit);
	LIB_FUNC("C36iRE0F5sE", Posix::sem_wait);
	LIB_FUNC("H2a+IN9TP0E", Posix::sem_trywait);
	LIB_FUNC("aishVAiFaYM", Posix::sem_post);
	LIB_FUNC("Vwc+L05e6oE", Posix::sem_destroy);

	LIB_FUNC("QBi7HCK03hw", LibKernel::KernelClockGettime);
	LIB_FUNC("ejekcaNQNq0", LibKernel::KernelGettimeofday);
	LIB_FUNC("-2IRUCO--PM", LibKernel::KernelReadTsc);
	LIB_FUNC("1j3S3n-tTW4", LibKernel::KernelGetTscFrequency);
	LIB_FUNC("4J2sUJmuHZQ", LibKernel::KernelGetProcessTime);
	LIB_FUNC("BNowx2l588E", LibKernel::KernelGetProcessTimeCounterFrequency);
	LIB_FUNC("fgxnMeTNUtY", LibKernel::KernelGetProcessTimeCounter);

	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
}

LIB_DEFINE(InitLibKernel_1)
{
	InitLibKernel_1_FS(s);
	InitLibKernel_1_Mem(s);
	InitLibKernel_1_Equeue(s);
	InitLibKernel_1_EventFlag(s);
	InitLibKernel_1_Semaphore(s);
	InitLibKernel_1_Pthread(s);
	Posix::InitLibKernel_1_Posix(s);

	LIB_OBJECT("f7uOxY9mM1U", &LibKernel::g_stack_chk_guard);
	LIB_OBJECT("djxxOmW6-aw", &LibKernel::g_progname);

	LIB_FUNC("1jfXLRVzisc", LibKernel::KernelUsleep);
	LIB_FUNC("-ZR+hG7aDHw", LibKernel::KernelSleep);
	// sceKernelNanosleep (NID verified against the public PS5 stub name).
	LIB_FUNC("QvsZxomvUHs", LibKernel::KernelNanosleep);
	LIB_FUNC("6c3rCVE-fTU", LibKernel::open);
	LIB_FUNC("6xVpy0Fdq+I", LibKernel::sigprocmask);
	LIB_FUNC("6Z83sYWFlA8", LibKernel::exit);
	LIB_FUNC("8OnWXlgQlvo", LibKernel::KernelRtldThreadAtexitDecrement);
	LIB_FUNC("959qrazPIrg", LibKernel::KernelGetProcParam);
	LIB_FUNC("LwG8g3niqwA", LibKernel::KernelDlsym);
	LIB_FUNC("9BcDykPmo1I", LibKernel::get_error_addr);
	LIB_FUNC("HoLVWNanBBc", Posix::getpid);
	LIB_FUNC("bnZxYgAFeA0", LibKernel::KernelGetSanitizerNewReplaceExternal);
	LIB_FUNC("ca7v6Cxulzs", LibKernel::KernelSetGPO);
	// sceKernelGetGPI (PS5 stub name ↔ NID 4oXYe9Xmk0Q). Retail no-op success.
	LIB_FUNC("4oXYe9Xmk0Q", LibKernel::KernelGetGPI);
	LIB_FUNC("DRuBt2pvICk", LibKernel::read);
	LIB_FUNC("f7KBOafysXo", LibKernel::KernelGetModuleInfoFromAddr);
	LIB_FUNC("RpQJJVKTiFM", LibKernel::KernelGetModuleInfoForUnwind);
	LIB_FUNC("kUpgrXIrz7Q", LibKernel::KernelGetModuleInfo);
	LIB_FUNC("IuxnUuXk6Bg", LibKernel::KernelGetModuleList);
	LIB_FUNC("Hc4CaR6JBL0", LibKernel::SyncOnAddress::KernelSyncOnAddressWait);
	LIB_FUNC("q2y-wDIVWZA", LibKernel::SyncOnAddress::KernelSyncOnAddressWake);
	LIB_FUNC("Fjc4-n1+y2g", LibKernel::elf_phdr_match_addr);
	LIB_FUNC("FxVZqBAA7ks", LibKernel::write);
	LIB_FUNC("kbw4UHHSYy0", LibKernel::pthread_cxa_finalize);
	LIB_FUNC("lLMT9vJAck0", LibKernel::clock_gettime);
	LIB_FUNC("NNtFaKJbPt0", LibKernel::close);
	LIB_FUNC("OMDRKKAZ8I4", LibKernel::KernelDebugRaiseException);
	LIB_FUNC("il03nluKfMk", LibKernel::KernelRaiseException);
	LIB_FUNC("WkwEd3N7w0Y", LibKernel::KernelInstallExceptionHandler);
	LIB_FUNC("jh+8XiK4LeE", LibKernel::KernelIsAddressSanitizerEnabled);
	LIB_FUNC("-o5uEDpN+oY", LibKernel::KernelConvertUtcToLocaltime);
	// sceKernelMapNamedFlexibleMemoryInternal uses the same out-pointer ABI as
	// the public named-flexible mapper.
	LIB_FUNC("4h6F1LLbTiw", Memory::KernelMapNamedFlexibleMemory);
	LIB_FUNC("Ou3iL1abvng", LibKernel::stack_chk_fail);
	LIB_FUNC("p5EcQeEeJAE", LibKernel::KernelRtldSetApplicationHeapAPI);
	LIB_FUNC("pB-yGZ2nQ9o", LibKernel::KernelSetThreadAtexitCount);
	LIB_FUNC("py6L8jiVAN8", LibKernel::KernelGetSanitizerMallocReplaceExternal);
	LIB_FUNC("QKd0qM58Qes", LibKernel::KernelStopUnloadModule);
	LIB_FUNC("rNhWz+lvOMU", LibKernel::KernelSetThreadDtors);
	LIB_FUNC("Tz4RNUCBbGI", LibKernel::KernelRtldThreadAtexitIncrement);
	LIB_FUNC("vNe1w4diLCs", LibKernel::tls_get_addr);
	LIB_FUNC("WhCc1w3EhSI", LibKernel::KernelSetThreadAtexitReport);
	LIB_FUNC("WslcK1FQcGI", LibKernel::KernelIsNeoMode);
	LIB_FUNC("wzvqT4UqKX8", LibKernel::KernelLoadStartModule);
	LIB_FUNC("Xjoosiw+XPI", LibKernel::KernelUuidCreate);
	LIB_FUNC("zE-wXIZjLoM", LibKernel::KernelDebugRaiseExceptionOnReleaseMode);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
