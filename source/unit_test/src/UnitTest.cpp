#include "Kyty/UnitTest.h"

namespace Kyty::UnitTest {

UT_LINK(CoreCharString);
UT_LINK(CoreCharString8);
UT_LINK(CoreMSpace);
UT_LINK(CoreDateTime);
UT_LINK(CoreMemoryAlloc);
UT_LINK(CoreVirtualMemory);
UT_LINK(DevToolsEventRing);
UT_LINK(DevToolsProgress);
UT_LINK(DevToolsClassifier);
UT_LINK(DevToolsProtocol);
UT_LINK(DevToolsSupervisor);
UT_LINK(DevToolsBundle);
UT_LINK(DevToolsLifecycle);
UT_LINK(DevToolsExportCatalog);
UT_LINK(EmulatorGraphicsState);
UT_LINK(EmulatorGraphicsDirtyTracking);
UT_LINK(EmulatorKernelMemory);
UT_LINK(EmulatorGraphicsPackets);
UT_LINK(EmulatorKernelProcess);
UT_LINK(EmulatorNp);
UT_LINK(EmulatorLibcPrintf);
UT_LINK(EmulatorLibcMemalign);
UT_LINK(EmulatorLibcCxaDynamicCast);
UT_LINK(EmulatorLibcCxxLocale);
UT_LINK(EmulatorSaveData);
UT_LINK(EmulatorAudio);
UT_LINK(EmulatorPad);
UT_LINK(EmulatorLoaderTls);
UT_LINK(EmulatorModuleLoad);
UT_LINK(EmulatorApplicationHeap);
UT_LINK(AgentTools);

KYTY_SUBSYSTEM_INIT(UnitTest)
{
	testing::InitGoogleTest(KYTY_SUBSYSTEM_ARGC, KYTY_SUBSYSTEM_ARGV);
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(UnitTest) {}

KYTY_SUBSYSTEM_DESTROY(UnitTest) {}

bool unit_test_all()
{
	return RUN_ALL_TESTS() == 0;
}

} // namespace Kyty::UnitTest
