#pragma once

#include "Emulator/Common.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kShaderProgramSnapshotBytesMax = 256u * 1024u;

enum class ShaderProgramSnapshotStatus: uint8_t
{
	Unmapped,
	InvalidRange,
	Unreadable,
	Complete,
	Truncated,
};

struct ShaderProgramSnapshot
{
	ShaderProgramSnapshotStatus status = ShaderProgramSnapshotStatus::Unmapped;
	uint32_t mapped_bytes = 0;
	std::vector<uint32_t> words;
};

// Copies only a metadata-bounded program through the guest readable-range lease.
// The owned bytes outlive both leases; filesystem I/O belongs to the caller.
[[nodiscard]] ShaderProgramSnapshot ShaderSnapshotMappedProgram(uint64_t address, uint32_t max_bytes = kShaderProgramSnapshotBytesMax);
[[nodiscard]] const char* ShaderProgramSnapshotStatusName(ShaderProgramSnapshotStatus status);

} // namespace Kyty::Libs::Graphics

#endif
