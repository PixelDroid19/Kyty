#include "Emulator/Common.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Hle/Registration.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle::GraphicsRegistration {

namespace Graphics = ::Kyty::Libs::Graphics;

namespace LibGen4 {

LIB_VERSION("GnmDriver", 1, "GnmDriver", 1, 1);

namespace Gen4 = Graphics::Gen4;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("gAhCn6UiU4Y", Gen4::GraphicsSetVsShader);
	LIB_FUNC("V31V01UiScY", Gen4::GraphicsUpdateVsShader);
	LIB_FUNC("bQVd5YzCal0", Gen4::GraphicsSetPsShader);
	LIB_FUNC("5uFKckiJYRM", Gen4::GraphicsSetPsShader350);
	LIB_FUNC("4MgRw-bVNQU", Gen4::GraphicsUpdatePsShader);
	LIB_FUNC("mLVL7N7BVBg", Gen4::GraphicsUpdatePsShader350);
	LIB_FUNC("Kx-h-nWQJ8A", Gen4::GraphicsSetCsShaderWithModifier);
	LIB_FUNC("HlTPoZ-oY7Y", Gen4::GraphicsDrawIndex);
	LIB_FUNC("GGsn7jMTxw4", Gen4::GraphicsDrawIndexAuto);
	LIB_FUNC("oYM+YzfCm2Y", Gen4::GraphicsDrawIndexOffset);
	LIB_FUNC("zwY0YV91TTI", Gen4::GraphicsSubmitCommandBuffers);
	LIB_FUNC("xbxNatawohc", Gen4::GraphicsSubmitAndFlipCommandBuffers);
	LIB_FUNC("yvZ73uQUqrk", Gen4::GraphicsSubmitDone);
	LIB_FUNC("b08AgtPlHPg", Gen4::GraphicsAreSubmitsAllowed);
	LIB_FUNC("iBt3Oe00Kvc", Gen4::GraphicsFlushMemory);
	LIB_FUNC("b0xyllnVY-I", Gen4::GraphicsAddEqEvent);
	LIB_FUNC("PVT+fuoS9gU", Gen4::GraphicsDeleteEqEvent);
	LIB_FUNC("Idffwf3yh8s", Gen4::GraphicsDrawInitDefaultHardwareState);
	LIB_FUNC("QhnyReteJ1M", Gen4::GraphicsDrawInitDefaultHardwareState175);
	LIB_FUNC("0H2vBYbTLHI", Gen4::GraphicsDrawInitDefaultHardwareState200);
	LIB_FUNC("yb2cRhagD1I", Gen4::GraphicsDrawInitDefaultHardwareState350);
	LIB_FUNC("nF6bFRUBRAU", Gen4::GraphicsDispatchInitDefaultHardwareState);
	LIB_FUNC("1qXLHIpROPE", Gen4::GraphicsInsertWaitFlipDone);
	LIB_FUNC("0BzLGljcwBo", Gen4::GraphicsDispatchDirect);
	LIB_FUNC("29oKvKXzEZo", Gen4::GraphicsMapComputeQueue);
	LIB_FUNC("ArSg-TGinhk", Gen4::GraphicsUnmapComputeQueue);
	LIB_FUNC("ffrNQOshows", Gen4::GraphicsComputeWaitOnAddress);
	LIB_FUNC("bX5IbRvECXk", Gen4::GraphicsDingDong);
	LIB_FUNC("W1Etj-jlW7Y", Gen4::GraphicsInsertPushMarker);
	LIB_FUNC("7qZVNgEu+SY", Gen4::GraphicsInsertPopMarker);
	LIB_FUNC("+AFvOEXrKJk", Gen4::GraphicsSetEmbeddedVsShader);
	LIB_FUNC("ZFqKFl23aMc", Gen4::GraphicsRegisterOwner);
	LIB_FUNC("nvEwfYAImTs", Gen4::GraphicsRegisterResource);
	LIB_FUNC("Fwvh++m9IQI", Gen4::GraphicsGetGpuCoreClockFrequency);
	LIB_FUNC("jg33rEKLfVs", Gen4::GraphicsIsUserPaEnabled);
	LIB_FUNC("ln33zjBrfjk", Gen4::GraphicsGetTheTessellationFactorRingBufferBaseAddress);
}

} // namespace LibGen4

namespace LibAgc {

LIB_VERSION("Agc", 1, "Agc", 1, 1);

namespace Gen5 = Graphics::Gen5;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("23LRUSvYu1M", Gen5::GraphicsInit);
	LIB_FUNC("dbOlWdppb4o", Gen5::GraphicsBuildDescriptorTableOrInterpolantMapping);
	LIB_FUNC("k0E7vkgqAuE", Gen5::GraphicsAgcBuildDescriptorState);
	LIB_FUNC("2JtWUUiYBXs", Gen5::GraphicsGetRegisterDefaults2);
	LIB_FUNC("wRbq6ZjNop4", Gen5::GraphicsGetRegisterDefaults2Internal);
	// Patch helpers (3.20 export names): wait-mem / DMA destination rewrite.
	LIB_FUNC("3KDcnM3lrcU", Gen5::GraphicsAgcWaitRegMemPatchAddress);
	LIB_FUNC("n485EBnIWmk", Gen5::GraphicsAgcWaitRegMemPatchCompareFunction);
	LIB_FUNC("7nOoijNPvEU", Gen5::GraphicsAgcWaitRegMemPatchReference);
	LIB_FUNC("hXAnLgDHCoI", Gen5::GraphicsAgcWaitRegMemPatchMask);
	LIB_FUNC("IxYiarKlXxM", Gen5::GraphicsAgcDmaDataPatchSetDstAddressOrOffset);
	LIB_FUNC("cdDRpqcFGbU", Gen5::GraphicsAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate);
	// Lkf86B98qPc: sceAgcGetPacketSize (type-3 dword length).
	LIB_FUNC("Lkf86B98qPc", Gen5::GraphicsGetDataPacketSizeDw);
	LIB_FUNC("f3dg2CSgRKY", Gen5::GraphicsCreateShader);
	LIB_FUNC("dolOmWH+huQ", Gen5::GraphicsUnknownGetFusedShaderSize);
	LIB_FUNC("fd5Bp5tGTgo", Gen5::GraphicsUnknownFuseShaderHalves);
	LIB_FUNC("vcmNN+AAXnY", Gen5::GraphicsSetCxRegIndirectPatchSetAddress);
	LIB_FUNC("Qrj4c+61z4A", Gen5::GraphicsSetShRegIndirectPatchSetAddress);
	LIB_FUNC("6lNcCp+fxi4", Gen5::GraphicsSetUcRegIndirectPatchSetAddress);
	LIB_FUNC("d-6uF9sZDIU", Gen5::GraphicsSetCxRegIndirectPatchAddRegisters);
	LIB_FUNC("z2duB-hHQSM", Gen5::GraphicsSetShRegIndirectPatchAddRegisters);
	LIB_FUNC("vRoArM9zaIk", Gen5::GraphicsSetUcRegIndirectPatchAddRegisters);
	// LtTouSCZjHM: sceAgcCbNop (encode NOP of length num_dw).
	LIB_FUNC("LtTouSCZjHM", Gen5::GraphicsCbNop);
	LIB_FUNC("-KRzWekV120", Gen5::GraphicsCbEmitDefaultIndexedUconfig);
	LIB_FUNC("t7PlZ9nt5Lc", Gen5::GraphicsCbNopGetSize);
	// WmAc2MEj6Io: sceAgcDcbDmaData. Distinct from MWiElSNE8j8 WaitUntilSafe.
	LIB_FUNC("WmAc2MEj6Io", Gen5::GraphicsDcbDmaData);
	LIB_FUNC("-RnpfpxIhec", Gen5::GraphicsAcbDmaData);
	LIB_FUNC("2ccJz9LQI+w", Gen5::GraphicsDcbDmaDataGetSize);
	// AGC GetSize helpers imported by the title (bytes; writer-safe).
	LIB_FUNC("ewobAQeMo5k", Gen5::GraphicsAcbAcquireMemGetSize);
	LIB_FUNC("CbQh3DKMSno", Gen5::GraphicsAcbCopyDataGetSize);
	LIB_FUNC("PxKWV2fVAps", Gen5::GraphicsAcbDispatchIndirectGetSize);
	LIB_FUNC("M0ttm8h7SKA", Gen5::GraphicsAcbDmaDataGetSize);
	LIB_FUNC("Y-5vneiBtzk", Gen5::GraphicsAcbEventWriteGetSize);
	LIB_FUNC("yUBESvCCJ4I", Gen5::GraphicsCbSetShRegistersDirectGetSize);
	LIB_FUNC("-vnlTPPXPrw", Gen5::GraphicsDcbAcquireMemGetSize);
	LIB_FUNC("b5u0Jzm8TF8", Gen5::GraphicsDcbCopyDataGetSize);
	LIB_FUNC("w8HVkEeXPv8", Gen5::GraphicsDcbDispatchIndirectGetSize);
	LIB_FUNC("WrdP9Zxx3lQ", Gen5::GraphicsDcbDrawIndexAutoGetSize);
	LIB_FUNC("6ee9Hd3EWXQ", Gen5::GraphicsDcbDrawIndexGetSize);
	LIB_FUNC("mStuvI0zOtc", Gen5::GraphicsDcbDrawIndexIndirectGetSize);
	LIB_FUNC("qMlfB1ZhMDc", Gen5::GraphicsDcbDrawIndexOffsetGetSize);
	LIB_FUNC("cxPZ4Wgvdj8", Gen5::GraphicsDcbDrawIndirectGetSize);
	LIB_FUNC("C4l9fB17t8w", Gen5::GraphicsDcbEventWriteGetSize);
	LIB_FUNC("rUuVjyR+Rd4", Gen5::GraphicsDcbGetLodStatsGetSize);
	LIB_FUNC("1DeUNpRIDDA", Gen5::GraphicsDcbSetCxRegisterDirectGetSize);
	LIB_FUNC("GBCh3zCihoU", Gen5::GraphicsDcbSetCxRegistersIndirectGetSize);
	LIB_FUNC("j4emHHndCPY", Gen5::GraphicsDcbSetIndexBufferGetSize);
	LIB_FUNC("mljzuGDZRQ4", Gen5::GraphicsDcbSetIndexCountGetSize);
	LIB_FUNC("ca4KPvp0qLQ", Gen5::GraphicsDcbSetIndexSizeGetSize);
	LIB_FUNC("6DFuRKT4C9w", Gen5::GraphicsDcbSetNumInstancesGetSize);
	LIB_FUNC("QhPDD513V0w", Gen5::GraphicsDcbSetShRegisterDirectGetSize);
	LIB_FUNC("nNlUtdDDvZ0", Gen5::GraphicsDcbSetShRegistersIndirectGetSize);
	LIB_FUNC("aP1Ki9G3++4", Gen5::GraphicsDcbSetUcRegisterDirectGetSize);
	LIB_FUNC("UQGTw4xRlcM", Gen5::GraphicsDcbSetUcRegistersIndirectGetSize);
	LIB_FUNC("p9tI+yTvx68", Gen5::GraphicsDcbWriteDataGetSize);
	LIB_FUNC("43WJ08sSugE", Gen5::GraphicsCbWaitOnAddressGetSize);
	LIB_FUNC("idlaArvdXEs", Gen5::GraphicsCbWaitOnAddressGetSize);
	LIB_FUNC("u2T2DiA5hRI", Gen5::GraphicsDcbStallCommandBufferParser);
	LIB_FUNC("+u6dKSLWM2o", Gen5::GraphicsDcbStallCommandBufferParserGetSize);
	LIB_FUNC("D9sr1xGUriE", Gen5::GraphicsCreatePrimState);
	LIB_FUNC("pdEV7bI6COI", Gen5::GraphicsCreateInterpolantMapping);
	LIB_FUNC("HV4j+E0MBHE", Gen5::GraphicsCreateInterpolantMapping);
	LIB_FUNC("V++UgBtQhn0", Gen5::GraphicsGetDataPacketPayloadAddress);
	LIB_FUNC("h9z6+0hEydk", Gen5::GraphicsSuspendPoint);
	LIB_FUNC("0fWWK5uG9rQ", Gen5::GraphicsAgcQueueEndOfPipeActionPatchAddress);
	LIB_FUNC("MlEw1feXcjg", Gen5::GraphicsAgcQueueEndOfPipeActionPatchData);
	LIB_FUNC("J8YCgfKAMQs", Gen5::GraphicsAgcQueueEndOfPipeActionPatchGcrCntl);
	LIB_FUNC("T9fjQIINoeE", Gen5::GraphicsAgcQueueEndOfPipeActionPatchType);
	LIB_FUNC("fPSCdQxgpSw", Gen5::GraphicsWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("eAy8eGNsCuU", Gen5::GraphicsWriteDataPatchSetCachePolicy);
	LIB_FUNC("tmy-+rBpspY", Gen5::GraphicsWriteDataPatchSetDst);
	LIB_FUNC("BfBDZGbti7A", Gen5::GraphicsGetIsTrinityMode);
	LIB_FUNC("T6xuVw0KUJo", Gen5::GraphicsDebugRaiseException);

	LIB_FUNC("n2fD4A+pb+g", Gen5::GraphicsCbSetShRegisterRangeDirect);
	LIB_FUNC("bxGoVxpdSPQ", Gen5::GraphicsCbSetShRegisterRangeDirectGetSize);
	LIB_FUNC("UZbQjYAwwXM", Gen5::GraphicsCbSetShRegistersDirect);
	LIB_FUNC("k3GhuSNmBLU", Gen5::GraphicsCbDispatch);
	LIB_FUNC("Abendgtz+3o", Gen5::GraphicsCbDispatchGetSize);
	LIB_FUNC("wr23dPKyWc0", Gen5::GraphicsCbReleaseMem);
	LIB_FUNC("TRO721eVt4g", Gen5::GraphicsDcbResetQueue);
	LIB_FUNC("JrtiDtKeS38", Gen5::GraphicsAcbResetQueue);
	LIB_FUNC("MWiElSNE8j8", Gen5::GraphicsDcbWaitUntilSafeForRendering);
	LIB_FUNC("LFSPFmGc9Hg", Gen5::GraphicsDcbSetWorkloadsActive);
	LIB_FUNC("hEK26Wdny6s", Gen5::GraphicsDcbSetWorkloadComplete);
	LIB_FUNC("LHFXRrlTPD8", Gen5::GraphicsDcbSetCxRegisterDirect);
	LIB_FUNC("pFLArOT53+w", Gen5::GraphicsDcbSetShRegisterDirect);
	LIB_FUNC("w4-d0n60hdo", Gen5::GraphicsDcbSetUcRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", Gen5::GraphicsDcbSetCxRegistersIndirect);
	LIB_FUNC("-HOOCn0JY48", Gen5::GraphicsDcbSetShRegistersIndirect);
	LIB_FUNC("hvUfkUIQcOE", Gen5::GraphicsDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", Gen5::GraphicsDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", Gen5::GraphicsDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", Gen5::GraphicsDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", Gen5::GraphicsDcbSetNumInstances);
	LIB_FUNC("bbFueFP+J4k", Gen5::GraphicsDcbSetPredication);
	LIB_FUNC("w6Dj1VJt5qY", Gen5::GraphicsSetPacketPredication);
	LIB_FUNC("xSAR0LTcRKM", Gen5::GraphicsCbJump);        // sceAgcDcbJump
	LIB_FUNC("e1DFTg+Sd8U", Gen5::GraphicsCbJump);        // sceAgcAcbJump
	LIB_FUNC("VEGu4dixjUg", Gen5::GraphicsCbJumpGetSize); // sceAgcDcbJumpGetSize
	LIB_FUNC("zfcxg-ewMK8", Gen5::GraphicsDcbRewind);
	LIB_FUNC("DwICrVxerkY", Gen5::GraphicsAcbRewind);
	LIB_FUNC("BIPexNBSGog", Gen5::GraphicsDcbCondExec);
	LIB_FUNC("qyM2bxYFPAk", Gen5::GraphicsAcbCondExec);
	LIB_FUNC("QIXCsbipds0", Gen5::GraphicsDcbRewindGetSize);
	LIB_FUNC("0ZOG0jc9nRg", Gen5::GraphicsAcbRewindGetSize);
	LIB_FUNC("ou16V5hh5sg", Gen5::GraphicsDcbCondExecGetSize);
	LIB_FUNC("ozKzBP4aki4", Gen5::GraphicsAcbCondExecGetSize);
	LIB_FUNC("b-oySn+G2tE", Gen5::GraphicsCbJumpGetSize); // sceAgcAcbJumpGetSize
	LIB_FUNC("w1KFAHVqpaU", Gen5::GraphicsCbBranch);        // sceAgcCbBranch
	LIB_FUNC("uZW-mqsxkrM", Gen5::GraphicsCbBranchGetSize); // sceAgcCbBranchGetSize
	LIB_FUNC("Yw0jKSqop+E", Gen5::GraphicsDcbDrawIndexAuto);
	// sceAgcDcbDrawIndexOffset NID B+aG9DUnTKA.
	// Misbinding this to DrawIndexAutoWithBase made UI quads ignore IndexBase
	// (indices 0,1,2,1,2,3) and walk sequential verts → shear + diagonal wipe.
	LIB_FUNC("B+aG9DUnTKA", Gen5::GraphicsDcbDrawIndexOffset);
	LIB_FUNC("q88lQ+GP5Yk", Gen5::GraphicsDcbDrawIndex);
	LIB_FUNC("aJf+j5yntiU", Gen5::GraphicsDcbEventWrite);
	LIB_FUNC("cFazmnXpJOE", Gen5::GraphicsAcbEventWrite);
	LIB_FUNC("57labkp+rSQ", Gen5::GraphicsDcbAcquireMem);
	LIB_FUNC("KT-hTp-Ch14", Gen5::GraphicsAcbAcquireMem);
	LIB_FUNC("i1jyy49AjXU", Gen5::GraphicsDcbWriteData);
	LIB_FUNC("eZ4+17OQz4Q", Gen5::GraphicsAcbWriteData);
	LIB_FUNC("qj7QZpgr9Uw", Gen5::GraphicsDcbContextStateOp);
	LIB_FUNC("H6vHS5cidSA", Gen5::GraphicsDcbContextStateOpGetSize);
	LIB_FUNC("RmaJwLtc8rY", Gen5::GraphicsDcbSetBaseIndirectArgs);
	LIB_FUNC("CtB+A9-VxO0", Gen5::GraphicsDcbDispatchIndirect);
	LIB_FUNC("j3EtxFkSIhQ", Gen5::GraphicsAcbDispatchIndirect);
	LIB_FUNC("t1vNu082-jM", Gen5::GraphicsDcbDrawIndexIndirect);
	LIB_FUNC("1q1titRBL6o", Gen5::GraphicsDcbDrawIndirect);
	LIB_FUNC("VmW0Tdpy420", Gen5::GraphicsDcbWaitRegMem);
	LIB_FUNC("htn36gPnBk4", Gen5::GraphicsAcbWaitRegMem);
	LIB_FUNC("1rZSWUv1IRc", Gen5::GraphicsDcbCopyData);
	LIB_FUNC("qzMN2XKGA4k", Gen5::GraphicsAcbCopyData);
	LIB_FUNC("+kSrjIVxKFE", Gen5::GraphicsDcbPushMarker);
	LIB_FUNC("QhCbS4X9Rl8", Gen5::GraphicsDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", Gen5::GraphicsDcbPopMarker);
	LIB_FUNC("cpCILPya5Zk", Gen5::GraphicsAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", Gen5::GraphicsAcbPopMarker);
	LIB_FUNC("xAeBOa0A3kk", Gen5::GraphicsAcbSetMarker);
	LIB_FUNC("pxx-GoOSdw4", Gen5::GraphicsAcbSetMarkerSpan);
	LIB_FUNC("8Kly1JrJUlw", Gen5::GraphicsAcbPushMarkerSpan);
	LIB_FUNC("BYcSvEsINWU", Gen5::GraphicsDcbSetMarkerSpan);
	LIB_FUNC("N7M3+N-sBCw", Gen5::GraphicsDcbPushMarkerSpan);
	LIB_FUNC("XKKuA6VkSRc", Gen5::GraphicsAcbAtomicMem);
	LIB_FUNC("1-gUn1PI4Sw", Gen5::GraphicsDcbAtomicMem);
	LIB_FUNC("q4VuU-QsLOE", Gen5::GraphicsAcbMemSemaphore);
	LIB_FUNC("G0jrLdvEqDw", Gen5::GraphicsDcbMemSemaphore);
	LIB_FUNC("FcgdDM3MB+k", Gen5::GraphicsAcbSetWorkloadStreamInactive);
	LIB_FUNC("FneFypEDRgY", Gen5::GraphicsDcbSetWorkloadStreamInactive);
	// Unnamed Astro AGC exports identified by decompiled packet layout.
	LIB_FUNC("gQkqkLttcpw", Gen5::GraphicsAcbAtomicGds);
	LIB_FUNC("zARR5aCmkoY", Gen5::GraphicsDcbAtomicGds);
	LIB_FUNC("OQTgEXyihvA", Gen5::GraphicsAgcSetAmmSemaphoreMemory);
	LIB_FUNC("hFQ9pUxoLQ4", Gen5::GraphicsAgcGetSemaphoreLabel);
	LIB_FUNC("AAeX-U5-P3M", Gen5::GraphicsAgcGetDefaultCxStateFlat);
	LIB_FUNC("NKIzURsgV7I", Gen5::GraphicsAgcGetGsOversubscription);
	LIB_FUNC("kUlvghKs-mA", Gen5::GraphicsDcbDrawIndirectMulti);
	LIB_FUNC("ypVBz4uPKcQ", Gen5::GraphicsDcbDrawIndexIndirectMulti);
	LIB_FUNC("Rlx+bykm0r0", Gen5::GraphicsDcbDrawIndexMultiInstanced);
	LIB_FUNC("vuSXe69VILM", Gen5::GraphicsDcbGetLodStats);
	LIB_FUNC("YUeqkyT7mEQ", Gen5::GraphicsDcbSetFlip);
	// AGC patch/helper family and its CondExec/Rewind packet writers.
	LIB_FUNC("K2mciNVxUCE", Gen5::GraphicsSetNop);
	LIB_FUNC("whb1RL7K4Ss", Gen5::GraphicsSetCxRegIndirectPatchSetNumRegisters);
	LIB_FUNC("nCUgItdN2ms", Gen5::GraphicsSetShRegIndirectPatchSetNumRegisters);
	LIB_FUNC("fRG-JOH5+sI", Gen5::GraphicsSetUcRegIndirectPatchSetNumRegisters);
	LIB_FUNC("d4NZIlguzv0", Gen5::GraphicsAgcAsyncWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("y5K5tPktiL8", Gen5::GraphicsAgcAsyncWriteDataPatchSetCachePolicy);
	LIB_FUNC("EJBA4dbmvfg", Gen5::GraphicsAgcAsyncWriteDataPatchSetDst);
	LIB_FUNC("eWaWyFegzgQ", Gen5::GraphicsAgcRewindPatchSetRewindState);
	LIB_FUNC("ziVA3whp3p4", Gen5::GraphicsAgcRewindPatchSetRewindState);
	LIB_FUNC("3ZWa3AoyWZQ", Gen5::GraphicsAgcCondExecPatchSetCommandAddress);
	LIB_FUNC("YWTKOju587o", Gen5::GraphicsAgcCondExecPatchSetCommandAddress);
	LIB_FUNC("k-JpyR2dYAM", Gen5::GraphicsAgcCondExecPatchSetEnd);
	LIB_FUNC("ORWsxIbk4TE", Gen5::GraphicsAgcCondExecPatchSetEnd);
	LIB_FUNC("GXBlM-ekzrI", Gen5::GraphicsAgcBranchPatchSetCompareAddress);
	LIB_FUNC("W0WEyog0f74", Gen5::GraphicsAgcAcquireMemSetEngine);
	LIB_FUNC("s+VGAMDQ0AQ", Gen5::GraphicsGetDataPacketPayloadRange);
	LIB_FUNC("n8vgpaQg6dA", Gen5::GraphicsSetPacketPredicationRange);
	LIB_FUNC("Y3ymLfZ1384", Gen5::GraphicsUpdatePrimState);
	LIB_FUNC("hcIxS8pmXF4", Gen5::GraphicsAcbAtomicGdsGetSize);
	LIB_FUNC("da1Sm8-QDoU", Gen5::GraphicsAcbAtomicMemGetSize);
	LIB_FUNC("eCjKaqeeQ5s", Gen5::GraphicsAcbPrimeUtcl2GetSize);
	LIB_FUNC("F8NLhWvFemI", Gen5::GraphicsAcbQueueEndOfShaderActionGetSize);
	LIB_FUNC("FuVbkyKlf+s", Gen5::GraphicsCbCondWriteGetSize);
	LIB_FUNC("hL7C0IRpWZI", Gen5::GraphicsCbQueueEndOfPipeActionGetSize);
	LIB_FUNC("JOWmDrl+j20", Gen5::GraphicsCbSetUcRegisterRangeDirectGetSize);
	LIB_FUNC("TGEZzUWLbrc", Gen5::GraphicsCbSetUcRegistersDirectGetSize);
	LIB_FUNC("1tB0xkLNjcw", Gen5::GraphicsDcbAtomicGdsGetSize);
	LIB_FUNC("oz6zQq1JwCE", Gen5::GraphicsDcbAtomicMemGetSize);
	LIB_FUNC("ms1xVoZ-Vwc", Gen5::GraphicsDcbBeginOcclusionQueryGetSize);
	LIB_FUNC("r98I08t+LOg", Gen5::GraphicsDcbDrawIndexIndirectMultiGetSize);
	LIB_FUNC("mR9j7+SfM34", Gen5::GraphicsDcbDrawIndexMultiInstancedGetSize);
	LIB_FUNC("pYoKs3lPy88", Gen5::GraphicsDcbDrawIndirectMultiGetSize);
	LIB_FUNC("P1CugZ99Uzc", Gen5::GraphicsDcbEndOcclusionQueryGetSize);
	LIB_FUNC("KjPeVduz6jU", Gen5::GraphicsDcbPrimeUtcl2GetSize);
	LIB_FUNC("zg6u-N6Otxs", Gen5::GraphicsDcbQueueEndOfShaderActionGetSize);
	LIB_FUNC("9S4noWrUI0s", Gen5::GraphicsDcbSetBaseDispatchIndirectArgsGetSize);
	LIB_FUNC("MMlmJAL7N5w", Gen5::GraphicsDcbSetBaseDrawIndirectArgsGetSize);
	LIB_FUNC("yheJGN-ay+A", Gen5::GraphicsDcbSetBoolPredicationEnableGetSize);
	LIB_FUNC("AFIh8SQkYlQ", Gen5::GraphicsDcbSetIndexIndirectArgsGetSize);
	LIB_FUNC("vLrBL8DQiz8", Gen5::GraphicsDcbSetPredicationDisableGetSize);
	LIB_FUNC("XN+Iuu7XsM8", Gen5::GraphicsDcbSetZPassPredicationEnableGetSize);
	LIB_FUNC("MDLD5Ly94Xk", Gen5::GraphicsCbSetUcRegisterRangeDirect);
	LIB_FUNC("03RZmELWWzw", Gen5::GraphicsCbSetUcRegistersDirect);
	LIB_FUNC("0o3VDdtA6nM", Gen5::GraphicsDcbSetIndexIndirectArgs);
	LIB_FUNC("rVOmPz2RBlg", Gen5::GraphicsAcbSetWorkloadsActive);
	LIB_FUNC("opR1JeJZCBU", Gen5::GraphicsAcbSetWorkloadComplete);
	LIB_FUNC("GPbUp9jXQa8", Gen5::GraphicsAcbWaitUntilSafeForRendering);
	LIB_FUNC("ebixW91gpPw", Gen5::GraphicsAcbSetFlip);
	LIB_FUNC("jt3pl7EN17o", Gen5::GraphicsDcbPrimeUtcl2);
	LIB_FUNC("szG7hz2yEhA", Gen5::GraphicsAcbPrimeUtcl2);
	LIB_FUNC("7toV+elXqNM", Gen5::GraphicsCbCondWrite);
	// Unnamed AGC exports identified by packet layout (Jump/Branch patches).
	LIB_FUNC("Ikfdt-rIqCE", Gen5::GraphicsAgcJumpPatchTarget);
	LIB_FUNC("7Wa3aeJgeVU", Gen5::GraphicsAgcBranchPatchSetThenTarget);
	LIB_FUNC("rP5xLdOf26k", Gen5::GraphicsAgcBranchPatchSetElseTarget);
}

} // namespace LibAgc

namespace LibAgcDriver {

LIB_VERSION("AgcDriver", 1, "AgcDriver", 1, 1);

namespace Gen5Driver = Graphics::Gen5Driver;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("AOLcoIkQDgM", Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements);
	LIB_FUNC("F0Y42t-3e18", Gen5Driver::GraphicsDriverInitResourceRegistration);
	LIB_FUNC("U9ueyEhSkF4", Gen5Driver::GraphicsDriverRegisterDefaultOwner);
	LIB_FUNC("F0ZXt5q0ZTA", Gen5Driver::GraphicsDriverGetDefaultOwner);
	LIB_FUNC("uJziRsODk1c", Gen5Driver::GraphicsDriverGetResourceRegistrationMaxNameLength);
	LIB_FUNC("X-Nm5KLREeg", Gen5Driver::GraphicsDriverRegisterOwner);
	LIB_FUNC("W5z4eZrjEas", Gen5Driver::GraphicsDriverRegisterResource);
	LIB_FUNC("pWLG7WOpVcw", Gen5Driver::GraphicsDriverUnregisterResource);
	LIB_FUNC("3AyTaWcF-H8", Graphics::Gen5::GraphicsDriverRegisterWorkloadStream);
	LIB_FUNC("UglJIZjGssM", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("AhGvpITrf4M", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("6UzEidRZwkg", Gen5Driver::GraphicsDriverSubmitMultiDcbs);
	LIB_FUNC("+T8Xo6LtFJI", Gen5Driver::GraphicsDriverAgrSubmitMultiDcbs);
	LIB_FUNC("gSRnr79F8tQ", Gen5Driver::GraphicsDriverSubmitAcb);
	LIB_FUNC("HF3YllT3mXU", Gen5Driver::GraphicsDriverSubmitMultiAcbs);
	LIB_FUNC("DL2RXaXOy88", Gen5Driver::GraphicsDriverDeleteEqEvent);
	LIB_FUNC("5CdQTZIQPxM", Gen5Driver::GraphicsDriverGetEqEventType);
	LIB_FUNC("w2rJhmD+dsE", Gen5Driver::GraphicsDriverAddEqEvent);
	LIB_FUNC("Zw7uUVPulbw", Gen5Driver::GraphicsDriverGetEqContextId);
	LIB_FUNC("XlNp7jzGiPo", Gen5Driver::GraphicsDriverSetTFRing);
	LIB_FUNC("MM4IZSEYytQ", Gen5Driver::GraphicsDriverSetHsOffchipParam);
	LIB_FUNC("n5ElQVYsU1A", Graphics::Gen5::GraphicsDriverUnregisterWorkloadStream);
	// Astro 01.018 fakelib stubs: exact constant returns, grouped by value.
	LIB_FUNC("5l3IfCFJxBs", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("LepGrgk77sM", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("NghWEUXp1qM", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("M9yBzRKkjPc", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("mXn+K9E-wOA", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("rI9lNAXPMIw", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("ls4jfY576lw", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("emP3ckeS2uo", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("VOMSpd9+vxU", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("SCoAN5fYlUM", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("ZLJk9r2+2Aw", Gen5Driver::GraphicsDriverResourceApiUnavailable);
	LIB_FUNC("ICaGtkEIXTk", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("WHIOMbb+iIU", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("SAfhzJPcjuk", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("FOwvmNlFLjM", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("chJWZcNSzjk", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("+iAOE3jCnkk", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("8F5j7U-Bq6s", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("Xq5WmbwPTnQ", Gen5Driver::GraphicsDriverDebugApiUnavailable);
	LIB_FUNC("Ddwk4gLT5j0", Gen5Driver::GraphicsDriverStatusDisabled);
	LIB_FUNC("qspAL8bgcBY", Gen5Driver::GraphicsDriverStatusDisabled);
	LIB_FUNC("+TN0oRTBxJQ", Gen5Driver::GraphicsDriverStatusDisabled);
	LIB_FUNC("rJUyMrDdxJg", Gen5Driver::GraphicsDriverShaderDebuggingStatus);
}

} // namespace LibAgcDriver


LIB_DEFINE(InitGraphicsDriver_1)
{
	LibGen4::InitGraphicsDriver_1(s);
	LibAgc::InitGraphicsDriver_1(s);
	LibAgcDriver::InitGraphicsDriver_1(s);
}

} // namespace Kyty::Hle::GraphicsRegistration

#endif // KYTY_EMU_ENABLED
