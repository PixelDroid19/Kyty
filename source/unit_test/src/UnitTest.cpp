#include "Kyty/UnitTest.h"

#include <cstdlib>

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
UT_LINK(EmulatorGraphicsResources);
UT_LINK(EmulatorGeState);
UT_LINK(EmulatorComputeModes);
UT_LINK(EmulatorNativeWaveAdmission);
UT_LINK(EmulatorNativeWaveSnapshot);
UT_LINK(EmulatorVertexProgram);
UT_LINK(EmulatorNggPassthroughProof);
UT_LINK(EmulatorNggFront);
UT_LINK(EmulatorFragmentMaskFlow);
UT_LINK(EmulatorComputeColorFill);
UT_LINK(EmulatorSyncOnAddressLibrary);
UT_LINK(EmulatorImeDialog);
UT_LINK(EmulatorPrimitiveState);
UT_LINK(EmulatorShaderArithmetic);
UT_LINK(EmulatorShaderMaskValues);
UT_LINK(EmulatorShaderLdsBounds);
UT_LINK(EmulatorGraphicsDirtyTracking);
UT_LINK(EmulatorGraphicsGdsRange);
UT_LINK(EmulatorShaderResourceFoldReplay);
UT_LINK(EmulatorShaderControlFlowGraph);
UT_LINK(EmulatorLabelPublication);
UT_LINK(EmulatorGuestDeviceAddress);
UT_LINK(EmulatorKernelMemory);
UT_LINK(EmulatorKernelTime);
UT_LINK(EmulatorGuestMemory);
UT_LINK(EmulatorLibCTime);
UT_LINK(EmulatorGraphicsPackets);
UT_LINK(EmulatorTileDetile);
UT_LINK(EmulatorKernelProcess);
UT_LINK(EmulatorAmprRead);
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
UT_LINK(EmulatorLibcHeap);
UT_LINK(AgentJson);
UT_LINK(AgentTools);
UT_LINK(EmulatorExactStagingPool);
UT_LINK(EmulatorFiber);
UT_LINK(EmulatorFiberOwnership);
UT_LINK(EmulatorVideoOutFlipPending);
UT_LINK(EmulatorVideoOutLifecycle);
UT_LINK(EmulatorControllerSources);
UT_LINK(EmulatorFileSystemPath);
UT_LINK(EmulatorFileDescriptors);
UT_LINK(EmulatorPlayGo);
// Also registers EmulatorFragmentNativeWaveTier from this translation unit.
UT_LINK(EmulatorFragmentNeutralRegion);
UT_LINK(EmulatorFragmentParameterState);
UT_LINK(EmulatorFragmentTransportAdmission);
UT_LINK(EmulatorFragmentTransportLayout);
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
UT_LINK(EmulatorShaderMimg);
UT_LINK(EmulatorShaderExport);
UT_LINK(EmulatorShaderScalarCompare);
UT_LINK(EmulatorShaderScalarBit);
UT_LINK(EmulatorShaderResourcePointers);
UT_LINK(EmulatorShaderResourceBounds);
UT_LINK(EmulatorShaderDescriptorLimits);
UT_LINK(EmulatorShaderDescriptorLayoutPlan);
UT_LINK(EmulatorShaderMetadataResourceIndex);
UT_LINK(EmulatorShaderDynamicMappings);
UT_LINK(EmulatorShaderGetpc);
UT_LINK(EmulatorShaderProgramAddress);
UT_LINK(EmulatorComputeWaveLayout);
UT_LINK(EmulatorComputeWaveIdentity);
UT_LINK(EmulatorComputeWaveDispatchPacket);
UT_LINK(EmulatorComputeWaveRuntime);
UT_LINK(EmulatorComputeWaveResourceAnalysis);
UT_LINK(EmulatorComputeWaveVulkan);
UT_LINK(EmulatorComputeWaveMasks);
UT_LINK(EmulatorComputeWaveAnalysis);
UT_LINK(EmulatorComputeWaveNativeEquivalence);
UT_LINK(EmulatorComputeWaveLds);
UT_LINK(EmulatorComputeWaveAlu);
UT_LINK(EmulatorComputeWaveScalar);
UT_LINK(EmulatorComputeWaveControlFlow);
UT_LINK(EmulatorComputeWaveWaitcnt);
UT_LINK(EmulatorShaderSmemEncoding);
UT_LINK(EmulatorComputeWaveScalarBuffer);
UT_LINK(EmulatorComputeWaveVectorBuffer);
UT_LINK(EmulatorComputeWaveSdwa);
UT_LINK(EmulatorComputeWaveTernaryAlu);
UT_LINK(EmulatorShaderScalarPack);
UT_LINK(EmulatorShaderVectorPack);
UT_LINK(EmulatorShaderMaskAnalysis);
UT_LINK(EmulatorShaderReverseBorrow);
UT_LINK(EmulatorShaderLaneExec);
UT_LINK(EmulatorShaderEmitterPreconditions);
UT_LINK(EmulatorShaderSopkDecode);
UT_LINK(EmulatorDiagnosticDump);
UT_LINK(EmulatorShaderTranslationCache);
UT_LINK(EmulatorSymbolDatabase);
UT_LINK(EmulatorSystemContentPort);
UT_LINK(EmulatorVideoOutResolution);
UT_LINK(EmulatorVulkanQueueIdentity);

void UnitTestSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	// Earlier suites can leave worker threads alive. Re-exec the death-test
	// child rather than inheriting possibly locked runtime state after fork.
	// Explicit environment/command-line selections still take precedence.
	if (std::getenv("GTEST_DEATH_TEST_STYLE") == nullptr)
	{
		::testing::FLAGS_gtest_death_test_style = "threadsafe";
	}
	testing::InitGoogleTest(parent->GetArgc(), parent->GetArgv());
}

void UnitTestSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void UnitTestSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

bool unit_test_all()
{
	return RUN_ALL_TESTS() == 0;
}

} // namespace Kyty::UnitTest
