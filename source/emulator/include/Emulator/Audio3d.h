#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO3D_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO3D_H_

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::Audio::Audio3d {

struct Audio3dOpenParameters
{
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	uint32_t num_beds    = 2;
};

} // namespace Kyty::Libs::Audio::Audio3d

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO3D_H_ */
