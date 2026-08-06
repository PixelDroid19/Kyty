#include "Kyty/UnitTest.h"

namespace Kyty::UnitTest {

UT_LINK(CoreCharString);
UT_LINK(CoreCharString8);
UT_LINK(CoreLanguage);
UT_LINK(CoreSubsystems);
UT_LINK(CoreMSpace);
UT_LINK(CoreDateTime);
UT_LINK(CoreMemoryAlloc);
UT_LINK(CoreVirtualMemory);
#if defined(KYTY_HAVE_DEVTOOLS_TESTS)
UT_LINK(DevToolsEventRing);
UT_LINK(DevToolsProgress);
UT_LINK(DevToolsClassifier);
UT_LINK(DevToolsProtocol);
#if !defined(_WIN32)
UT_LINK(DevToolsSupervisor);
UT_LINK(DevToolsBundle);
UT_LINK(DevToolsLifecycle);
#endif
UT_LINK(DevToolsExportCatalog);
#endif
UT_LINK(EmulatorGraphicsState);
UT_LINK(EmulatorGraphicsDirtyTracking);
UT_LINK(EmulatorKernelMemory);
UT_LINK(EmulatorKernelTime);
UT_LINK(EmulatorGuestMemory);
UT_LINK(EmulatorLibCTime);
UT_LINK(EmulatorGraphicsPackets);
UT_LINK(EmulatorKernelProcess);
UT_LINK(EmulatorNp);
UT_LINK(EmulatorNpTrophy2);
UT_LINK(EmulatorHttp2);
UT_LINK(EmulatorNetwork);
UT_LINK(EmulatorLibcPrintf);
UT_LINK(EmulatorLibcMemalign);
UT_LINK(EmulatorLibcCxaDynamicCast);
UT_LINK(EmulatorLibcCxxLocale);
UT_LINK(EmulatorSaveData);
UT_LINK(EmulatorAudio);
UT_LINK(EmulatorPad);
UT_LINK(EmulatorIme);
UT_LINK(EmulatorLibcString);
UT_LINK(EmulatorLoaderTls);
UT_LINK(EmulatorModuleLoad);
UT_LINK(EmulatorNeutralPorts);
UT_LINK(EmulatorApplicationHeap);
UT_LINK(AgentJson);
UT_LINK(AgentTools);
UT_LINK(EmulatorExactStagingPool);
UT_LINK(EmulatorFiber);
UT_LINK(EmulatorGpuDeferredDeletionQueue);
UT_LINK(EmulatorGpuMemoryFault);
UT_LINK(EmulatorGpuMemoryRangeQueryCache);
UT_LINK(EmulatorGpuSubmissionCoordinator);
UT_LINK(EmulatorGpuSubmissionTracker);
UT_LINK(EmulatorHostImageSurface);
UT_LINK(EmulatorKernelGuestRuntime);
UT_LINK(EmulatorLoaderModuleStart);
UT_LINK(EmulatorLoaderUnwind);
UT_LINK(EmulatorLog);
UT_LINK(EmulatorModuleDiscovery);
UT_LINK(EmulatorShaderTranslationCache);
UT_LINK(EmulatorSymbolDatabase);
UT_LINK(EmulatorSystemContentPort);
UT_LINK(EmulatorVideoOutResolution);
UT_LINK(EmulatorVulkanQueueIdentity);

void UnitTestSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	testing::InitGoogleTest(parent->GetArgc(), parent->GetArgv());
}

void UnitTestSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void UnitTestSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

bool unit_test_all()
{
	return RUN_ALL_TESTS() == 0;
}

} // namespace Kyty::UnitTest
