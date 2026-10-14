#include "Kyty/Sys/SysWriteLease.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <cinttypes>

namespace Kyty::Core {

namespace {

bool HasCallbacks(const VirtualMemory::WriteLeaseAuthority* authority)
{
	return authority != nullptr && authority->begin_change != nullptr && authority->decide != nullptr && authority->end_change != nullptr;
}

// One native step of a protection change and the token that undoes it.
struct NativeStep
{
	uint64_t address  = 0;
	uint64_t size     = 0;
	uint32_t next     = 0;
	uint32_t previous = 0;
};

struct DecisionState
{
	const SysWriteLeaseNative* native      = nullptr;
	std::vector<NativeStep>*   steps       = nullptr;
	uint32_t                   guest_token = 0;
	uint32_t                   old_token   = 0;
	uint64_t                   next        = 0;
	uint64_t                   end         = 0;
	bool                       valid       = true;
};

// A page that keeps its lease had write removed under the old protection
// as well, so the same decision describes both directions of the step.
bool RecordDecision(void* context, uint64_t address, uint64_t size, bool remove_write) noexcept
{
	auto* state = static_cast<DecisionState*>(context);
	if (size == 0 || address != state->next || size > state->end - address)
	{
		state->valid = false;
		return false;
	}
	const auto& native = *state->native;
	state->steps->push_back({address, size, remove_write ? native.remove_write(state->guest_token) : state->guest_token,
	                         remove_write ? native.remove_write(state->old_token) : state->old_token});
	state->next = address + size;
	return true;
}

void PlanSpan(const SysProtectionSpan& span, const std::vector<SysWriteLeaseRun>& leases, uint32_t guest_token, VirtualMemory::Mode mode,
              const SysWriteLeaseNative& native, std::vector<NativeStep>* steps)
{
	uint64_t cursor = span.address;
	for (const auto& lease: leases)
	{
		const uint64_t begin = std::max(lease.address, span.address);
		const uint64_t end   = std::min(lease.end, span.end);
		if (begin >= end)
		{
			continue;
		}
		if (begin > cursor)
		{
			steps->push_back({cursor, begin - cursor, guest_token, span.old_token});
		}
		if (HasCallbacks(lease.authority))
		{
			DecisionState                         state {&native, steps, guest_token, span.old_token, begin, end, true};
			const VirtualMemory::WriteLeaseChange change {begin, end - begin, VirtualMemory::WriteLeaseChangeKind::Protect, mode, guest_token,
			                                              false};
			lease.authority->decide(lease.authority->context, change, &RecordDecision, &state);
			// The authority owns the per-page decision. A run it did not describe
			// completely would leave pages with a protection nobody chose.
			EXIT_IF(!state.valid || state.next != end);
		} else
		{
			// Nobody owns this lease: the guest protection replaces it, and a
			// rollback returns the old guest protection rather than a removal
			// nobody could undo.
			steps->push_back({begin, end - begin, guest_token, span.old_token});
		}
		cursor = end;
	}
	if (cursor < span.end)
	{
		steps->push_back({cursor, span.end - cursor, guest_token, span.old_token});
	}
}

void RollBack(const std::vector<NativeStep>& steps, size_t count, const SysWriteLeaseNative& native)
{
	while (count != 0)
	{
		count--;
		const auto& step = steps[count];
		if (!native.protect(step.address, step.size, step.previous))
		{
			EXIT("write-lease protection rollback failed: address=0x%016" PRIx64 " size=0x%" PRIx64 "\n", step.address, step.size);
		}
	}
}

} // namespace

void SysWriteLeases::Split(uint64_t at)
{
	auto it = m_records.upper_bound(at);
	if (it == m_records.begin())
	{
		return;
	}
	--it;
	if (it->first >= at || it->second.end <= at)
	{
		return;
	}
	const Record right = it->second;
	it->second.end     = at;
	m_records.emplace(at, right);
}

void SysWriteLeases::Erase(uint64_t address, uint64_t end)
{
	if (address >= end)
	{
		return;
	}
	Split(address);
	Split(end);
	for (auto it = m_records.lower_bound(address); it != m_records.end() && it->first < end;)
	{
		it = m_records.erase(it);
	}
}

void SysWriteLeases::SetToken(uint64_t address, uint64_t end, uint32_t guest_token)
{
	Split(address);
	Split(end);
	for (auto it = m_records.lower_bound(address); it != m_records.end() && it->first < end; ++it)
	{
		it->second.guest_token = guest_token;
	}
}

void SysWriteLeases::Detach(uint64_t address, uint64_t end)
{
	Split(address);
	Split(end);
	for (auto it = m_records.lower_bound(address); it != m_records.end() && it->first < end; ++it)
	{
		it->second.authority = nullptr;
	}
}

void SysWriteLeases::Add(uint64_t address, uint64_t end, uint32_t guest_token, const VirtualMemory::WriteLeaseAuthority* authority)
{
	if (address >= end)
	{
		return;
	}
	Erase(address, end);
	auto same = [](const Record& first, const Record& second)
	{ return first.guest_token == second.guest_token && first.authority == second.authority; };
	auto [inserted, unused] = m_records.emplace(address, Record {end, guest_token, authority});
	(void)unused;
	if (inserted != m_records.begin())
	{
		auto previous = inserted;
		--previous;
		if (previous->second.end == address && same(previous->second, inserted->second))
		{
			previous->second.end = end;
			m_records.erase(inserted);
			inserted = previous;
		}
	}
	auto next = inserted;
	++next;
	if (next != m_records.end() && inserted->second.end == next->first && same(inserted->second, next->second))
	{
		inserted->second.end = next->second.end;
		m_records.erase(next);
	}
}

bool SysWriteLeases::Find(uint64_t address, SysWriteLeaseRun* run) const
{
	auto it = m_records.upper_bound(address);
	if (it == m_records.begin())
	{
		return false;
	}
	--it;
	if (address >= it->second.end)
	{
		return false;
	}
	if (run != nullptr)
	{
		*run = {it->first, it->second.end, it->second.guest_token, it->second.authority};
	}
	return true;
}

void SysWriteLeases::Collect(uint64_t address, uint64_t end, std::vector<SysWriteLeaseRun>* runs) const
{
	EXIT_IF(runs == nullptr);
	auto it = m_records.upper_bound(address);
	if (it != m_records.begin())
	{
		--it;
		if (it->second.end <= address)
		{
			++it;
		}
	}
	for (; it != m_records.end() && it->first < end; ++it)
	{
		const uint64_t begin = std::max(it->first, address);
		const uint64_t limit = std::min(it->second.end, end);
		if (begin < limit)
		{
			runs->push_back({begin, limit, it->second.guest_token, it->second.authority});
		}
	}
}

bool SysWriteLeases::Protect(const std::vector<SysProtectionSpan>& spans, uint32_t guest_token, VirtualMemory::Mode mode,
                             const SysWriteLeaseNative& native)
{
	if (spans.empty())
	{
		return false;
	}
	std::vector<SysWriteLeaseRun> leases;
	Collect(spans.front().address, spans.back().end, &leases);
	for (const auto& lease: leases)
	{
		if (HasCallbacks(lease.authority))
		{
			lease.authority->begin_change(lease.authority->context, lease.address, lease.end - lease.address);
		}
	}

	std::vector<NativeStep> steps;
	for (const auto& span: spans)
	{
		if (span.recorded)
		{
			PlanSpan(span, leases, guest_token, mode, native, &steps);
		}
	}
	bool committed = true;
	for (size_t index = 0; index < steps.size(); index++)
	{
		const auto& step = steps[index];
		if (!native.protect(step.address, step.size, step.next))
		{
			// The failed step may have been applied in part.
			RollBack(steps, index + 1u, native);
			committed = false;
			break;
		}
	}
	// Untracked host memory has no recorded protection to return to, so it
	// changes only after every recorded span did.
	for (size_t index = 0; committed && index < spans.size(); index++)
	{
		const auto& span = spans[index];
		if (!span.recorded && !native.protect(span.address, span.end - span.address, guest_token))
		{
			RollBack(steps, steps.size(), native);
			committed = false;
		}
	}

	for (const auto& lease: leases)
	{
		if (HasCallbacks(lease.authority))
		{
			const VirtualMemory::WriteLeaseChange change {lease.address,
			                                              lease.end - lease.address,
			                                              VirtualMemory::WriteLeaseChangeKind::Protect,
			                                              mode,
			                                              guest_token,
			                                              committed};
			lease.authority->end_change(lease.authority->context, change);
		}
	}
	if (!committed)
	{
		return false;
	}
	for (const auto& lease: leases)
	{
		if (HasCallbacks(lease.authority))
		{
			SetToken(lease.address, lease.end, guest_token);
		} else
		{
			Erase(lease.address, lease.end);
		}
	}
	return true;
}

void SysWriteLeases::BeginUnmap(uint64_t address, uint64_t end, std::vector<SysWriteLeaseRun>* fenced) const
{
	EXIT_IF(fenced == nullptr);
	fenced->clear();
	Collect(address, end, fenced);
	for (const auto& run: *fenced)
	{
		if (HasCallbacks(run.authority))
		{
			run.authority->begin_change(run.authority->context, run.address, run.end - run.address);
		}
	}
}

void SysWriteLeases::EndUnmap(uint64_t address, uint64_t end, const std::vector<SysWriteLeaseRun>& fenced, bool committed)
{
	for (const auto& run: fenced)
	{
		if (HasCallbacks(run.authority))
		{
			const VirtualMemory::WriteLeaseChange change {run.address,  run.end - run.address, VirtualMemory::WriteLeaseChangeKind::Unmap,
			                                              VirtualMemory::Mode::NoAccess, 0, committed};
			run.authority->end_change(run.authority->context, change);
		}
	}
	if (committed)
	{
		Erase(address, end);
	}
}

bool SysWriteLeases::Release(const VirtualMemory::WriteLeaseAuthority* authority, const SysWriteLeaseNative& native)
{
	std::vector<SysWriteLeaseRun> runs;
	for (const auto& [address, record]: m_records)
	{
		if (record.authority == authority)
		{
			runs.push_back({address, record.end, record.guest_token, record.authority});
		}
	}
	bool released = true;
	for (const auto& run: runs)
	{
		const uint64_t size = run.end - run.address;
		if (HasCallbacks(authority))
		{
			authority->begin_change(authority->context, run.address, size);
		}
		const bool restored = native.protect(run.address, size, run.guest_token);
		if (HasCallbacks(authority))
		{
			const VirtualMemory::WriteLeaseChange change {run.address, size, VirtualMemory::WriteLeaseChangeKind::Protect,
			                                              VirtualMemory::Mode::NoAccess, run.guest_token, false};
			authority->end_change(authority->context, change);
		}
		if (restored)
		{
			Erase(run.address, run.end);
		} else
		{
			Detach(run.address, run.end);
			released = false;
		}
	}
	return released;
}

} // namespace Kyty::Core
