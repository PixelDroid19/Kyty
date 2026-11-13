#include "GraphicsRunInternal.h"
#include "GraphicsRunWaitPolicy.h"

#include "Emulator/Config.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] const char* gpu_submission_result_name(GpuSubmissionResult result)
{
	switch (result)
	{
		case GpuSubmissionResult::Success: return "Success";
		case GpuSubmissionResult::InvalidArgument: return "InvalidArgument";
		case GpuSubmissionResult::UnknownSubmission: return "UnknownSubmission";
		case GpuSubmissionResult::InvalidTransition: return "InvalidTransition";
		case GpuSubmissionResult::SubmissionFrozen: return "SubmissionFrozen";
		case GpuSubmissionResult::SlotBusy: return "SlotBusy";
		case GpuSubmissionResult::ProducerNotFound: return "ProducerNotFound";
		case GpuSubmissionResult::ProducerValueMismatch: return "ProducerValueMismatch";
		case GpuSubmissionResult::AlreadyCompleted: return "AlreadyCompleted";
		case GpuSubmissionResult::CompletionActionsPending: return "CompletionActionsPending";
	}
	return "UnknownResult";
}

void require_submission_success(GpuSubmissionResult result, const char* operation, int queue, uint32_t slot)
{
	if (result != GpuSubmissionResult::Success)
	{
		EXIT("GPU submission lifecycle failed: operation=%s queue=%d slot=%" PRIu32 " result=%s\n", operation, queue, slot,
		     gpu_submission_result_name(result));
	}
}

[[nodiscard]] const char* gpu_submission_publication_result_name(GpuSubmissionPublicationResult result)
{
	switch (result)
	{
		case GpuSubmissionPublicationResult::Success: return "Success";
		case GpuSubmissionPublicationResult::InvalidArgument: return "InvalidArgument";
		case GpuSubmissionPublicationResult::UnknownSubmission: return "UnknownSubmission";
		case GpuSubmissionPublicationResult::InvalidTransition: return "InvalidTransition";
		case GpuSubmissionPublicationResult::NotReady: return "NotReady";
	}
	return "UnknownResult";
}

void require_publication_success(GpuSubmissionPublicationResult result, const char* operation, int queue, SubmissionId submission)
{
	if (result != GpuSubmissionPublicationResult::Success)
	{
		EXIT("GPU submission publication failed: operation=%s queue=%d sequence=%" PRIu64 " result=%s\n", operation, queue,
		     submission.sequence, gpu_submission_publication_result_name(result));
	}
}

uint64_t ParseSuspendedWaitTimeoutMs(const char* value)
{
	if (value == nullptr || *value == '\0')
	{
		return 0;
	}
	constexpr uint64_t max_timeout_ms = std::numeric_limits<uint64_t>::max() / 1'000'000ull;
	uint64_t           timeout_ms     = 0;
	for (const char* digit = value; *digit != '\0'; ++digit)
	{
		if (*digit < '0' || *digit > '9')
		{
			return 0;
		}
		const auto next = static_cast<uint64_t>(*digit - '0');
		if (timeout_ms > (max_timeout_ms - next) / 10u)
		{
			return 0;
		}
		timeout_ms = timeout_ms * 10u + next;
	}
	return timeout_ms;
}

uint64_t SuspendedWaitTimeoutMs()
{
	static const uint64_t timeout_ms = ParseSuspendedWaitTimeoutMs(std::getenv("KYTY_WAIT_TIMEOUT_MS"));
	return timeout_ms;
}

uint64_t SuspendedWaitNowNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

namespace {

struct WrittenRange
{
	uint64_t address;
	uint64_t size;
};

bool RangeCovered(const std::vector<WrittenRange>& written, uint64_t address, uint64_t size)
{
	// Address 0 and 1 are encoded guest markers, not memory locations (see WaitRegMem32/WaitRegMem64).
	if (address <= 1u)
	{
		return false;
	}
	for (const auto& range: written)
	{
		if (address >= range.address && address - range.address <= range.size && size <= range.size - (address - range.address))
		{
			return true;
		}
	}
	return false;
}

} // namespace

BatchMemoryWaitScan ScanBatchMemoryWaits(const uint32_t* words, uint32_t num_dw)
{
	BatchMemoryWaitScan scan;
	if (words == nullptr)
	{
		return scan;
	}

	std::vector<WrittenRange> written;
	uint32_t                  previous_header      = 0;
	uint64_t                  previous_release_dst = 0;

	for (uint32_t offset = 0; offset < num_dw;)
	{
		const uint32_t header = words[offset];
		const uint32_t type   = header >> 30u;
		if (type == 0u || type == 1u)
		{
			offset += (header != 0u && num_dw - offset >= 2u) ? 2u : 1u;
			continue;
		}
		if (type != 3u)
		{
			offset += 1u;
			continue;
		}

		uint32_t len = Pm4::Pm4SpecialType3PacketDwords(header);
		if (len == 0u)
		{
			len = KYTY_PM4_LEN(header);
		}
		if (len > num_dw - offset)
		{
			// The decoder steps over such a unit, but whether it is a real packet is unknown, so a wait after it may
			// have a producer the scan cannot see: keep the batch serial.
			scan.unparsed++;
			offset += (num_dw - offset >= 2u) ? 2u : 1u;
			continue;
		}

		const uint32_t  op   = (header >> 8u) & 0xffu;
		const uint32_t  r    = KYTY_PM4_R(header);
		const uint32_t* body = words + offset + 1u;
		const uint32_t  body_dw = len - 1u;
		const auto      address64 = [&](uint32_t lo, uint32_t hi) { return static_cast<uint64_t>(body[lo]) | (static_cast<uint64_t>(body[hi]) << 32u); };
		const auto      address48 = [&](uint32_t lo, uint32_t hi) { return static_cast<uint64_t>(body[lo]) | (static_cast<uint64_t>(body[hi] & 0xffffu) << 32u); };

		// Packets that store to guest memory, by the encodings the packet parsers decode.
		uint64_t release_dst = 0;
		if ((op == Pm4::IT_WRITE_DATA || (op == Pm4::IT_NOP && r == Pm4::R_WRITE_DATA)) && body_dw >= 4u)
		{
			written.push_back({address64(1, 2), 4ull * (body_dw - 3u)});
		}
		else if (header == 0xC0054902u && body_dw >= 6u) // IT_RELEASE_MEM, data size in the top bits of body[1]
		{
			written.push_back({address64(2, 3), (body[1] >> 29u) == 1u ? 4u : 8u});
		}
		else if ((header == 0xC0051060u || header == 0xC0061060u) && body_dw >= 6u) // IT_NOP + R_RELEASE_MEM
		{
			// Selector 0 stores nothing; 1 stores 32 bits, anything else 64 bits.
			const uint32_t data_select = (body[1] >> 16u) & 0xffu;
			release_dst                = address64(2, 3);
			if (data_select != 0u)
			{
				written.push_back({release_dst, data_select == 1u ? 4u : 8u});
			}
		}
		else if (header == 0xC0044700u && body_dw >= 5u) // IT_EVENT_WRITE_EOP
		{
			written.push_back({address48(1, 2), 8u});
		}
		else if (header == 0xC0034802u && body_dw >= 4u) // IT_EVENT_WRITE_EOS
		{
			written.push_back({address48(1, 2), 4u});
		}
		else if (header == 0xC0061064u && body_dw >= 7u) // IT_NOP + R_DMA_DATA, byte count in body[2]
		{
			written.push_back({address64(3, 4), body[2]});
		}
		else if (header == 0xC0038300u && body_dw >= 4u) // IT_DUMP_CONST_RAM, dword count in body[1]
		{
			written.push_back({address64(2, 3), 4ull * body[1]});
		}
		else if (op == Pm4::IT_NOP && r == Pm4::R_ZERO && body_dw >= 4u && (body[0] & 0xffff0000u) == 0x68750000u &&
		         ((body[0] & 0xffffu) == 0x778u || (body[0] & 0xffffu) == 0x781u)) // Flip / FlipWithInterrupt markers store a 32-bit value
		{
			written.push_back({address64(1, 2), 4u});
		}
		else if (op == Pm4::IT_INDIRECT_BUFFER || op == Pm4::IT_INDIRECT_BUFFER_CNST)
		{
			scan.nested++;
		}

		// Memory waits: IT_WAIT_REG_MEM and the custom R_WAIT_MEM_32 / R_WAIT_MEM_64 NOPs.
		uint64_t wait_address = 0;
		uint64_t wait_size    = 4u;
		bool     is_wait      = false;
		if (header == 0xC0053C00u)
		{
			is_wait      = true;
			wait_address = address64(1, 2);
		}
		else if (header == 0xC0051028u || header == 0xC00C1028u)
		{
			is_wait      = true;
			wait_address = address64(0, 1);
		}
		else if (header == 0xC0071058u)
		{
			is_wait      = true;
			wait_size    = 8u;
			wait_address = address64(0, 1);
			// A null address stands for the destination of the release right before it.
			if (wait_address == 0u && (previous_header == 0xC0051060u || previous_header == 0xC0061060u))
			{
				wait_address = previous_release_dst;
			}
		}
		if (is_wait)
		{
			scan.waits++;
			if (RangeCovered(written, wait_address, wait_size))
			{
				scan.own++;
			}
		}

		previous_header      = header;
		previous_release_dst = release_dst;
		if (op == Pm4::IT_INDIRECT_BUFFER_END)
		{
			break;
		}
		offset += len;
	}
	return scan;
}

bool BatchProducesItsOwnWaits(const BatchMemoryWaitScan& draw, const BatchMemoryWaitScan& constant)
{
	const auto self_contained = [](const BatchMemoryWaitScan& scan) { return scan.nested == 0u && scan.unparsed == 0u && scan.own == scan.waits; };
	return self_contained(draw) && self_contained(constant);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
