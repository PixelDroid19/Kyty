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

std::vector<uint8_t> MakeSelfWrappedElf(Elf64_Half type)
{
	SelfHeader self {};
	const uint8_t ident[] = {0x4f, 0x15, 0x3d, 0x1d, 0x00, 0x01, 0x01, 0x12, 0x01, 0x01, 0x00, 0x00};
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

TEST(EmulatorModuleLoad, LibkernelRegistersPosixLseekAlias)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libkernel_1", &symbols));
	ASSERT_NE(symbols.Find(LibkernelFunc(u"oib76F-12fk")), nullptr);
	EXPECT_NE(symbols.Find(LibkernelFunc(u"Oy6IpwgtYOk")), nullptr);
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
