#include "Kyty/UnitTest.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <cstdlib>

namespace Kyty::Libs::PlayGo {
struct PlayGoInitParams;
int KYTY_SYSV_ABI PlayGoInitialize(const PlayGoInitParams* init);
int KYTY_SYSV_ABI PlayGoOpen(int* out_handle, const void* param);
int KYTY_SYSV_ABI PlayGoClose(int handle);
int KYTY_SYSV_ABI PlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int8_t* out_loci);
} // namespace Kyty::Libs::PlayGo

UT_BEGIN(EmulatorPlayGo);

TEST(EmulatorPlayGo, InvalidArgumentsReturnBeforeAccessingOutputs)
{
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    using namespace Libs;
		    using namespace Libs::PlayGo;
		    int handle = 42;
		    uint16_t chunk = 0;
		    int8_t locus = -7;
		    const bool rejected =
		        PlayGoInitialize(nullptr) == PLAYGO_ERROR_BAD_POINTER &&
		        PlayGoOpen(nullptr, nullptr) == PLAYGO_ERROR_BAD_POINTER &&
		        PlayGoOpen(&handle, &chunk) == PLAYGO_ERROR_INVALID_ARGUMENT && handle == 42 &&
		        PlayGoGetLocus(0, nullptr, 1, nullptr) == PLAYGO_ERROR_BAD_HANDLE &&
		        PlayGoGetLocus(1, nullptr, 1, &locus) == PLAYGO_ERROR_BAD_POINTER &&
		        PlayGoGetLocus(1, &chunk, 1, nullptr) == PLAYGO_ERROR_BAD_POINTER &&
		        PlayGoGetLocus(1, &chunk, 0, &locus) == PLAYGO_ERROR_BAD_SIZE && locus == -7 &&
		        PlayGoClose(0) == PLAYGO_ERROR_BAD_HANDLE;
		    std::_Exit(rejected ? 0 : 1);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
