#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"

UT_BEGIN(EmulatorComputeWaveIdentity);

using namespace Libs::Graphics;

namespace {

void InitializeNextGenConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
}

HW::ComputeShaderInfo SyntheticComputeShader()
{
	HW::ComputeShaderInfo shader {};
	shader.cs_regs.data_addr = 0x100000u;
	shader.cs_regs.chksum    = 0x1122334455667788ull;
	return shader;
}

ShaderComputeInputInfo SyntheticPairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	input.wave_layout    = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	return input;
}

} // namespace

TEST(EmulatorComputeWaveIdentity, SeparatesNativeAndPairedStrategies)
{
	InitializeNextGenConfig();
	const auto shader = SyntheticComputeShader();

	// Keep every other translation input fixed to isolate the strategy field.
	auto native                 = SyntheticPairedInput();
	native.wave_layout.strategy = ShaderComputeWaveStrategy::Native;
	const auto paired           = SyntheticPairedInput();

	EXPECT_NE(ShaderGetIdCS(&shader, &native), ShaderGetIdCS(&shader, &paired));
}

TEST(EmulatorComputeWaveIdentity, SeparatesPairedPhysicalLocalShapes)
{
	InitializeNextGenConfig();
	const auto shader                  = SyntheticComputeShader();
	auto       narrow                  = SyntheticPairedInput();
	auto       wide                    = narrow;
	wide.wave_layout.physical_local[0] = 64;

	// This identity-level synthetic delta deliberately holds guest shape fixed.
	EXPECT_NE(ShaderGetIdCS(&shader, &narrow), ShaderGetIdCS(&shader, &wide));
}

TEST(EmulatorComputeWaveIdentity, DistinguishesGuestShapeAndIgnoresGetpcBaseValue)
{
	InitializeNextGenConfig();
	const auto shader                          = SyntheticComputeShader();
	auto       guest_shape_64                  = SyntheticPairedInput();
	auto       guest_shape_128                 = guest_shape_64;
	guest_shape_128.threads_num[0]             = 128;
	guest_shape_128.wave_layout.guest_local[0] = 128;
	EXPECT_NE(ShaderGetIdCS(&shader, &guest_shape_64), ShaderGetIdCS(&shader, &guest_shape_128));

	auto first_dispatch                        = guest_shape_64;
	first_dispatch.bind.program_base_used      = true;
	first_dispatch.bind.program_base_offset_dw = 4;
	first_dispatch.bind.program_base           = 0x200000u;
	auto second_dispatch                       = first_dispatch;
	second_dispatch.bind.program_base          = 0x400000u;
	EXPECT_EQ(ShaderGetIdCS(&shader, &first_dispatch), ShaderGetIdCS(&shader, &second_dispatch));

	// Group counts are absent from ShaderComputeInputInfo and ShaderGetIdCS's
	// signature. Their per-dispatch exclusion needs renderer-path validation.
}

UT_END();
