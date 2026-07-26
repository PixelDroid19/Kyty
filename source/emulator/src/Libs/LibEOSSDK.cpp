#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/Threads.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <unordered_map>

extern "C" {
#include "miniz.h"
}

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("EOSSDK-PS5-Shipping", 1, "EOSSDK-PS5-Shipping", 1, 1);

namespace EOSSDKPS5Shipping {

struct GuestZlibStream72
{
	const void* next_in      = nullptr;
	uint32_t    avail_in     = 0;
	uint32_t    avail_in_pad = 0;
	mz_ulong    total_in     = 0;
	void*       next_out     = nullptr;
	uint32_t    avail_out    = 0;
	uint32_t    avail_out_pad = 0;
	mz_ulong    total_out    = 0;
	char*       msg          = nullptr;
	mz_internal_state* state  = nullptr;
	mz_alloc_func       zalloc = nullptr;
};

static_assert(sizeof(GuestZlibStream72) == 72, "GuestZlibStream72 expected size 0x48");

struct GuestZlibStream112
{
	const void*        next_in;
	uint32_t           avail_in;
	uint32_t           avail_in_pad;
	uint64_t           total_in;
	void*              next_out;
	uint32_t           avail_out;
	uint32_t           avail_out_pad;
	uint64_t           total_out;
	char*              msg;
	mz_internal_state* state;
	mz_alloc_func      zalloc;
	mz_free_func       zfree;
	void*              opaque;
	int32_t            data_type;
	uint32_t           data_type_pad;
	uint64_t           adler;
	uint64_t           reserved;
};

static_assert(sizeof(GuestZlibStream112) == 112, "GuestZlibStream112 expected size 0x70");

struct StreamLayoutInfo
{
	int size = 0;
};

static Core::Mutex g_eos_stream_mutex;
static std::unordered_map<mz_streamp, StreamLayoutInfo> g_eos_stream_layouts;

static bool IsCompatLegacyStream72(int stream_size)
{
	return stream_size == static_cast<int>(sizeof(GuestZlibStream72));
}

static bool IsCompatStream112(int stream_size)
{
	return stream_size == static_cast<int>(sizeof(GuestZlibStream112));
}

static void* HostAlloc(void* /*opaque*/, size_t items, size_t size)
{
	if (items != 0 && size > SIZE_MAX / items)
	{
		return nullptr;
	}
	return Core::mem_alloc(items * size);
}

static void HostFree(void* /*opaque*/, void* address)
{
	Core::mem_free(address);
}

static void RegisterLegacyStreamLayout(mz_streamp strm, int stream_size)
{
	if (strm == nullptr)
	{
		return;
	}

	Core::LockGuard lock(g_eos_stream_mutex);
	StreamLayoutInfo info {};
	info.size = stream_size;
	g_eos_stream_layouts[strm] = info;
}

static void ForgetLegacyStreamLayout(mz_streamp strm)
{
	if (strm == nullptr)
	{
		return;
	}

	Core::LockGuard lock(g_eos_stream_mutex);
	g_eos_stream_layouts.erase(strm);
}

static bool GetLegacyStreamLayout(mz_streamp strm, StreamLayoutInfo* out)
{
	Core::LockGuard lock(g_eos_stream_mutex);
	auto it = g_eos_stream_layouts.find(strm);
	if (it == g_eos_stream_layouts.end())
	{
		return false;
	}
	if (out != nullptr)
	{
		*out = it->second;
	}
	return true;
}

static int InflateInitCompatInitImpl(mz_streamp strm, int window_bits, const char* version, int stream_size)
{
	mz_stream host_stream {};
	if (IsCompatLegacyStream72(stream_size))
	{
		GuestZlibStream72 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = guest_stream.total_in;
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = guest_stream.total_out;
		host_stream.msg       = guest_stream.msg;
		// miniz is built with MINIZ_NO_MALLOC in Kyty, so callers must always
		// provide host allocators. Guest callback addresses cannot be invoked
		// directly by the host.
		host_stream.zalloc    = HostAlloc;
		host_stream.zfree     = HostFree;
		host_stream.opaque    = nullptr;
		host_stream.data_type = 0;
		host_stream.adler     = 0;
		host_stream.reserved  = 0;
	}
	else if (IsCompatStream112(stream_size))
	{
		GuestZlibStream112 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = static_cast<mz_ulong>(guest_stream.total_in);
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = static_cast<mz_ulong>(guest_stream.total_out);
		host_stream.msg        = guest_stream.msg;
		host_stream.state      = guest_stream.state;
		host_stream.zalloc     = HostAlloc;
		host_stream.zfree      = HostFree;
		host_stream.opaque     = nullptr;
		host_stream.data_type = guest_stream.data_type;
		host_stream.adler     = static_cast<mz_ulong>(guest_stream.adler);
		host_stream.reserved  = static_cast<mz_ulong>(guest_stream.reserved);
	}
	else
	{
		host_stream = *strm;
	}

	const int rc = inflateInit2(&host_stream, window_bits);

	if (IsCompatLegacyStream72(stream_size))
	{
		GuestZlibStream72 guest_stream {};
		guest_stream.next_in      = host_stream.next_in;
		guest_stream.avail_in     = host_stream.avail_in;
		guest_stream.total_in     = host_stream.total_in;
		guest_stream.next_out     = host_stream.next_out;
		guest_stream.avail_out    = host_stream.avail_out;
		guest_stream.total_out    = host_stream.total_out;
		guest_stream.msg         = host_stream.msg;
		guest_stream.state       = reinterpret_cast<mz_internal_state*>(host_stream.state);
		guest_stream.zalloc      = host_stream.zalloc;
		std::memcpy(strm, &guest_stream, sizeof(guest_stream));
		return rc;
	}
	if (IsCompatStream112(stream_size))
	{
		GuestZlibStream112 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));
		guest_stream.next_in   = host_stream.next_in;
		guest_stream.avail_in  = host_stream.avail_in;
		guest_stream.total_in  = host_stream.total_in;
		guest_stream.next_out  = host_stream.next_out;
		guest_stream.avail_out = host_stream.avail_out;
		guest_stream.total_out = host_stream.total_out;
		guest_stream.msg       = host_stream.msg;
		guest_stream.state     = reinterpret_cast<mz_internal_state*>(host_stream.state);
		guest_stream.data_type = host_stream.data_type;
		guest_stream.adler     = host_stream.adler;
		guest_stream.reserved  = host_stream.reserved;
		std::memcpy(strm, &guest_stream, sizeof(guest_stream));
		return rc;
	}

	*strm = host_stream;
	return rc;
}

static int InflateCompatImpl(mz_streamp strm, int flush)
{
	StreamLayoutInfo layout {};
	if (!GetLegacyStreamLayout(strm, &layout))
	{
		return inflate(strm, flush);
	}

	if (IsCompatLegacyStream72(layout.size))
	{
		GuestZlibStream72 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		mz_stream host_stream {};
		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = guest_stream.total_in;
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = guest_stream.total_out;
		host_stream.msg       = guest_stream.msg;
		host_stream.state     = guest_stream.state;
		host_stream.zalloc    = HostAlloc;
		host_stream.zfree     = HostFree;
		host_stream.opaque    = nullptr;

		const int rc = inflate(&host_stream, flush);

		guest_stream.next_in      = host_stream.next_in;
		guest_stream.avail_in     = host_stream.avail_in;
		guest_stream.total_in     = host_stream.total_in;
		guest_stream.next_out     = host_stream.next_out;
		guest_stream.avail_out    = host_stream.avail_out;
		guest_stream.total_out    = host_stream.total_out;
		guest_stream.msg         = host_stream.msg;
		guest_stream.state       = reinterpret_cast<mz_internal_state*>(host_stream.state);
		guest_stream.zalloc      = host_stream.zalloc;
		std::memcpy(strm, &guest_stream, sizeof(guest_stream));
		return rc;
	}
	if (IsCompatStream112(layout.size))
	{
		GuestZlibStream112 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		mz_stream host_stream {};
		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = static_cast<mz_ulong>(guest_stream.total_in);
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = static_cast<mz_ulong>(guest_stream.total_out);
		host_stream.msg        = guest_stream.msg;
		host_stream.state      = guest_stream.state;
		host_stream.zalloc     = HostAlloc;
		host_stream.zfree      = HostFree;
		host_stream.opaque     = nullptr;
		host_stream.data_type  = guest_stream.data_type;
		host_stream.adler      = static_cast<mz_ulong>(guest_stream.adler);
		host_stream.reserved   = static_cast<mz_ulong>(guest_stream.reserved);

		const int rc = inflate(&host_stream, flush);

		guest_stream.next_in   = host_stream.next_in;
		guest_stream.avail_in  = host_stream.avail_in;
		guest_stream.total_in  = host_stream.total_in;
		guest_stream.next_out  = host_stream.next_out;
		guest_stream.avail_out = host_stream.avail_out;
		guest_stream.total_out = host_stream.total_out;
		guest_stream.msg       = host_stream.msg;
		guest_stream.state     = reinterpret_cast<mz_internal_state*>(host_stream.state);
		guest_stream.data_type = host_stream.data_type;
		guest_stream.adler     = host_stream.adler;
		guest_stream.reserved  = host_stream.reserved;
		std::memcpy(strm, &guest_stream, sizeof(guest_stream));
		return rc;
	}

	return inflate(strm, flush);
}

static int InflateEndCompatImpl(mz_streamp strm)
{
	StreamLayoutInfo layout {};
	if (!GetLegacyStreamLayout(strm, &layout))
	{
		return inflateEnd(strm);
	}

	if (IsCompatLegacyStream72(layout.size))
	{
		GuestZlibStream72 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		mz_stream host_stream {};
		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = guest_stream.total_in;
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = guest_stream.total_out;
		host_stream.msg       = guest_stream.msg;
		host_stream.state     = guest_stream.state;
		host_stream.zalloc    = HostAlloc;
		host_stream.zfree     = HostFree;
		host_stream.opaque    = nullptr;

		const int rc = inflateEnd(&host_stream);
		ForgetLegacyStreamLayout(strm);
		return rc;
	}
	if (IsCompatStream112(layout.size))
	{
		GuestZlibStream112 guest_stream {};
		std::memcpy(&guest_stream, strm, sizeof(guest_stream));

		mz_stream host_stream {};
		host_stream.next_in    = reinterpret_cast<const unsigned char*>(guest_stream.next_in);
		host_stream.avail_in   = guest_stream.avail_in;
		host_stream.total_in   = static_cast<mz_ulong>(guest_stream.total_in);
		host_stream.next_out   = reinterpret_cast<unsigned char*>(guest_stream.next_out);
		host_stream.avail_out  = guest_stream.avail_out;
		host_stream.total_out  = static_cast<mz_ulong>(guest_stream.total_out);
		host_stream.msg        = guest_stream.msg;
		host_stream.state      = guest_stream.state;
		host_stream.zalloc     = HostAlloc;
		host_stream.zfree      = HostFree;
		host_stream.opaque     = nullptr;
		host_stream.data_type  = guest_stream.data_type;
		host_stream.adler      = static_cast<mz_ulong>(guest_stream.adler);
		host_stream.reserved   = static_cast<mz_ulong>(guest_stream.reserved);

		const int rc = inflateEnd(&host_stream);
		ForgetLegacyStreamLayout(strm);
		return rc;
	}

	return inflateEnd(strm);
}

// Minimal zlib-like API compatibility for EOS modules that expect standard
// inflate entry points from a PS5 shipping SDK import table.
static KYTY_SYSV_ABI int InflateInit(void* strm, const char* version, int stream_size)
{
	PRINT_NAME();

	printf("\t version      = %s\n", version != nullptr ? version : "(null)");
	printf("\t stream_size  = %d\n", stream_size);
	printf("\t sizeof(z_stream) = %zu\n", sizeof(mz_stream));

	if (strm == nullptr)
	{
		return Z_STREAM_ERROR;
	}

	if (IsCompatLegacyStream72(stream_size) || IsCompatStream112(stream_size))
	{
		const int rc = InflateInitCompatInitImpl(static_cast<mz_streamp>(strm), MZ_DEFAULT_WINDOW_BITS, version, stream_size);
		RegisterLegacyStreamLayout(static_cast<mz_streamp>(strm), stream_size);
		return rc;
	}

	auto* stream   = static_cast<mz_streamp>(strm);
	stream->zalloc = HostAlloc;
	stream->zfree  = HostFree;
	stream->opaque = nullptr;
	RegisterLegacyStreamLayout(stream, sizeof(mz_stream));
	return inflateInit2(stream, MZ_DEFAULT_WINDOW_BITS);
}

static KYTY_SYSV_ABI int InflateInit2(void* strm, int window_bits, const char* version, int stream_size)
{
	PRINT_NAME();
	printf("\t window_bits  = %d\n", window_bits);
	printf("\t version      = %s\n", version != nullptr ? version : "(null)");
	printf("\t stream_size  = %d\n", stream_size);
	printf("\t sizeof(z_stream) = %zu\n", sizeof(mz_stream));

	if (strm == nullptr)
	{
		return Z_STREAM_ERROR;
	}

	if (IsCompatLegacyStream72(stream_size) || IsCompatStream112(stream_size))
	{
		const int rc = InflateInitCompatInitImpl(static_cast<mz_streamp>(strm), window_bits, version, stream_size);
		RegisterLegacyStreamLayout(static_cast<mz_streamp>(strm), stream_size);
		return rc;
	}

	auto* stream   = static_cast<mz_streamp>(strm);
	stream->zalloc = HostAlloc;
	stream->zfree  = HostFree;
	stream->opaque = nullptr;
	RegisterLegacyStreamLayout(stream, sizeof(mz_stream));
	return inflateInit2(stream, window_bits);
}

static KYTY_SYSV_ABI int Inflate(mz_streamp strm, int flush)
{
	PRINT_NAME();
	return InflateCompatImpl(strm, flush);
}

static KYTY_SYSV_ABI int InflateEnd(mz_streamp strm)
{
	PRINT_NAME();
	return InflateEndCompatImpl(strm);
}

static KYTY_SYSV_ABI int InflateCompatFlexible(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t, uint64_t)
{
	if (a0 == 0)
	{
		return Z_STREAM_ERROR;
	}

	auto* stream = reinterpret_cast<void*>(a0);
	const auto window_bits = static_cast<int64_t>(a1);

	// Heuristic dispatch: when the second argument is a small integer, assume
	// callers are using inflateInit2_ (window bits as second positional arg).
	if (window_bits <= static_cast<int64_t>(MZ_DEFAULT_WINDOW_BITS) && window_bits >= -static_cast<int64_t>(MZ_DEFAULT_WINDOW_BITS))
	{
		return InflateInit2(stream, static_cast<int>(window_bits), reinterpret_cast<const char*>(a2), static_cast<int>(a3));
	}

	// Otherwise treat as inflateInit_(stream, version, stream_size).
	return InflateInit(stream, reinterpret_cast<const char*>(a1), static_cast<int>(a2));
}

// Conservative stub: some EOS binaries reference private/internal zlib-like entry
// points with private NIDs. Returning OK keeps execution moving until real
// mappings are recovered from symbol discovery.
static KYTY_SYSV_ABI int InflateCompatReturnZero(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	PRINT_NAME();
	printf("\t a0 = 0x%016" PRIx64 "\n", a0);
	printf("\t a1 = 0x%016" PRIx64 "\n", a1);
	printf("\t a2 = 0x%016" PRIx64 "\n", a2);
	printf("\t a3 = 0x%016" PRIx64 "\n", a3);
	printf("\t a4 = 0x%016" PRIx64 "\n", a4);
	printf("\t a5 = 0x%016" PRIx64 "\n", a5);
	return 0;
}

} // namespace EOSSDKPS5Shipping

LIB_DEFINE(InitEOSSDKPS5Shipping_1)
{
	LIB_FUNC("9ET3A90qn2o", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("Ji+98V2xGZA", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("D0odCqXaXgk", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("jTKhlnqi5+o", EOSSDKPS5Shipping::InflateCompatFlexible);
	LIB_FUNC("fKk7unahoVM", EOSSDKPS5Shipping::InflateCompatFlexible);
	LIB_FUNC("Z0pL-Tae6N4", EOSSDKPS5Shipping::InflateCompatFlexible);
	LIB_FUNC("gnWUEMlAxZY", EOSSDKPS5Shipping::InflateCompatFlexible);
	LIB_FUNC("70tCTRcliEQ", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("MM-aVBE7p-A", EOSSDKPS5Shipping::InflateInit2);
	LIB_FUNC("dbDvWQUel6A", EOSSDKPS5Shipping::Inflate);
	LIB_FUNC("XVx6JyC0Mv4", EOSSDKPS5Shipping::InflateEnd);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
