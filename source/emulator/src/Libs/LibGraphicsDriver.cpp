#include "Emulator/Common.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibGen4 {

LIB_VERSION("GraphicsDriver", 1, "GraphicsDriver", 1, 1);

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

namespace LibGen5 {

LIB_VERSION("Graphics5", 1, "Graphics5", 1, 1);

namespace Gen5 = Graphics::Gen5;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("23LRUSvYu1M", Gen5::GraphicsInit);
	LIB_FUNC("dbOlWdppb4o", Gen5::GraphicsBuildDescriptorTable);
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
	LIB_FUNC("t7PlZ9nt5Lc", Gen5::GraphicsCbNopGetSize);
	// WmAc2MEj6Io: sceAgcDcbDmaData. Distinct from MWiElSNE8j8 WaitUntilSafe.
	LIB_FUNC("WmAc2MEj6Io", Gen5::GraphicsDcbDmaData);
	LIB_FUNC("-RnpfpxIhec", Gen5::GraphicsDcbDmaData); // sceAgcAcbDmaData alias
	LIB_FUNC("2ccJz9LQI+w", Gen5::GraphicsDcbDmaDataGetSize);
	LIB_FUNC("u2T2DiA5hRI", Gen5::GraphicsDcbStallCommandBufferParser);
	LIB_FUNC("+u6dKSLWM2o", Gen5::GraphicsDcbStallCommandBufferParserGetSize);
	LIB_FUNC("D9sr1xGUriE", Gen5::GraphicsCreatePrimState);
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
	LIB_FUNC("pFLArOT53+w", Gen5::GraphicsDcbSetShRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", Gen5::GraphicsDcbSetCxRegistersIndirect);
	LIB_FUNC("-HOOCn0JY48", Gen5::GraphicsDcbSetShRegistersIndirect);
	LIB_FUNC("hvUfkUIQcOE", Gen5::GraphicsDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", Gen5::GraphicsDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", Gen5::GraphicsDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", Gen5::GraphicsDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", Gen5::GraphicsDcbSetNumInstances);
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
	LIB_FUNC("qj7QZpgr9Uw", Gen5::GraphicsCbType2Pad);
	LIB_FUNC("RmaJwLtc8rY", Gen5::GraphicsDcbSetBaseIndirectArgs);
	LIB_FUNC("CtB+A9-VxO0", Gen5::GraphicsDcbDispatchIndirect);
	LIB_FUNC("t1vNu082-jM", Gen5::GraphicsDcbDrawIndexIndirect);
	LIB_FUNC("VmW0Tdpy420", Gen5::GraphicsDcbWaitRegMem);
	LIB_FUNC("htn36gPnBk4", Gen5::GraphicsAcbWaitRegMem);
	LIB_FUNC("1rZSWUv1IRc", Gen5::GraphicsDcbCopyData);
	LIB_FUNC("qzMN2XKGA4k", Gen5::GraphicsAcbCopyData);
	LIB_FUNC("+kSrjIVxKFE", Gen5::GraphicsDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", Gen5::GraphicsDcbPopMarker);
	LIB_FUNC("cpCILPya5Zk", Gen5::GraphicsAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", Gen5::GraphicsAcbPopMarker);
	LIB_FUNC("vuSXe69VILM", Gen5::GraphicsDcbGetLodStats);
	LIB_FUNC("YUeqkyT7mEQ", Gen5::GraphicsDcbSetFlip);
	LIB_FUNC("vuSXe69VILM", Gen5::GraphicsDcbGetLodStats);
	LIB_FUNC("BfBDZGbti7A", Gen5::GraphicsGetIsTrinityMode);
}

} // namespace LibGen5

namespace LibGen5Driver {

LIB_VERSION("Graphics5Driver", 1, "Graphics5Driver", 1, 1);

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
	LIB_FUNC("UglJIZjGssM", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("AhGvpITrf4M", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("gSRnr79F8tQ", Gen5Driver::GraphicsDriverSubmitAcb);
	LIB_FUNC("w2rJhmD+dsE", Gen5Driver::GraphicsDriverAddEqEvent);
	LIB_FUNC("XlNp7jzGiPo", Gen5Driver::GraphicsDriverSetTFRing);
	LIB_FUNC("MM4IZSEYytQ", Gen5Driver::GraphicsDriverSetHsOffchipParam);
}

} // namespace LibGen5Driver

namespace LibAgc {

// Guest imports libSceAgc; module name "Agc" matches the strip of Sce/lib prefixes.
// Astro Bot (and other Gen5 titles) resolve builders from Agc, not only Graphics5.
// Keep the builder surface in sync with LibGen5 for libSceAgc imports.
LIB_VERSION("Agc", 1, "Agc", 1, 1);

namespace Gen5 = Graphics::Gen5;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("-KRzWekV120", Gen5::GraphicsAgcDriverUnknownKRzWekV120);

	// Command builders / patches used by Astro / Gen5 AGC (same NIDs as Graphics5).
	LIB_FUNC("LtTouSCZjHM", Gen5::GraphicsCbNop);
	LIB_FUNC("t7PlZ9nt5Lc", Gen5::GraphicsCbNopGetSize);
	LIB_FUNC("WmAc2MEj6Io", Gen5::GraphicsDcbDmaData);
	LIB_FUNC("-RnpfpxIhec", Gen5::GraphicsDcbDmaData);
	LIB_FUNC("2ccJz9LQI+w", Gen5::GraphicsDcbDmaDataGetSize);
	LIB_FUNC("u2T2DiA5hRI", Gen5::GraphicsDcbStallCommandBufferParser);
	LIB_FUNC("+u6dKSLWM2o", Gen5::GraphicsDcbStallCommandBufferParserGetSize);
	LIB_FUNC("TRO721eVt4g", Gen5::GraphicsDcbResetQueue);
	LIB_FUNC("JrtiDtKeS38", Gen5::GraphicsAcbResetQueue);
	LIB_FUNC("MWiElSNE8j8", Gen5::GraphicsDcbWaitUntilSafeForRendering);
	LIB_FUNC("pFLArOT53+w", Gen5::GraphicsDcbSetShRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", Gen5::GraphicsDcbSetCxRegistersIndirect);
	LIB_FUNC("-HOOCn0JY48", Gen5::GraphicsDcbSetShRegistersIndirect);
	LIB_FUNC("hvUfkUIQcOE", Gen5::GraphicsDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", Gen5::GraphicsDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", Gen5::GraphicsDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", Gen5::GraphicsDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", Gen5::GraphicsDcbSetNumInstances);
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
	LIB_FUNC("qj7QZpgr9Uw", Gen5::GraphicsCbType2Pad);
	LIB_FUNC("RmaJwLtc8rY", Gen5::GraphicsDcbSetBaseIndirectArgs);
	LIB_FUNC("CtB+A9-VxO0", Gen5::GraphicsDcbDispatchIndirect);
	LIB_FUNC("t1vNu082-jM", Gen5::GraphicsDcbDrawIndexIndirect);
	LIB_FUNC("VmW0Tdpy420", Gen5::GraphicsDcbWaitRegMem);
	LIB_FUNC("htn36gPnBk4", Gen5::GraphicsAcbWaitRegMem);
	LIB_FUNC("1rZSWUv1IRc", Gen5::GraphicsDcbCopyData);
	LIB_FUNC("qzMN2XKGA4k", Gen5::GraphicsAcbCopyData);
	LIB_FUNC("+kSrjIVxKFE", Gen5::GraphicsDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", Gen5::GraphicsDcbPopMarker);
	LIB_FUNC("cpCILPya5Zk", Gen5::GraphicsAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", Gen5::GraphicsAcbPopMarker);
	LIB_FUNC("vuSXe69VILM", Gen5::GraphicsDcbGetLodStats);
	LIB_FUNC("YUeqkyT7mEQ", Gen5::GraphicsDcbSetFlip);
	LIB_FUNC("n2fD4A+pb+g", Gen5::GraphicsCbSetShRegisterRangeDirect);
	LIB_FUNC("bxGoVxpdSPQ", Gen5::GraphicsCbSetShRegisterRangeDirectGetSize);
	LIB_FUNC("UZbQjYAwwXM", Gen5::GraphicsCbSetShRegistersDirect);
	LIB_FUNC("k3GhuSNmBLU", Gen5::GraphicsCbDispatch);
	LIB_FUNC("Abendgtz+3o", Gen5::GraphicsCbDispatchGetSize);
	LIB_FUNC("wr23dPKyWc0", Gen5::GraphicsCbReleaseMem);
	LIB_FUNC("23LRUSvYu1M", Gen5::GraphicsInit);
	LIB_FUNC("2JtWUUiYBXs", Gen5::GraphicsGetRegisterDefaults2);
	LIB_FUNC("wRbq6ZjNop4", Gen5::GraphicsGetRegisterDefaults2Internal);
	LIB_FUNC("f3dg2CSgRKY", Gen5::GraphicsCreateShader);
	LIB_FUNC("dolOmWH+huQ", Gen5::GraphicsUnknownGetFusedShaderSize);
	LIB_FUNC("fd5Bp5tGTgo", Gen5::GraphicsUnknownFuseShaderHalves);
	LIB_FUNC("vcmNN+AAXnY", Gen5::GraphicsSetCxRegIndirectPatchSetAddress);
	LIB_FUNC("Qrj4c+61z4A", Gen5::GraphicsSetShRegIndirectPatchSetAddress);
	LIB_FUNC("6lNcCp+fxi4", Gen5::GraphicsSetUcRegIndirectPatchSetAddress);
	LIB_FUNC("d-6uF9sZDIU", Gen5::GraphicsSetCxRegIndirectPatchAddRegisters);
	LIB_FUNC("z2duB-hHQSM", Gen5::GraphicsSetShRegIndirectPatchAddRegisters);
	LIB_FUNC("vRoArM9zaIk", Gen5::GraphicsSetUcRegIndirectPatchAddRegisters);
	LIB_FUNC("D9sr1xGUriE", Gen5::GraphicsCreatePrimState);
	LIB_FUNC("HV4j+E0MBHE", Gen5::GraphicsCreateInterpolantMapping);
	LIB_FUNC("V++UgBtQhn0", Gen5::GraphicsGetDataPacketPayloadAddress);
	LIB_FUNC("h9z6+0hEydk", Gen5::GraphicsSuspendPoint);
	LIB_FUNC("0fWWK5uG9rQ", Gen5::GraphicsAgcQueueEndOfPipeActionPatchAddress);
	LIB_FUNC("MlEw1feXcjg", Gen5::GraphicsAgcQueueEndOfPipeActionPatchData);
	LIB_FUNC("J8YCgfKAMQs", Gen5::GraphicsAgcQueueEndOfPipeActionPatchGcrCntl);
	LIB_FUNC("T9fjQIINoeE", Gen5::GraphicsAgcQueueEndOfPipeActionPatchType);
	LIB_FUNC("3KDcnM3lrcU", Gen5::GraphicsAgcWaitRegMemPatchAddress);
	LIB_FUNC("n485EBnIWmk", Gen5::GraphicsAgcWaitRegMemPatchCompareFunction);
	LIB_FUNC("7nOoijNPvEU", Gen5::GraphicsAgcWaitRegMemPatchReference);
	LIB_FUNC("hXAnLgDHCoI", Gen5::GraphicsAgcWaitRegMemPatchMask);
	LIB_FUNC("IxYiarKlXxM", Gen5::GraphicsAgcDmaDataPatchSetDstAddressOrOffset);
	LIB_FUNC("cdDRpqcFGbU", Gen5::GraphicsAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate);
	LIB_FUNC("Lkf86B98qPc", Gen5::GraphicsGetDataPacketSizeDw);
	LIB_FUNC("fPSCdQxgpSw", Gen5::GraphicsWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("eAy8eGNsCuU", Gen5::GraphicsWriteDataPatchSetCachePolicy);
	LIB_FUNC("tmy-+rBpspY", Gen5::GraphicsWriteDataPatchSetDst);
	LIB_FUNC("BfBDZGbti7A", Gen5::GraphicsGetIsTrinityMode);
	LIB_FUNC("T6xuVw0KUJo", Gen5::GraphicsDebugRaiseException);
}

} // namespace LibAgc

LIB_DEFINE(InitGraphicsDriver_1)
{
	LibGen4::InitGraphicsDriver_1(s);
	LibGen5::InitGraphicsDriver_1(s);
	LibGen5Driver::InitGraphicsDriver_1(s);
	LibAgc::InitGraphicsDriver_1(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
