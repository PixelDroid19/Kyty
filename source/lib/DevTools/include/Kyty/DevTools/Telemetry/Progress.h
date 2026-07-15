#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_PROGRESS_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_PROGRESS_H_

#include "Kyty/DevTools/Telemetry/Event.h"
#include "Kyty/DevTools/Telemetry/WriterRegistry.h"

#include <atomic>
#include <cstdint>

namespace Kyty::DevTools {

enum class ProgressState: uint16_t
{
	Unknown     = 0,
	Idle        = 1,
	Active      = 2,
	Waiting     = 3,
	Closed      = 4,
	Unavailable = 5,
	Count       = 6
};

enum class ProgressFlag: uint16_t
{
	OperationValid   = 0x01,
	CorrelationValid = 0x02,
	Auxiliary0Valid  = 0x04,
	Auxiliary1Valid  = 0x08,
	TimedWait        = 0x10
};

struct ProgressKey
{
	Domain   domain   = Domain::Unknown;
	uint64_t instance = 0;

	// Pack: bits 56..63 = domain, bits 0..55 = instance (nonzero instance).
	[[nodiscard]] bool TryPack(uint64_t* out) const noexcept;
	[[nodiscard]] static bool TryUnpack(uint64_t wire, ProgressKey* out) noexcept;
};

struct ProgressToken
{
	uint16_t domain     = 0;
	uint16_t slot       = 0;
	uint32_t generation = 0;
};

struct ProgressRecord
{
	uint64_t instance_key    = 0;
	uint64_t last_change_ns  = 0;
	uint64_t submitted       = 0;
	uint64_t completed       = 0;
	uint32_t operation       = 0;
	uint16_t state           = 0;
	uint16_t flags           = 0;
	uint64_t correlation     = 0;
	uint64_t auxiliary[2]    = {};
};

struct ProgressUpdate
{
	uint64_t       epoch         = 0;
	OperationCode  operation     = OperationCode::Unknown;
	ProgressState  state         = ProgressState::Unknown;
	uint16_t       flags         = 0;
	uint64_t       correlation   = 0;
	uint64_t       auxiliary[2]  = {};
	uint64_t       monotonic_ns  = 0;
};

inline constexpr uint32_t MaxProgressSnapshotEntries = 1504u;

struct ProgressSnapshotEntry
{
	ProgressKey    key {};
	ProgressRecord record {};
};

struct ProgressSnapshot
{
	ProgressSnapshotEntry entries[MaxProgressSnapshotEntries] {};
	uint32_t              count              = 0;
	uint32_t              unavailable_count  = 0;
	uint64_t              inventory_generation = 0;
};

struct ProgressLossSnapshot
{
	GlobalLossCounter capacity[static_cast<uint32_t>(Domain::Count)] {};
	GlobalLossCounter rejected_update[static_cast<uint32_t>(Domain::Count)] {};
	uint64_t          max_loss_monotonic_ns = 0;
};

// Per-domain slot budgets (sum == MaxProgressSnapshotEntries).
inline constexpr uint32_t kProgressSlotsGuestThread      = 256u;
inline constexpr uint32_t kProgressSlotsHle              = 512u;
inline constexpr uint32_t kProgressSlotsCommandProcessor = 128u;
inline constexpr uint32_t kProgressSlotsRenderer         = 128u;
inline constexpr uint32_t kProgressSlotsGpuQueue         = 128u;
inline constexpr uint32_t kProgressSlotsVideoOut         = 64u;
inline constexpr uint32_t kProgressSlotsPresentation     = 64u;
inline constexpr uint32_t kProgressSlotsSynchronization  = 224u;

class ProgressRegistry
{
public:
	ProgressRegistry() noexcept = default;

	bool Register(ProgressKey key, ProgressToken* out) noexcept;
	bool Submit(ProgressToken token, const ProgressUpdate& update) noexcept;
	bool Advance(ProgressToken token, const ProgressUpdate& update) noexcept;
	bool Complete(ProgressToken token, uint64_t epoch, uint64_t now_ns) noexcept;
	void Close(ProgressToken token) noexcept;
	bool Snapshot(ProgressToken token, ProgressRecord* out) const noexcept;
	bool SnapshotAll(ProgressSnapshot* out) const noexcept;
	ProgressLossSnapshot SnapshotLoss() const noexcept;

private:
	struct Slot
	{
		std::atomic<uint32_t> generation {0};
		std::atomic<uint32_t> live {0}; // 0 free, 1 live, 2 closed-reusable after key clear
		ProgressKey           key {};
		ProgressRecord        record {};
		bool                  retired {false};
	};

	[[nodiscard]] static uint32_t DomainSlotCount(Domain d) noexcept;
	[[nodiscard]] static uint32_t DomainBase(Domain d) noexcept;
	[[nodiscard]] bool TokenMatches(const ProgressToken& token) const noexcept;
	[[nodiscard]] bool ValidateUpdate(Domain domain, const ProgressUpdate& update, bool is_submit) const noexcept;
	void NoteCapacity(Domain d, uint64_t now) noexcept;
	void NoteRejected(Domain d, uint64_t now) noexcept;
	void NoteMaxLoss(uint64_t now) noexcept;

	Slot                  slots_[MaxProgressSnapshotEntries] {};
	std::atomic<uint64_t> inventory_generation_ {0};
	ProgressLossSnapshot  loss_ {};
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_PROGRESS_H_ */
