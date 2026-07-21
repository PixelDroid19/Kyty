#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/MissingImport.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Core::VirtualMemory {
class ExceptionHandler;
} // namespace Kyty::Core::VirtualMemory

namespace Kyty::Loader {

class Elf64;
struct Elf64_Sym;
struct Elf64_Rela;
class RuntimeLinker;
class RuntimeLinkerIntegrationAccess;

using module_func_t = int (*)(size_t args, const void* argp);

struct ModuleId
{
	bool operator==(const ModuleId& other) const
	{
		return version_major == other.version_major && version_minor == other.version_minor && name == other.name;
	}

	String id;
	int    version_major;
	int    version_minor;
	String name;
};

struct LibraryId
{
	bool operator==(const LibraryId& other) const { return version == other.version && name == other.name; }

	String id;
	int    version;
	String name;
};

struct ThreadLocalStorage
{
	uint64_t image_vaddr   = 0;
	uint64_t init_size     = 0;
	uint64_t image_size    = 0;
	uint64_t handler_vaddr = 0;
	uint64_t module_id     = 0;
	uint64_t static_offset = 0;

	Core::Hashmap<int, uint8_t*> tlss;
	Core::Mutex                  mutex;
};

struct DynamicInfo
{
	void*    hash_table      = nullptr;
	uint64_t hash_table_size = 0;

	char*    str_table      = nullptr;
	uint64_t str_table_size = 0;

	Elf64_Sym* symbol_table            = nullptr;
	uint64_t   symbol_table_total_size = 0;
	uint64_t   symbol_table_entry_size = 0;

	uint64_t init_vaddr          = 0;
	uint64_t fini_vaddr          = 0;
	uint64_t init_array_vaddr    = 0;
	uint64_t fini_array_vaddr    = 0;
	uint64_t preinit_array_vaddr = 0;
	uint64_t init_array_size     = 0;
	uint64_t fini_array_size     = 0;
	uint64_t preinit_array_size  = 0;
	uint64_t pltgot_vaddr        = 0;

	Elf64_Rela* jmprela_table      = nullptr;
	uint64_t    jmprela_table_size = 0;

	Elf64_Rela* rela_table            = nullptr;
	uint64_t    rela_table_total_size = 0;
	uint64_t    rela_table_entry_size = 0;

	uint64_t relative_count = 0;

	uint64_t debug   = 0;
	uint64_t textrel = 0;
	uint64_t flags   = 0;

	const char* so_name = nullptr;

	Vector<const char*> needed;
	Vector<ModuleId>    export_modules;
	Vector<ModuleId>    import_modules;
	Vector<LibraryId>   export_libs;
	Vector<LibraryId>   import_libs;
};

struct Program
{
	int32_t                                unique_id = -1;
	RuntimeLinker*                         rt        = nullptr;
	String                                 file_name;
	Elf64*                                 elf               = nullptr;
	Core::VirtualMemory::ExceptionHandler* exception_handler = nullptr;
	DynamicInfo*                           dynamic_info      = nullptr;
	uint64_t                               base_vaddr        = 0;
	uint64_t                               base_size         = 0;
	uint64_t                               base_size_aligned = 0;
	SymbolDatabase*                        export_symbols    = nullptr;
	SymbolDatabase*                        import_symbols    = nullptr;
	ThreadLocalStorage                     tls;
	bool                                   fail_if_global_not_resolved = true;
	bool                                   dbg_print_reloc             = false;
	uint64_t                               proc_param_vaddr            = 0;
	uint64_t                               custom_call_plt_vaddr       = 0;
	uint32_t                               custom_call_plt_num         = 0;
};

// Immutable data copied while RuntimeLinker owns m_mutex. Consumers can inspect
// this after the lock is released without retaining a Program pointer.
struct ProgramExportSnapshot
{
	int32_t        unique_id = -1;
	String         file_name;
	Vector<String> export_names;
};

struct LoadedModuleSnapshot
{
	int32_t  unique_id   = -1;
	String   file_name;
	uint64_t base_vaddr  = 0;
	uint64_t base_size   = 0;
	uint64_t entry_point = 0;
};

struct EhFrameInfo
{
	uint64_t header_addr = 0;
	uint64_t frame_addr  = 0;
	uint64_t frame_size  = 0;
};

struct ProgramUnwindInfo
{
	String   file_name;
	uint64_t eh_frame_header_addr = 0;
	uint64_t eh_frame_addr        = 0;
	uint64_t eh_frame_size        = 0;
	uint64_t image_addr           = 0;
	uint64_t image_size           = 0;
};

struct ModuleStartDescriptor
{
	String         file_name;
	String         so_name;
	Vector<String> needed;
};

// Return stable dependency-first indices. Dependencies not present in the
// loaded set are supplied by HLE or loaded later through the module API.
// libc.prx is visited first when present so CRT heap/TSD bootstrap runs before
// C++ module constructors that call operator new during startup.
Vector<uint32_t> LoaderBuildModuleStartOrder(const Vector<ModuleStartDescriptor>& modules);

// Decode the captured GNU EH-frame header form (version 1, pcrel+sdata4).
// readable_end is the exclusive bound of the mapped readable range.
bool LoaderDecodeEhFrameHeader(const uint8_t* header, size_t header_size, uint64_t header_addr, uint64_t readable_end,
                               EhFrameInfo* out_info);

// MissingImportDiagnostics is defined in MissingImport.h (owned by
// ImportDiagnostics / MissingImportRegistry).

class RuntimeLinker
{
public:
	RuntimeLinker();
	virtual ~RuntimeLinker();
	void Clear();

	KYTY_CLASS_NO_COPY(RuntimeLinker);

	void DbgDump(const String& folder);
	void DbgDumpSymbols(const String& folder);

	Program* LoadProgram(const String& elf_name);
	// True if a program with this exact host path is already in the load list.
	[[nodiscard]] bool HasProgramFile(const String& elf_name);
	void     SaveMainProgram(const String& elf_name);
	void     SaveProgram(Program* program, const String& elf_name);
	void     UnloadProgram(Program* program);

	[[nodiscard]] uint64_t GetEntry();
	[[nodiscard]] uint64_t GetProcParam();

	void RelocateAll();
	// Relocate a newly loaded module without replaying relocation records of
	// initialized modules whose GOT entries may be runtime-managed.
	void RelocateProgram(Program* program);

	void Execute();
	int  StartModule(Program* program, size_t args, const void* argp, module_func_t func);
	int  StopModule(Program* program, size_t args, const void* argp, module_func_t func);
	void StartAllModules();
	void StopAllModules();
	void DeleteTlss(int thread_id);

	void Resolve(const String& name, SymbolType type, Program* program, SymbolRecord* out_info, bool* bind_self = nullptr);
	[[nodiscard]] MissingImportDiagnostics GetMissingImportDiagnostics() const;
	[[nodiscard]] static MissingImportDiagnostics GetGlobalMissingImportDiagnostics();

	SymbolDatabase* Symbols() { return m_symbols; }

	// Immutable view for export-conflict scans. Do not expose Program pointers
	// outside the lock: another lifecycle operation may unload them immediately.
	[[nodiscard]] uint32_t LoadedProgramCount();
	[[nodiscard]] Vector<ProgramExportSnapshot> SnapshotExportPrograms();
	[[nodiscard]] Vector<LoadedModuleSnapshot>  SnapshotLoadedModules();
	[[nodiscard]] bool TryGetProgramUnwindInfoByAddr(uint64_t vaddr, ProgramUnwindInfo* out_info);

	static uint64_t ReadFromElf(Program* program, uint64_t vaddr);
	Program*        FindProgramByAddr(uint64_t vaddr);
	Program*        FindProgramById(int32_t id);

	static uint8_t* TlsGetAddr(Program* program);
	static uint8_t* TlsGetAddr(uint64_t module_id, uint64_t offset);
	static void     DeleteTls(Program* program, int thread_id);

	void StackTrace(uint64_t frame_ptr);

private:
	friend class RuntimeLinkerIntegrationAccess;

	// Integration-only seam. Kept private so production callers cannot attach a
	// Program that did not pass through ELF loading and validation.
	Program* AttachSyntheticExportModule(const Core::String& file_name);

	static void LoadProgramToMemory(Program* program);
	static void ParseProgramDynamicInfo(Program* program);
	static void CreateSymbolDatabase(Program* program);
	static void Relocate(Program* program);
	static void DeleteProgram(Program* program);
	static void SetupTlsHandler(Program* program);

	Program* FindProgram(const ModuleId& m, const LibraryId& l);

	static const ModuleId*  FindModule(const Program& program, const String& id);
	static const LibraryId* FindLibrary(const Program& program, const String& id);

	Vector<Program*> m_programs;
	SymbolDatabase*  m_symbols   = nullptr;
	bool             m_relocated = false;
	Core::Mutex      m_mutex;
};

// Rewrite PS5 TLS-call sites that encode three 0x66 prefixes before E8
// (66 66 66 E8 rel32) into the SysV form 66 66 48 E8 (REX.W). Observed on a
// post-Play Gen5 eboot: every such site targets the TLS handler; three 0x66
// prefixes execute as a 16-bit CALL on the host (IP 0x3ffe, misaligned stack).
// Returns the number of sites rewritten. Pure buffer transform for unit tests.
uint64_t LoaderRewriteTlsGdCallRexPrefix(uint8_t* code, uint64_t size);

// Patch direct guest TLS-base loads in executable code:
//   [66 ...] mov rax, qword ptr fs:[0]
// into a call to Kyty's per-thread guest TLS handler. Returns the number of
// load sites rewritten. Pure buffer transform for unit tests; callers own page
// permissions and instruction-cache flushing.
uint64_t LoaderPatchTlsFsBaseLoads(uint8_t* code, uint64_t size, uint64_t handler_vaddr);

// Calculate x86-64 TLS relocation payloads in guest terms. DTPMOD64 writes the
// stable guest TLS module id; DTPOFF64 writes the module-local TLS offset; and
// TPOFF64 writes the negative static offset from the thread pointer.
uint64_t LoaderTlsRelocationValue(uint32_t relocation_type, uint64_t module_id, uint64_t symbol_offset, int64_t addend,
                                  uint64_t static_tls_size);

// Initialize a per-thread PT_TLS image. Only the file-backed prefix is copied;
// the remaining memory image is TLS BSS and must start at zero.
void LoaderInitializeThreadTlsImage(uint8_t* tls, uint64_t image_size, const uint8_t* template_data, uint64_t init_size);

// Prepare a per-thread TLS image after memcpy from the PT_TLS template:
// 1) Relocate absolute pointers that fall inside [template_vaddr, +image_size)
//    into this thread's copy (self-pointers baked as absolute template addrs).
// 2) Clear absolute pointers into [program_base, +program_size) whose pointee
//    looks like an unconstructed Context (word0==0 and buffer control at
//    +0x3e0==0). Gen5 eboot TLS slots copy a non-null absolute pointer to such
//    storage; the guest null→factory path is the real constructor, and leaving
//    the stale pointer skips it (FATAL-ACCESS-VIOLATION on null buffer+8).
// `guest_read64(addr, out)` returns true when `addr` is a safe 8-byte guest
// read; used for step 2. Pure helper for unit tests when guest_read64 is a stub.
// Returns the number of 8-byte cells modified.
uint64_t LoaderPrepareThreadTlsImage(uint8_t* tls, uint64_t image_size, uint64_t template_vaddr, uint64_t program_base,
                                     uint64_t program_size, bool (*guest_read64)(uint64_t addr, uint64_t* out, void* ctx), void* guest_ctx);

// Run DT_INIT / DT_PREINIT_ARRAY / DT_INIT_ARRAY for a loaded program. Each
// entry is a void(void) guest function at base_vaddr + offset. Testable helper
// for shared-module StartModule paths. Do not call this for main ET_EXEC images
// whose CRT entry already invokes DT_INIT — double init re-links static tables.
void LoaderRunProgramInitializers(uint64_t base_vaddr, const DynamicInfo& info);

// True if `code` (image-resident at code_vaddr) contains a direct x86-64 near
// CALL (E8 rel32) whose absolute target equals target_vaddr. Used to recognize
// CRT _start that invokes DT_INIT (_init) itself.
bool LoaderCodeContainsDirectCallTo(const uint8_t* code, uint64_t size, uint64_t code_vaddr, uint64_t target_vaddr);

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_ */
