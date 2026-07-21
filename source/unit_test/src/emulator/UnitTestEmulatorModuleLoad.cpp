#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include "Kyty/Core/File.h"
#include "Kyty/UnitTest.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

class RuntimeLinkerIntegrationAccess
{
public:
	static Program* AttachSyntheticExportModule(RuntimeLinker* rt, const Core::String& file_name)
	{
		return rt->AttachSyntheticExportModule(file_name);
	}
};

} // namespace Kyty::Loader

UT_BEGIN(EmulatorModuleLoad);

namespace {

using Kyty::Core::String;
using namespace Kyty::Loader;

template <typename T>
void AppendPod(std::vector<uint8_t>* out, const T& value)
{
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	out->insert(out->end(), bytes, bytes + sizeof(T));
}

std::vector<uint8_t> MakeSelfWrappedElf(Elf64_Half type, bool alternate_signature = false, bool unstored_sections = false)
{
	SelfHeader self {};
	const uint8_t ident[] = {alternate_signature ? uint8_t {0x54} : uint8_t {0x4f},
	                         alternate_signature ? uint8_t {0x14} : uint8_t {0x15},
	                         alternate_signature ? uint8_t {0xf5} : uint8_t {0x3d},
	                         alternate_signature ? uint8_t {0xee} : uint8_t {0x1d},
	                         0x00, 0x01, 0x01, 0x12, 0x01, 0x01, 0x00, 0x00};
	std::memcpy(self.ident, ident, sizeof(ident));
	self.file_size    = sizeof(SelfHeader);
	self.segments_num = 0;
	self.unknown      = 0x22;

	Elf64_Ehdr ehdr {};
	ehdr.e_ident[EI_MAG0]       = 0x7f;
	ehdr.e_ident[EI_MAG1]       = 'E';
	ehdr.e_ident[EI_MAG2]       = 'L';
	ehdr.e_ident[EI_MAG3]       = 'F';
	ehdr.e_ident[EI_CLASS]      = ELFCLASS64;
	ehdr.e_ident[EI_DATA]       = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION]    = EV_CURRENT;
	ehdr.e_ident[EI_OSABI]      = ELFOSABI_FREEBSD;
	ehdr.e_ident[EI_ABIVERSION] = 2;
	ehdr.e_type                 = type;
	ehdr.e_machine              = EM_X86_64;
	ehdr.e_version              = EV_CURRENT;
	ehdr.e_ehsize               = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize            = sizeof(Elf64_Phdr);
	if (unstored_sections)
	{
		ehdr.e_shoff     = 0x10000;
		ehdr.e_shentsize = sizeof(Elf64_Shdr);
		ehdr.e_shnum     = 2;
		ehdr.e_shstrndx  = 1;
	}

	std::vector<uint8_t> out;
	AppendPod(&out, self);
	AppendPod(&out, ehdr);
	return out;
}

bool WriteBinary(const String& path, const std::vector<uint8_t>& data)
{
	Kyty::Core::File f;
	if (!f.Create(path))
	{
		return false;
	}
	uint32_t bytes_written = 0;
	f.Write(data.data(), static_cast<uint32_t>(data.size()), &bytes_written);
	f.Close();
	return bytes_written == data.size();
}

struct TempPackageRoot
{
	explicit TempPackageRoot(const String& path): root(path) { Kyty::Core::File::DeleteDirectories(root); }
	~TempPackageRoot() { Kyty::Core::File::DeleteDirectories(root); }

	KYTY_CLASS_NO_COPY(TempPackageRoot);

	String root;
};

SymbolResolve LibcFunc(const char16_t* nid)
{
	SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = U"libc";
	sr.library_version      = 1;
	sr.module               = U"libc";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = SymbolType::Func;
	return sr;
}

SymbolResolve LibkernelFunc(const char16_t* nid)
{
	SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = U"libkernel";
	sr.library_version      = 1;
	sr.module               = U"libkernel";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = SymbolType::Func;
	return sr;
}

void AddLibcImportIds(Program* program)
{
	ASSERT_NE(program, nullptr);
	ASSERT_NE(program->dynamic_info, nullptr);
	program->dynamic_info->import_libs.Add(LibraryId {U"libc-lib-id", 1, U"libc"});
	program->dynamic_info->import_modules.Add(ModuleId {U"libc-mod-id", 1, 1, U"libc"});
}

void AddLibcExportIds(Program* program)
{
	ASSERT_NE(program, nullptr);
	ASSERT_NE(program->dynamic_info, nullptr);
	program->dynamic_info->export_libs.Add(LibraryId {U"libc-lib-id", 1, U"libc"});
	program->dynamic_info->export_modules.Add(ModuleId {U"libc-mod-id", 1, 1, U"libc"});
}

void EnsureFileSystemSubsystem()
{
	static bool initialized = false;
	if (!initialized)
	{
		if (!Kyty::Config::IsInitialized())
		{
			Kyty::Config::ConfigSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
		}
		Kyty::Log::LogSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
		Kyty::Libs::LibKernel::FileSystem::FileSystemSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
		initialized = true;
	}
}

} // namespace

TEST(EmulatorModuleLoad, BuildPlanAcceptsSelfWrappedAdjacentSharedPrx)
{
	const TempPackageRoot temp(U"/tmp/kyty_module_load_self_prx_test/");
	ASSERT_TRUE(Kyty::Core::File::CreateDirectories(temp.root + U"sce_module/"));
	ASSERT_TRUE(WriteBinary(temp.root + U"eboot.bin", MakeSelfWrappedElf(ET_DYNEXEC)));
	ASSERT_TRUE(WriteBinary(temp.root + U"sce_module/libc.prx", MakeSelfWrappedElf(ET_DYNAMIC)));

	const auto plan = ModuleLoadPlanning::BuildPlan(temp.root + U"eboot.bin", true);

	ASSERT_TRUE(plan.valid) << plan.error;
	EXPECT_EQ(plan.diag.adjacent_count, 1u);
	ASSERT_EQ(plan.count, 2u);
	EXPECT_STREQ(plan.entries[1].relative_key, "sce_module/libc.prx");
	EXPECT_EQ(plan.entries[1].role, ModulePlanRole::AdjacentShared);
	EXPECT_EQ(plan.diag.rejection_count, 0u);
}

TEST(EmulatorModuleLoad, RecognizesAlternateGen5SelfSignature)
{
	EnsureFileSystemSubsystem();
	const TempPackageRoot temp(U"/tmp/kyty_module_load_alternate_self_test/");
	ASSERT_TRUE(Kyty::Core::File::CreateDirectories(temp.root));
	const String path = temp.root + U"eboot.bin";
	ASSERT_TRUE(WriteBinary(path, MakeSelfWrappedElf(ET_DYNEXEC, true, true)));

	Elf64 elf;
	elf.Open(path);
	EXPECT_TRUE(elf.IsSelf());
	EXPECT_TRUE(elf.IsValid());
}

TEST(EmulatorModuleLoad, BuildPlanRejectsExtensionOnlyAdjacentJunk)
{
	const TempPackageRoot temp(U"/tmp/kyty_module_load_junk_prx_test/");
	ASSERT_TRUE(Kyty::Core::File::CreateDirectories(temp.root + U"sce_module/"));
	ASSERT_TRUE(WriteBinary(temp.root + U"eboot.bin", MakeSelfWrappedElf(ET_DYNEXEC)));
	ASSERT_TRUE(WriteBinary(temp.root + U"sce_module/libc.prx", std::vector<uint8_t> {'j', 'u', 'n', 'k'}));

	const auto plan = ModuleLoadPlanning::BuildPlan(temp.root + U"eboot.bin", true);

	ASSERT_TRUE(plan.valid) << plan.error;
	EXPECT_EQ(plan.diag.adjacent_count, 0u);
	EXPECT_EQ(plan.count, 1u);
	ASSERT_EQ(plan.diag.rejection_count, 1u);
	EXPECT_STREQ(plan.diag.rejections[0], "reject not_shared_elf sce_module/libc.prx");
}

TEST(EmulatorModuleLoad, AfterPrimaryLoadedStagesAdjacentPlanWithoutBringupFlag)
{
	RuntimeLinker          rt;
	const TempPackageRoot temp(U"/tmp/kyty_module_load_default_stage_test/");
	ASSERT_TRUE(Kyty::Core::File::CreateDirectories(temp.root + U"sce_module/"));
	ASSERT_TRUE(WriteBinary(temp.root + U"eboot.bin", MakeSelfWrappedElf(ET_DYNEXEC)));
	ASSERT_TRUE(WriteBinary(temp.root + U"sce_module/libc.prx", MakeSelfWrappedElf(ET_DYNAMIC)));

	ModuleLifecycleCoordinator::AfterPrimaryLoaded(&rt, temp.root + U"eboot.bin");

	const auto diag = ModuleLifecycleCoordinator::GetDiagnostics();
	EXPECT_TRUE(diag.discovery_enabled);
	EXPECT_EQ(diag.adjacent_count, 1u);
	EXPECT_TRUE(ModuleLifecycleCoordinator::HasPendingAdjacentPlan(&rt));
}

TEST(EmulatorModuleLoad, ResolvePrefersLoadedModuleExportOverHleForSameIdentity)
{
	RuntimeLinker rt;
	const auto    sr = LibcFunc(u"same-nid");
	rt.Symbols()->Add(sr, 0x11110000, U"hle_libc_stub");

	Program* importer = RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&rt, U"/tmp/eboot.bin");
	AddLibcImportIds(importer);

	Program* libc = RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&rt, U"/tmp/sce_module/libc.prx");
	AddLibcExportIds(libc);
	libc->export_symbols->Add(sr, 0x22220000, U"libc_prx_export");

	SymbolRecord out {};
	bool         bind_self = true;
	rt.Resolve(U"same-nid#libc-lib-id#libc-mod-id", SymbolType::Func, importer, &out, &bind_self);

	EXPECT_EQ(out.vaddr, 0x22220000u);
	EXPECT_FALSE(bind_self);
}

TEST(EmulatorModuleLoad, ResolvePrefersHleMemalignOverUninitializedGuestLibcHeap)
{
	RuntimeLinker rt;
	const auto    sr = LibcFunc(u"Ujf3KzMvRmI");
	rt.Symbols()->Add(sr, 0x11110000, U"hle_memalign");

	Program* importer = RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&rt, U"/tmp/eboot.bin");
	AddLibcImportIds(importer);

	Program* libc = RuntimeLinkerIntegrationAccess::AttachSyntheticExportModule(&rt, U"/tmp/sce_module/libc.prx");
	AddLibcExportIds(libc);
	libc->export_symbols->Add(sr, 0x22220000, U"guest_memalign");

	SymbolRecord out {};
	bool         bind_self = true;
	rt.Resolve(U"Ujf3KzMvRmI#libc-lib-id#libc-mod-id", SymbolType::Func, importer, &out, &bind_self);

	EXPECT_EQ(out.vaddr, 0x11110000u);
	EXPECT_FALSE(bind_self);
}

TEST(EmulatorModuleLoad, LibkernelRegistersPosixLseekAlias)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	ASSERT_NE(symbols.Find(LibkernelFunc(u"oib76F-12fk")), nullptr);
	EXPECT_NE(symbols.Find(LibkernelFunc(u"Oy6IpwgtYOk")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersModuleInfoForUnwind)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"RpQJJVKTiFM")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersKernelSleep)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"-ZR+hG7aDHw";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelUnityRegistersExceptionHandler)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"WkwEd3N7w0Y";
	query.library              = U"libkernel_unity";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
	query.name = U"il03nluKfMk";
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorModuleLoad, PosixRegistersConditionTimedWait)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"27bAgiJmOh0";
	query.library              = U"Posix";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
	query.name = U"AqBioC2vF3I";
	EXPECT_NE(symbols.Find(query), nullptr);
	query.name = U"mqQMh1zPPT8";
	EXPECT_NE(symbols.Find(query), nullptr);
	query.name = U"VAzswvTOCzI";
	EXPECT_NE(symbols.Find(query), nullptr);
	for (const auto* nid:
	     {U"wuCroIGjt2g", U"bY-PO6JhzhQ", U"FN4gaPmuFV8", U"ezv-RSBNKqI", U"C2kJ-byS5rM", U"4n51s0zEf0c", U"XVL8So3QJUk",
	      U"6O8EwYOgH9Y", U"RenI1lL1WFk", U"3e+4Iv7IJ8U", U"lUk6wrGXyMw", U"aNeavPDNKzA", U"hI7oVeOluPM",
	      U"1471ajPzxh0", U"sIlRvQqsN2Y", U"KuOmgKoqCdY", U"pxnCmagrtao", U"6XG4B33N09g", U"7Xl257M4VNI"})
	{
		query.name = nid;
		EXPECT_NE(symbols.Find(query), nullptr);
	}
}

TEST(EmulatorModuleLoad, EncodesAndFindsDynamicSymbolsByNid)
{
	EXPECT_EQ(EncodeNameAsNid("sceKernelDlsym"), U"LwG8g3niqwA");
	EXPECT_EQ(EncodeNameAsNid("sceKernelGetModuleInfoForUnwind"), U"RpQJJVKTiFM");

	SymbolDatabase symbols;
	SymbolResolve  exported {};
	exported.name                 = U"LwG8g3niqwA";
	exported.library              = U"libkernel";
	exported.library_version      = 1;
	exported.module               = U"libkernel";
	exported.module_version_major = 1;
	exported.module_version_minor = 1;
	exported.type                 = SymbolType::Func;
	symbols.Add(exported, 0x12345678u);

	const auto* record = symbols.FindByNid(U"LwG8g3niqwA", SymbolType::Func);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->vaddr, 0x12345678u);
	EXPECT_EQ(symbols.FindByNid(U"LwG8g3niqwA", SymbolType::Object), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersDlsym)
{
	EnsureFileSystemSubsystem();
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"LwG8g3niqwA";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* record = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using DlsymFn = int (*)(int32_t, const char*, void**);
	auto* const preserved = reinterpret_cast<void*>(0x12345678u);
	void*      address    = preserved;
	const int  result     = reinterpret_cast<DlsymFn>(record->vaddr)(-1, "missing_symbol", &address);

	EXPECT_EQ(result, Kyty::Libs::LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(address, preserved);
}

TEST(EmulatorModuleLoad, NetRegistersEpollCreate)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"SF47kB2MNTo";
	query.library              = U"Net";
	query.library_version      = 1;
	query.module               = U"Net";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	for (const auto* nid: {U"SF47kB2MNTo", U"Inp1lfL+Jdw", U"ZVw46bsasAk", U"drjIbDbA7UQ", U"C4UgDHHPvdw",
	                       U"Nd91WaWmG2w", U"kJlYH5uMAWI", U"K7RlrTkI-mw", U"Apb4YDxKsRI", U"hLuXdjHnhiI",
	                       U"2mKX2Spso7I", U"xphrZusl78E"})
	{
		query.name = nid;
		EXPECT_NE(symbols.Find(query), nullptr);
	}
}

TEST(EmulatorModuleLoad, RtcRegistersCurrentTick)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSceRtc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"18B2NS1y9UU";
	query.library              = U"Rtc";
	query.library_version      = 1;
	query.module               = U"Rtc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersPosixRwlockReadLock)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"iGjsr1WAtI0")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersPosixRwlockUnlock)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"EgmLo6EWgso")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersPosixRwlockInit)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"ytQULN-nhL4")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersPthreadRename)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"GBUY7ywdULE")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelRegistersEventFlagClear)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	EXPECT_NE(symbols.Find(LibkernelFunc(u"7uhBFWRAS60")), nullptr);
}

TEST(EmulatorModuleLoad, LibkernelReadAliasReadsRegularFileDescriptor)
{
	using namespace Kyty::Libs::LibKernel;
	using ReadFn = int64_t(KYTY_SYSV_ABI*)(int, void*, uint64_t);

	EnsureFileSystemSubsystem();

	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	const SymbolRecord* read = symbols.Find(LibkernelFunc(u"DRuBt2pvICk"));
	ASSERT_NE(read, nullptr);
	auto* read_fn = reinterpret_cast<ReadFn>(read->vaddr);
	ASSERT_NE(read_fn, nullptr);

	const TempPackageRoot temp(U"/tmp/kyty_module_load_read_alias_test/");
	ASSERT_TRUE(Kyty::Core::File::CreateDirectories(temp.root));
	ASSERT_TRUE(WriteBinary(temp.root + U"payload.bin", std::vector<uint8_t> {'a', 'b', 'c', 'd'}));
	FileSystem::Mount(temp.root, U"/app0/");

	const int fd = FileSystem::KernelOpen("/app0/payload.bin", 0, 0);
	ASSERT_GE(fd, 3);

	char buf[4] = {};
	EXPECT_EQ(read_fn(fd, buf, 3), 3);
	EXPECT_EQ(std::memcmp(buf, "abc", 3), 0);

	EXPECT_EQ(FileSystem::KernelClose(fd), ::OK);
	FileSystem::Umount(U"/app0/");
}

UT_END();

#endif // KYTY_EMU_ENABLED
