#include "Kyty/Core/MemoryAlloc.h"

#include "Kyty/Core/ArrayWrapper.h" // IWYU pragma: keep
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h" // IWYU pragma: keep
#include "Kyty/Core/Debug.h"    // IWYU pragma: keep
#include "Kyty/Core/Hashmap.h"  // IWYU pragma: keep
#include "Kyty/Sys/SysHeap.h"
#include "Kyty/Sys/SysSync.h"

#include <cstdlib>
#include <new>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <pthread.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace Kyty::Core {

#if !defined(KYTY_FINAL) && !defined(KYTY_SHARED_DLL)
#define MEM_TRACKER
#endif

#define MEM_ALLOC_ALIGNED

#ifdef MEM_ALLOC_ALIGNED
#if KYTY_PLATFORM == KYTY_PLATFORM_ANDROID
constexpr int MEM_ALLOC_ALIGN = 8;
#else
constexpr int MEM_ALLOC_ALIGN = 16;
#endif
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_BITNESS == 64
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 5;
#elif KYTY_PLATFORM == KYTY_PLATFORM_ANDROID
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 4;
#else
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 2;
#endif

static SysCS*        g_mem_cs          = nullptr;
static bool          g_mem_initialized = false;
static sys_heap_id_t g_default_heap    = nullptr;
static size_t        g_mem_max_size    = 0;
static MemoryAllocDetail::ThreadDomainRegistry g_guest_thread_domains;

static uint64_t mem_current_thread_token()
{
#ifdef __APPLE__
	uint64_t token = 0;
	EXIT_IF(pthread_threadid_np(nullptr, &token) != 0);
	return token;
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint64_t>(GetCurrentThreadId());
#else
	return static_cast<uint64_t>(::syscall(SYS_gettid));
#endif
}

#ifdef MEM_TRACKER

using pattern_t = uint32_t;

constexpr size_t PATTERN_SIZE = (sizeof(pattern_t));
constexpr size_t PATTERNS_NUM = 4;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
struct MemBlockInfoT
{
	uintptr_t  addr;
	size_t     size;
	int        state;
	DebugStack stack;
	pattern_t  left_pattern;
	pattern_t  right_pattern;
};

#ifdef __APPLE__
// Guest execution can replace the TLS segment expected by native C++ code.
// pthread_threadid_np returns the complete kernel thread token without compiler
// TLS, while RecursionState avoids both hashed slots and tracker self-recursion.
static MemoryAllocDetail::RecursionState g_mem_recursion_state;
static bool                              g_mem_tracker_enabled = true;
#else
thread_local int  g_mem_depth           = 0;
thread_local bool g_mem_tracker_enabled = true;
#endif

static Hashmap<uintptr_t, MemBlockInfoT*>* g_mem_map         = nullptr;
static int                                 g_mem_state       = 0;
static size_t                              g_total_allocated = 0;

#define KYTY_MDBG(str, ptr)                                                                                                                \
	{                                                                                                                                      \
		if (false)                                                                                                                         \
		{                                                                                                                                  \
			printf("%s, %016" PRIx64 "\n", str, reinterpret_cast<uint64_t>(ptr));                                                          \
		}                                                                                                                                  \
	}

#endif

class MemLock
{
public:
	MemLock()
	{
		g_mem_cs->Enter();

#ifdef MEM_TRACKER
	#ifdef __APPLE__
		m_thread_token = mem_current_thread_token();
		g_mem_recursion_state.Enter(m_thread_token);
	#else
		g_mem_depth++;
	#endif
#endif
	}

	~MemLock()
	{
#ifdef MEM_TRACKER
	#ifdef __APPLE__
		g_mem_recursion_state.Leave(m_thread_token);
	#else
		g_mem_depth--;
	#endif
#endif

		g_mem_cs->Leave();
	}

	KYTY_CLASS_NO_COPY(MemLock);

#ifdef MEM_TRACKER
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] bool IsRecursive() const
	{
	#ifdef __APPLE__
		return g_mem_recursion_state.Depth() > 1;
	#else
		return g_mem_depth > 1;
	#endif
	}
#endif

private:
#if defined(MEM_TRACKER) && defined(__APPLE__)
	uint64_t m_thread_token = 0;
#endif
};

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#pragma code_seg(push)
#endif

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#pragma code_seg(".mem_a")
#endif

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#pragma code_seg(".mem_b")
#endif

#ifdef MEM_TRACKER

static const Array<pattern_t, 4> g_mem_patterns = {0xAAAAAAAA, 0xCCCCCCCC, 0x55555555, 0x33333333};

static int g_pattern_id = 0;

static pattern_t pattern_next()
{
	return g_mem_patterns[g_pattern_id++ % g_mem_patterns.Size()];
}

// The overflow guard follows the block. The user pointer is the allocation base
// (no leading guard), so any code path can free it with the plain system
// allocator regardless of which domain allocated it.
static void pattern_write(MemBlockInfoT* info)
{
	auto* ptr = reinterpret_cast<uint8_t*>(info->addr);
	for (int i = 0; i < 4; i++)
	{
		(reinterpret_cast<pattern_t*>(ptr + info->size))[i] = info->right_pattern;
	}
}

static bool pattern_check(MemBlockInfoT* info)
{
	auto* ptr = reinterpret_cast<uint8_t*>(info->addr);

	for (int i = 0; i < 4; i++)
	{
		if ((reinterpret_cast<pattern_t*>(ptr + info->size))[i] != info->right_pattern)
		{
			return false;
		}
	}
	return true;
}

#endif

static void mem_init()
{
	if (g_mem_initialized)
	{
		return;
	}

	g_mem_initialized = true;

#ifdef MEM_TRACKER
	srand(DateTime::FromSystemUTC().GetTime().MsecTotal());
	g_pattern_id = static_cast<int>(rand() % g_mem_patterns.Size()); // NOLINT(cert-msc30-c,cert-msc50-cpp)
#endif

	// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
	g_mem_cs = new (std::malloc(sizeof(SysCS))) SysCS;
	g_mem_cs->Init();
#ifdef MEM_TRACKER
	#ifdef __APPLE__
	const auto init_thread_token = mem_current_thread_token();
	g_mem_recursion_state.Enter(init_thread_token);
	#else
	g_mem_depth++;
	#endif
	g_mem_map = new Hashmap<uintptr_t, MemBlockInfoT*>;
#endif

	g_default_heap = sys_heap_create();

#ifdef MEM_TRACKER
	#ifdef __APPLE__
	g_mem_recursion_state.Leave(init_thread_token);
	#else
	g_mem_depth--;
	#endif
#endif
}

void core_memory_init()
{
	mem_init();
}

void mem_guest_thread_enter()
{
	EXIT_IF(!g_guest_thread_domains.Add(mem_current_thread_token()));
}

void mem_guest_thread_leave()
{
	EXIT_IF(!g_guest_thread_domains.Remove(mem_current_thread_token()));
}

bool mem_guest_thread_is_active()
{
	return g_guest_thread_domains.Contains(mem_current_thread_token());
}

void* mem_alloc_check_alignment(void* ptr)
{
#ifdef MEM_ALLOC_ALIGNED
	if ((reinterpret_cast<uintptr_t>(ptr) & static_cast<uintptr_t>(MEM_ALLOC_ALIGN - 1)) != 0u)
	{
		EXIT("mem alloc not aligned!\n");
	}
#endif

	return ptr;
}

void* mem_alloc(size_t size)
{
	if (size == 0)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		size = 1;
#else
		EXIT("size == 0\n");
#endif
	}

	if ((g_mem_max_size != 0u) && size > g_mem_max_size)
	{
		EXIT("mem_alloc(): size(%" PRIu64 ") > max(%" PRIu64 ")\n", uint64_t(size), uint64_t(g_mem_max_size));
	}

	mem_init();
	if (mem_guest_thread_is_active())
	{
		// Guest execution may replace the native TLS segment or leave through a
		// guest control transfer while native code is mid-call. Keep its HLE
		// allocations out of the host-only stack-tracing tracker and never take the
		// tracker lock (a guest thread could exit while holding it). The system
		// allocator is thread-safe on every supported platform.
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		return mem_alloc_check_alignment(std::malloc(size));
	}
#if defined(MEM_TRACKER) && defined(__APPLE__)
	if (g_mem_recursion_state.IsOwnedBy(mem_current_thread_token()))
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		return mem_alloc_check_alignment(std::malloc(size));
	}
#endif
	MemLock lock;

#ifdef MEM_TRACKER
	DebugStack stack;
	DebugStack::Trace(&stack);

	if (lock.IsRecursive() || !g_mem_tracker_enabled)
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		void* r = std::malloc(size);
		KYTY_MDBG("- std alloc -", r);
		return mem_alloc_check_alignment(r);
	}

#endif

#ifdef MEM_TRACKER
	void* ptr = sys_heap_alloc(g_default_heap, size + PATTERN_SIZE * PATTERNS_NUM);
#else
	void* ptr  = sys_heap_alloc(g_default_heap, size);
#endif

	if (ptr == nullptr)
	{
		EXIT("mem_alloc(): can't alloc %" PRIu64 " bytes\n", uint64_t(size));
	}

#ifdef MEM_TRACKER

	// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
	auto* info = static_cast<MemBlockInfoT*>(std::malloc(sizeof(MemBlockInfoT)));

	info->addr  = reinterpret_cast<uintptr_t>(ptr);
	info->size  = size;
	info->state = g_mem_state;
	stack.CopyTo(&info->stack);

	info->left_pattern  = pattern_next();
	info->right_pattern = pattern_next();

	pattern_write(info);

	g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr), info);

	g_total_allocated += size;

	KYTY_MDBG("- mem_alloc -", ptr);

#endif

	return mem_alloc_check_alignment(ptr);
}

void* mem_realloc(void* ptr, size_t size)
{
	EXIT_IF(size == 0);

	if ((g_mem_max_size != 0u) && size > g_mem_max_size)
	{
		EXIT("mem_realloc(): size(%" PRIu64 ") > max(%" PRIu64 ")\n", uint64_t(size), uint64_t(g_mem_max_size));
	}

	mem_init();
	if (mem_guest_thread_is_active())
	{
		// See mem_alloc: guest domain reallocations must not enter the tracker or
		// take its lock. A block allocated in the same domain came from the system
		// allocator, so std::realloc is the matching, thread-safe operation.
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		return mem_alloc_check_alignment(std::realloc(ptr, size));
	}
#if defined(MEM_TRACKER) && defined(__APPLE__)
	if (g_mem_recursion_state.IsOwnedBy(mem_current_thread_token()))
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		return mem_alloc_check_alignment(std::realloc(ptr, size));
	}
#endif
	MemLock lock;

#ifdef MEM_TRACKER
	DebugStack stack;
	DebugStack::Trace(&stack);

	if (lock.IsRecursive() || !g_mem_tracker_enabled)
	{

		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		void* ptr2 = std::realloc(ptr, size);
		KYTY_MDBG("- std realloc old -", ptr);
		KYTY_MDBG("- std realloc new -", ptr2);
		return mem_alloc_check_alignment(ptr2);
	}

	if (ptr != nullptr && g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr)) == nullptr)
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		return mem_alloc_check_alignment(std::realloc(ptr, size));
	}
#endif

#ifdef MEM_TRACKER
	void* ptr2 = sys_heap_realloc(g_default_heap, ptr, size + PATTERN_SIZE * PATTERNS_NUM);
#else
	void* ptr2 = sys_heap_realloc(g_default_heap, ptr, size);
#endif

	if (ptr2 == nullptr)
	{
		EXIT("mem_realloc(): can't alloc %" PRIu64 " bytes\n", uint64_t(size));
	}

#ifdef MEM_TRACKER
	if (ptr != nullptr)
	{
		MemBlockInfoT* const* info_p = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));

		// EXIT_IF(info == 0);
		if (info_p == nullptr)
		{
			printf("error %016" PRIx64 "\n", reinterpret_cast<uint64_t>(ptr));
			EXIT_IF(info_p == nullptr);
		}

		MemBlockInfoT* info = *info_p;

		g_total_allocated -= info->size;
		g_total_allocated += size;

		if (ptr == ptr2)
		{
			info->size  = size;
			info->state = g_mem_state;
			stack.CopyTo(&info->stack);
		} else
		{
			info->addr  = reinterpret_cast<uintptr_t>(ptr2);
			info->size  = size;
			info->state = g_mem_state;
			stack.CopyTo(&info->stack);
			g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr2), info);
			g_mem_map->Remove(reinterpret_cast<uintptr_t>(ptr));
		}

		pattern_write(info);
	} else
	{

		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		auto* info  = static_cast<MemBlockInfoT*>(std::malloc(sizeof(MemBlockInfoT)));
		info->addr  = reinterpret_cast<uintptr_t>(ptr2);
		info->size  = size;
		info->state = g_mem_state;
		stack.CopyTo(&info->stack);

		info->left_pattern  = pattern_next();
		info->right_pattern = pattern_next();

		pattern_write(info);

		g_total_allocated += size;

		g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr2), info);
	}

	KYTY_MDBG("- mem_realloc old -", ptr);
	KYTY_MDBG("- mem_realloc new -", ptr2);
#endif

	// g_mem_cs->Leave();

	return mem_alloc_check_alignment(ptr2);
}

void mem_free(void* ptr)
{

	EXIT_IF(!g_mem_initialized);

	if (mem_guest_thread_is_active())
	{
		// See mem_alloc: guest domain frees must not enter the tracker or take its
		// lock. Blocks reaching mem_free on a guest thread were allocated in the
		// same domain from the system allocator, so std::free is the matching,
		// thread-safe operation and cannot orphan the tracker mutex.
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		std::free(ptr);
		return;
	}
#if defined(MEM_TRACKER) && defined(__APPLE__)
	if (g_mem_recursion_state.IsOwnedBy(mem_current_thread_token()))
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		std::free(ptr);
		return;
	}
#endif
	MemLock lock;

#ifdef MEM_TRACKER
	MemBlockInfoT* const* info = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));

	if (info != nullptr)
	{

		if (!pattern_check(*info))
		{
			EXIT("memory overflow\n");
		}

		g_total_allocated -= (*info)->size;
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		std::free(*info);
		g_mem_map->Remove(reinterpret_cast<uintptr_t>(ptr));
		// printf("heap_free: %x\n", uintptr_t(ptr));
#endif

#ifdef MEM_TRACKER
		sys_heap_free(g_default_heap, ptr);
#else
	sys_heap_free(g_default_heap, ptr);
#endif

#ifdef MEM_TRACKER
	} else
	{
		if (ptr != nullptr)
		{
			// printf("free: %x\n", uintptr_t(ptr));
			// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
			std::free(ptr);
		}
	}

	KYTY_MDBG("- mem_free -", ptr);
#endif
}

bool mem_check([[maybe_unused]] const void* ptr)
{
#ifdef MEM_TRACKER
	EXIT_IF(!g_mem_initialized);

	MemLock lock;

	MemBlockInfoT* const* info = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));

	return info != nullptr && pattern_check(*info);
#else
	return true;
#endif
}

#ifdef MEM_TRACKER
static bool KYTY_HASH_CALL mem_map_print_callback(const uintptr_t* /*key*/, MemBlockInfoT* const* value, void* arg)
{
	int state = static_cast<int>(reinterpret_cast<intptr_t>(arg));

	if (state <= (*value)->state)
	{
		if (sizeof(uintptr_t) == 4)
		{
			printf("\n%08" PRIx32 ", %" PRIu32 ", %d\n", static_cast<uint32_t>((*value)->addr), static_cast<uint32_t>((*value)->size),
			       (*value)->state);
			(*value)->stack.Print(STACK_CHECK_FROM);
		} else
		{
			printf("\n%016" PRIx64 ", %" PRIu64 ", %d\n", static_cast<uint64_t>((*value)->addr), static_cast<uint64_t>((*value)->size),
			       (*value)->state);
			(*value)->stack.Print(STACK_CHECK_FROM);
		}
	}
	return true;
}

static bool KYTY_HASH_CALL mem_map_stat_callback(const uintptr_t* /*key*/, MemBlockInfoT* const* value, void* arg)
{
	auto* s = static_cast<MemStats*>(arg);

	if (s->state <= (*value)->state)
	{
		s->total_allocated += (*value)->size;
		s->blocks_num++;
	}
	return true;
}
#endif

void mem_get_stat(MemStats* s)
{
#ifdef MEM_TRACKER
	if (!g_mem_initialized)
	{
#endif
		s->total_allocated = 0;
		s->blocks_num      = 0;
#ifdef MEM_TRACKER
		return;
	}

	MemLock lock;

	if (s->state == 0)
	{
		s->total_allocated = g_total_allocated;
		s->blocks_num      = g_mem_map->Size();
	} else
	{
		s->total_allocated = 0;
		s->blocks_num      = 0;

		g_mem_map->ForEach(mem_map_stat_callback, static_cast<void*>(s));
	}

#endif
}

int mem_new_state()
{
#ifdef MEM_TRACKER
	if (!g_mem_initialized)
	{
#endif
		return 0;
#ifdef MEM_TRACKER
	}

	MemLock lock;

	g_mem_state++;

	return g_mem_state;
#endif
}

void mem_print(int from_state)
{
#ifdef MEM_TRACKER
	if (!g_mem_initialized)
	{
		return;
	}

	MemLock lock;

	intptr_t s = from_state;

	g_mem_map->ForEach(mem_map_print_callback, reinterpret_cast<void*>(s));

#endif
}

} // namespace Kyty::Core

#ifndef KYTY_SHARED_DLL
void* operator new(size_t size)
{
	return Kyty::Core::mem_alloc(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*nothrow_value*/) noexcept
{
	return Kyty::Core::mem_alloc(size);
}

void* operator new[](size_t size)
{
	return Kyty::Core::mem_alloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*nothrow_value*/) noexcept
{
	return Kyty::Core::mem_alloc(size);
}

void operator delete(void* block) noexcept
{
	Kyty::Core::mem_free(block);
}

void operator delete[](void* block) noexcept
{
	Kyty::Core::mem_free(block);
}
#else
//#error "haha"
#endif

extern "C" {
void* mem_alloc_c(size_t size)
{
	return Kyty::Core::mem_alloc(size);
}

void* mem_realloc_c(void* ptr, size_t size)
{
	return Kyty::Core::mem_realloc(ptr, size);
}

void mem_free_c(void* ptr)
{
	Kyty::Core::mem_free(ptr);
}
}

namespace Kyty::Core {

bool mem_tracker_enabled()
{
#ifdef MEM_TRACKER
	return g_mem_tracker_enabled;
#else
	return false;
#endif
}

void mem_tracker_enable()
{
#ifdef MEM_TRACKER
	g_mem_tracker_enabled = true;
#endif
}

void mem_tracker_disable()
{
#ifdef MEM_TRACKER
	g_mem_tracker_enabled = false;
#endif
}

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#pragma code_seg(".mem_c")
#endif

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#pragma code_seg(pop)
#endif

void mem_set_max_size(size_t size)
{
	g_mem_max_size = size;
}

} // namespace Kyty::Core
