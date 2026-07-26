#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/KernelModuleInfo.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <mutex>
#include <set>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace Sysmodule {

static std::mutex        g_loaded_modules_mutex;
static std::set<uint16_t> g_loaded_modules;

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags, LibKernel::ModuleInfoForUnwind* info)
{
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id)
{
	std::lock_guard lock(g_loaded_modules_mutex);
	g_loaded_modules.insert(id);
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id)
{
	std::lock_guard lock(g_loaded_modules_mutex);
	g_loaded_modules.erase(id);
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2, int arg3, int* ret)
{
	if (arg1 != 0 || arg2 != 0 || arg3 != 0 || ret == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*ret = 0;
	{
		std::lock_guard lock(g_loaded_modules_mutex);
		g_loaded_modules.insert(id);
	}

	return 0;
}

static KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id)
{
	std::lock_guard lock(g_loaded_modules_mutex);
	return g_loaded_modules.find(id) != g_loaded_modules.end() ? 0 : LibKernel::KERNEL_ERROR_ENOENT;
}

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1)
{
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
