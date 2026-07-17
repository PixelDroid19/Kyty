#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_BUNDLEWRITER_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_BUNDLEWRITER_H_

#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Diagnostics/StallClassifier.h"
#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Supervisor/DurableFile.h"

#include <cstdint>

namespace Kyty::DevTools {

enum class BundleWriteResult: uint8_t
{
	Ok              = 0,
	InvalidInput    = 1,
	Conflict        = 2,
	IoError         = 3,
	DurabilityError = 4
};

// Trigger reason recorded in the manifest (numeric / allowlisted only).
enum class BundleTrigger: uint32_t
{
	Unknown            = 0,
	SuspectedStall     = 1,
	ConfirmedStall     = 2,
	ProcessTerminal    = 3,
	StatusDecodeError  = 4,
	Manual             = 5
};

struct BundleInput
{
	const ProgressPublication*  publication  = nullptr;
	const TimelineSnapshot*     timeline     = nullptr;
	const WaitGraphSnapshot*    wait_graph   = nullptr;
	const ClassifierState*      classifier   = nullptr;
	const StallResult*          result       = nullptr;
	ProcessStatus               process {};
	ProtocolHealthSnapshot      health {};
	uint64_t                    bundle_generation = 0;
	BundleTrigger               trigger           = BundleTrigger::Unknown;
	// Optional provenance (allowlisted numeric / fixed enums only).
	LoggingMode                 logging_mode       = LoggingMode::Unknown;
	ShaderCacheState            shader_cache_state = ShaderCacheState::Unknown;
	uint32_t                    validation_enabled = 0;
	uint32_t                    resolution_width   = 0;
	uint32_t                    resolution_height  = 0;
	const char*                 revision_hex40     = nullptr; // 40 hex or null
	uint32_t                    dirty              = 0;
	// Test clock; null uses DefaultDurableClock.
	DurableClockNs              clock = nullptr;
};

struct BundlePath
{
	char     bytes[1024] = {};
	uint32_t size        = 0;
};

// complete.marker: 64 bytes LE.
inline constexpr uint64_t kBundleMarkerMagic       = 0x31444e425454594Bull; // 'K''Y''T''D''B''N''D''1'
inline constexpr uint16_t kBundleSchemaMajor       = 1u;
inline constexpr uint16_t kBundleSchemaMinor       = 0u;
inline constexpr uint32_t kBundleMarkerSize        = 64u;

// timeline.bin header constants.
inline constexpr uint64_t kTimelineFileMagic       = 0x31304c544454594Bull; // 'K''Y''T''D''T''L''0''1'
inline constexpr uint32_t kTimelineHeaderSize      = 64u;
inline constexpr uint32_t kTimelineRecordSize      = 72u;
inline constexpr uint32_t kTimelineByteOrderTag    = 0x01020304u;
inline constexpr uint32_t kTimelineDictionaryVer   = 1u;

// Temp age threshold for orphan cleanup (24 hours in ns).
inline constexpr uint64_t kBundleTempMaxAgeNs = 24ull * 60ull * 60ull * 1000000000ull;

[[nodiscard]] BundleWriteResult WriteBundle(const char* absolute_output_dir, const BundleInput& input,
                                            BundlePath* completed_path) noexcept;

// Pure validators / format helpers used by tests and the writer.
[[nodiscard]] bool ValidateTimelineFile(const uint8_t* data, uint64_t size) noexcept;
[[nodiscard]] bool ValidateCompleteMarker(const uint8_t* data, uint64_t size, uint64_t expected_manifest_size,
                                          uint64_t expected_manifest_crc, uint64_t expected_generation) noexcept;

// Build timeline.bin bytes into caller buffer; returns total size or 0 on error.
// Buffer must hold kTimelineHeaderSize + count * kTimelineRecordSize.
[[nodiscard]] uint64_t EncodeTimelineFile(const TimelineSnapshot& timeline, uint8_t* out,
                                          uint64_t out_capacity) noexcept;

[[nodiscard]] bool EncodeCompleteMarker(uint64_t manifest_size, uint64_t manifest_crc,
                                        uint64_t generation, uint8_t out[kBundleMarkerSize]) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_BUNDLEWRITER_H_ */
