#include "Emulator/Graphics/FragmentTransportShaders.h"

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

#include "host_shaders/fragment_wave_pack_comp.inc"
#include "host_shaders/fragment_wave_scan_comp.inc"

} // namespace

Binary GetKernelBinary(Kernel kernel) noexcept
{
	switch (kernel)
	{
		case Kernel::Scan: return {kFragmentWaveScanSpirv, sizeof(kFragmentWaveScanSpirv) / sizeof(uint32_t)};
		case Kernel::Pack: return {kFragmentWavePackSpirv, sizeof(kFragmentWavePackSpirv) / sizeof(uint32_t)};
	}
	return {};
}

} // namespace Kyty::Libs::Graphics::FragmentTransport
