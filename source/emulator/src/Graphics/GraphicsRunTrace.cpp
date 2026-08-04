#include "GraphicsRunTrace.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Kyty::Libs::Graphics {

void GraphicsRunTraceAaRegisterWrite(const char* path, const char* name, uint32_t value)
{
	if (std::getenv("KYTY_DUMP_AA_REGS") == nullptr)
	{
		return;
	}
	static std::atomic_uint64_t sequence {0};
	const auto                  current = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	std::fprintf(stderr, "KYTY_AA_REG seq=%" PRIu64 " path=%s reg=%s value=0x%08" PRIx32 "\n", current, path, name, value);
}

void GraphicsRunTraceWait(const char* stage, int queue, uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                          uint64_t sequence, uint64_t elapsed_ns)
{
	static const bool enabled = (std::getenv("KYTY_WAIT_TRACE") != nullptr);
	if (!enabled)
	{
		return;
	}
	const bool important_begin = std::strcmp(stage, "wait32_suspended") == 0 || std::strcmp(stage, "wait64_suspended") == 0 ||
	                             std::strcmp(stage, "wait_timeout") == 0;
	const bool important_end = elapsed_ns >= 5'000'000u;
	if (!important_begin && !important_end)
	{
		return;
	}
	static std::atomic_uint32_t count {0};
	if (count.fetch_add(1, std::memory_order_relaxed) >= 8192u)
	{
		return;
	}
	std::fprintf(stderr,
	             "KYTY_WAIT_TRACE stage=%s queue=%d addr=0x%016" PRIx64 " value=0x%016" PRIx64 " ref=0x%016" PRIx64
	             " mask=0x%016" PRIx64 " submission=%" PRIu64 " elapsed_ns=%" PRIu64 "\n",
	             stage, queue, address, value, reference, mask, sequence, elapsed_ns);
}

} // namespace Kyty::Libs::Graphics
