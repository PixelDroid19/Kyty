#ifndef INCLUDE_KYTY_SYS_SYSWRITELEASE_H_
#define INCLUDE_KYTY_SYS_SYSWRITELEASE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/VirtualMemory.h"

#include <map>
#include <vector>

namespace Kyty::Core {

// Write-lease records shared by the virtual-memory backends (see VirtualMemory::WriteLeaseAuthority).
// A record keeps the guest protection token of leased host pages and the authority to notify. A
// backend owns one registry and calls it only with its protection transaction lock held. Addresses
// are host-page aligned.
struct SysWriteLeaseRun
{
	uint64_t                                  address     = 0;
	uint64_t                                  end         = 0;
	uint32_t                                  guest_token = 0;
	const VirtualMemory::WriteLeaseAuthority* authority   = nullptr;
};

struct SysWriteLeaseNative
{
	// Applies a native protection token to [address, address + size).
	bool (*protect)(uint64_t address, uint64_t size, uint32_t token) noexcept = nullptr;
	// `guest_token` without write access; unchanged when it grants none.
	uint32_t (*remove_write)(uint32_t guest_token) noexcept = nullptr;
};

// One span of a protection change with the protection it replaces. A span without a recorded
// protection is host memory the backend does not track: it is applied after every recorded span
// and cannot be rolled back.
struct SysProtectionSpan
{
	uint64_t address   = 0;
	uint64_t end       = 0;
	uint32_t old_token = 0;
	bool     recorded  = true;
};

class SysWriteLeases final
{
public:
	void Add(uint64_t address, uint64_t end, uint32_t guest_token, const VirtualMemory::WriteLeaseAuthority* authority);
	// The record containing `address`, if any.
	[[nodiscard]] bool Find(uint64_t address, SysWriteLeaseRun* run) const;
	// Records overlapping [address, end), clipped to it, in ascending order.
	void Collect(uint64_t address, uint64_t end, std::vector<SysWriteLeaseRun>* runs) const;

	// Applies `guest_token` to the ascending, contiguous spans as one transaction. Every leased run is
	// fenced first and its authority decides per page whether write stays removed. When any native
	// step fails, every recorded span returns to the protection it had, the authorities are told the
	// change did not commit, and no record changes. A failing rollback is fatal: the native state
	// would no longer match any recorded protection.
	[[nodiscard]] bool Protect(const std::vector<SysProtectionSpan>& spans, uint32_t guest_token, VirtualMemory::Mode mode,
	                           const SysWriteLeaseNative& native);

	// Mapping identity change of [address, end) in two phases: Begin fences every leased run before
	// the host operation; End revokes (committed) or keeps the authorities' tokens, and drops the
	// committed records.
	void BeginUnmap(uint64_t address, uint64_t end, std::vector<SysWriteLeaseRun>* fenced) const;
	void EndUnmap(uint64_t address, uint64_t end, const std::vector<SysWriteLeaseRun>& fenced, bool committed);

	// Ends every lease of `authority` and reapplies the guest protection of its pages. A run whose
	// protection cannot be reapplied stays recorded without an authority (still write-protected, never
	// reported again) and the release fails. No record refers to `authority` afterwards.
	[[nodiscard]] bool Release(const VirtualMemory::WriteLeaseAuthority* authority, const SysWriteLeaseNative& native);

private:
	struct Record
	{
		uint64_t                                  end         = 0;
		uint32_t                                  guest_token = 0;
		const VirtualMemory::WriteLeaseAuthority* authority   = nullptr;
	};

	void Split(uint64_t at);
	void Erase(uint64_t address, uint64_t end);
	void SetToken(uint64_t address, uint64_t end, uint32_t guest_token);
	void Detach(uint64_t address, uint64_t end);

	std::map<uint64_t, Record> m_records;
};

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_SYS_SYSWRITELEASE_H_ */
