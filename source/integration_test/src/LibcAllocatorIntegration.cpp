#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace Kyty;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "libc-allocator integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

const Loader::SymbolRecord* FindLibcFunction(Loader::SymbolDatabase& symbols, const char* nid)
{
	Loader::SymbolResolve resolve {};
	resolve.name                 = nid;
	resolve.library              = U"libc";
	resolve.library_version      = 1;
	resolve.module               = U"libc";
	resolve.module_version_major = 1;
	resolve.module_version_minor = 1;
	resolve.type                 = Loader::SymbolType::Func;
	return symbols.Find(resolve);
}

uint64_t Resolve(Loader::SymbolDatabase& symbols, const char* nid)
{
	const auto* record = FindLibcFunction(symbols, nid);
	Expect(record != nullptr, "allocator import must resolve through the libc HLE registry");
	return record->vaddr;
}

bool ExerciseAlignedAllocation(uint64_t memalign_fn, uint64_t realloc_fn, uint64_t free_fn, uint8_t fill)
{
	const auto aligned = Loader::GuestCall::Invoke(memalign_fn, 64, 0x80, 0);
	if (aligned == 0 || (aligned & 63) != 0)
	{
		return false;
	}

	std::memset(reinterpret_cast<void*>(aligned), fill, 0x80);
	const auto resized = Loader::GuestCall::Invoke(realloc_fn, aligned, 0x400, 0);
	if (resized == 0)
	{
		Loader::GuestCall::Invoke(free_fn, aligned, 0, 0);
		return false;
	}

	for (size_t i = 0; i < 0x80; i++)
	{
		if (reinterpret_cast<const uint8_t*>(resized)[i] != fill)
		{
			Loader::GuestCall::Invoke(free_fn, resized, 0, 0);
			return false;
		}
	}

	Loader::GuestCall::Invoke(free_fn, resized, 0, 0);
	return true;
}

} // namespace

int main()
{
	Loader::SymbolDatabase symbols;
	Expect(Libs::Init(U"libc_1", &symbols), "libc HLE registration must succeed");

	const uint64_t malloc_fn   = Resolve(symbols, "gQX+4GDQjpM");
	const uint64_t calloc_fn   = Resolve(symbols, "2X5agFjKxMc");
	const uint64_t memalign_fn = Resolve(symbols, "Ujf3KzMvRmI");
	const uint64_t realloc_fn  = Resolve(symbols, "Y7aJ1uydPMo");
	const uint64_t free_fn     = Resolve(symbols, "tIhsqj0qsFE");

	const auto raw = Loader::GuestCall::Invoke(malloc_fn, 0x100000, 0, 0);
	Expect(raw != 0, "malloc must return storage");
	Loader::GuestCall::Invoke(free_fn, raw, 0, 0);

	const auto zeroed = Loader::GuestCall::Invoke(calloc_fn, 16, 64, 0);
	Expect(zeroed != 0, "calloc must return storage");
	for (size_t i = 0; i < 16 * 64; i++)
	{
		Expect(reinterpret_cast<const uint8_t*>(zeroed)[i] == 0, "calloc must zero its raw allocation");
	}
	Loader::GuestCall::Invoke(free_fn, zeroed, 0, 0);

	const auto resized_raw = Loader::GuestCall::Invoke(malloc_fn, 0x40, 0, 0);
	Expect(resized_raw != 0, "second malloc must return storage");
	static_cast<uint8_t*>(reinterpret_cast<void*>(resized_raw))[0] = 0x5a;
	const auto resized                                             = Loader::GuestCall::Invoke(realloc_fn, resized_raw, 0x800, 0);
	Expect(resized != 0, "realloc must preserve the raw malloc contract");
	Expect(reinterpret_cast<const uint8_t*>(resized)[0] == 0x5a, "raw realloc must preserve existing bytes");
	Loader::GuestCall::Invoke(free_fn, resized, 0, 0);

	const auto aligned = Loader::GuestCall::Invoke(memalign_fn, 64, 0x80, 0);
	Expect(aligned != 0 && (aligned & 63) == 0, "memalign must return the requested alignment");
	std::memset(reinterpret_cast<void*>(aligned), 0xa5, 0x80);
	const auto resized_aligned = Loader::GuestCall::Invoke(realloc_fn, aligned, 0x400, 0);
	Expect(resized_aligned != 0, "realloc must preserve aligned-allocation provenance");
	for (size_t i = 0; i < 0x80; i++)
	{
		Expect(reinterpret_cast<const uint8_t*>(resized_aligned)[i] == 0xa5, "aligned realloc must preserve existing bytes");
	}
	Loader::GuestCall::Invoke(free_fn, resized_aligned, 0, 0);

	const auto preserved = Loader::GuestCall::Invoke(memalign_fn, 64, 0x80, 0);
	Expect(preserved != 0, "memalign must provide storage for realloc failure coverage");
	std::memset(reinterpret_cast<void*>(preserved), 0x3c, 0x80);
	const auto failed_realloc = Loader::GuestCall::Invoke(realloc_fn, preserved, std::numeric_limits<size_t>::max(), 0);
	Expect(failed_realloc == 0, "oversized aligned realloc must fail");
	for (size_t i = 0; i < 0x80; i++)
	{
		Expect(reinterpret_cast<const uint8_t*>(preserved)[i] == 0x3c, "failed aligned realloc must preserve the original allocation");
	}
	Loader::GuestCall::Invoke(free_fn, preserved, 0, 0);

	constexpr size_t         thread_count  = 8;
	constexpr size_t         iterations    = 128;
	std::atomic_size_t       ready_threads = 0;
	std::atomic_bool         start         = false;
	std::atomic_bool         concurrent_ok = true;
	std::vector<std::thread> workers;
	workers.reserve(thread_count);

	for (size_t thread_index = 0; thread_index < thread_count; thread_index++)
	{
		workers.emplace_back(
		    [&, thread_index]
		    {
			    ready_threads.fetch_add(1, std::memory_order_release);
			    while (!start.load(std::memory_order_acquire))
			    {
				    std::this_thread::yield();
			    }

			    for (size_t iteration = 0; iteration < iterations; iteration++)
			    {
				    const auto fill = static_cast<uint8_t>((thread_index + iteration) | 1);
				    if (!ExerciseAlignedAllocation(memalign_fn, realloc_fn, free_fn, fill))
				    {
					    concurrent_ok.store(false, std::memory_order_relaxed);
					    return;
				    }
			    }
		    });
	}

	while (ready_threads.load(std::memory_order_acquire) != thread_count)
	{
		std::this_thread::yield();
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers)
	{
		worker.join();
	}
	Expect(concurrent_ok.load(std::memory_order_relaxed), "independent aligned ownership transitions must remain safe under concurrency");

	return 0;
}
