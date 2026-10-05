#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

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

// Guest zlib streams come in two layouts: the LP64 zlib z_stream (112 bytes)
// and a 72-byte legacy layout that ends at zalloc. Every call copies the guest
// stream into a host mz_stream, runs miniz with host allocators (guest
// allocation callbacks cannot be called from the host), and copies the result
// back. The layout is fixed by the stream_size passed to the init call.
struct GuestZlibStream72
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

enum class GuestStreamLayout
{
	Legacy72,
	Zlib112,
};

static Core::Mutex                                  g_eos_stream_mutex;
static std::unordered_map<void*, GuestStreamLayout> g_eos_stream_layouts;

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

// zlib rejects a stream_size or major version it was not built for.
static bool LayoutFromInit(const char* version, int stream_size, GuestStreamLayout* layout)
{
	if (version == nullptr || version[0] != '1')
	{
		return false;
	}
	if (stream_size == static_cast<int>(sizeof(GuestZlibStream72)))
	{
		*layout = GuestStreamLayout::Legacy72;
		return true;
	}
	if (stream_size == static_cast<int>(sizeof(GuestZlibStream112)))
	{
		*layout = GuestStreamLayout::Zlib112;
		return true;
	}
	return false;
}

template <typename Guest>
static void LoadCommonFields(const Guest& guest, mz_stream* host)
{
	host->next_in   = static_cast<const unsigned char*>(guest.next_in);
	host->avail_in  = guest.avail_in;
	host->total_in  = static_cast<mz_ulong>(guest.total_in);
	host->next_out  = static_cast<unsigned char*>(guest.next_out);
	host->avail_out = guest.avail_out;
	host->total_out = static_cast<mz_ulong>(guest.total_out);
	host->msg       = guest.msg;
	host->state     = guest.state;
}

template <typename Guest>
static void StoreCommonFields(const mz_stream& host, Guest* guest)
{
	guest->next_in   = host.next_in;
	guest->avail_in  = host.avail_in;
	guest->total_in  = host.total_in;
	guest->next_out  = host.next_out;
	guest->avail_out = host.avail_out;
	guest->total_out = host.total_out;
	guest->msg       = host.msg;
	guest->state     = host.state;
}

static void LoadStream(const void* guest, GuestStreamLayout layout, mz_stream* host)
{
	*host = {};
	if (layout == GuestStreamLayout::Zlib112)
	{
		GuestZlibStream112 stream {};
		std::memcpy(&stream, guest, sizeof(stream));
		LoadCommonFields(stream, host);
		host->data_type = stream.data_type;
		host->adler     = static_cast<mz_ulong>(stream.adler);
		host->reserved  = static_cast<mz_ulong>(stream.reserved);
	} else
	{
		GuestZlibStream72 stream {};
		std::memcpy(&stream, guest, sizeof(stream));
		LoadCommonFields(stream, host);
	}
	host->zalloc = HostAlloc;
	host->zfree  = HostFree;
	host->opaque = nullptr;
}

// The guest's own allocator fields are left as the guest set them.
static void StoreStream(const mz_stream& host, GuestStreamLayout layout, void* guest)
{
	if (layout == GuestStreamLayout::Zlib112)
	{
		GuestZlibStream112 stream {};
		std::memcpy(&stream, guest, sizeof(stream));
		StoreCommonFields(host, &stream);
		stream.data_type = host.data_type;
		stream.adler     = host.adler;
		stream.reserved  = host.reserved;
		std::memcpy(guest, &stream, sizeof(stream));
	} else
	{
		GuestZlibStream72 stream {};
		std::memcpy(&stream, guest, sizeof(stream));
		StoreCommonFields(host, &stream);
		std::memcpy(guest, &stream, sizeof(stream));
	}
}

template <typename Init>
static int InitGuestStream(void* guest, const char* version, int stream_size, Init init)
{
	GuestStreamLayout layout {};
	if (guest == nullptr)
	{
		return MZ_STREAM_ERROR;
	}
	if (!LayoutFromInit(version, stream_size, &layout))
	{
		return MZ_VERSION_ERROR;
	}
	mz_stream host {};
	LoadStream(guest, layout, &host);
	const int rc = init(&host);
	StoreStream(host, layout, guest);
	if (rc == MZ_OK)
	{
		Core::LockGuard lock(g_eos_stream_mutex);
		g_eos_stream_layouts[guest] = layout;
	}
	return rc;
}

// A stream that no init call of this module set up is a guest error.
template <typename Op>
static int RunGuestStream(void* guest, bool end, Op op)
{
	GuestStreamLayout layout {};
	{
		Core::LockGuard lock(g_eos_stream_mutex);
		const auto      it = g_eos_stream_layouts.find(guest);
		if (it == g_eos_stream_layouts.end())
		{
			return MZ_STREAM_ERROR;
		}
		layout = it->second;
		if (end)
		{
			g_eos_stream_layouts.erase(it);
		}
	}
	mz_stream host {};
	LoadStream(guest, layout, &host);
	const int rc = op(&host);
	StoreStream(host, layout, guest);
	return rc;
}

static KYTY_SYSV_ABI int InflateInit2(void* strm, int window_bits, const char* version, int stream_size)
{
	PRINT_NAME();
	return InitGuestStream(strm, version, stream_size, [window_bits](mz_stream* host) { return mz_inflateInit2(host, window_bits); });
}

static KYTY_SYSV_ABI int Inflate(void* strm, int flush)
{
	PRINT_NAME();
	return RunGuestStream(strm, false, [flush](mz_stream* host) { return mz_inflate(host, flush); });
}

static KYTY_SYSV_ABI int InflateEnd(void* strm)
{
	PRINT_NAME();
	return RunGuestStream(strm, true, [](mz_stream* host) { return mz_inflateEnd(host); });
}

static KYTY_SYSV_ABI int DeflateInit2(void* strm, int level, int method, int window_bits, int mem_level, int strategy,
                                      const char* version, int stream_size)
{
	PRINT_NAME();
	return InitGuestStream(strm, version, stream_size, [=](mz_stream* host)
	                       { return mz_deflateInit2(host, level, method, window_bits, mem_level, strategy); });
}

static KYTY_SYSV_ABI int Deflate(void* strm, int flush)
{
	PRINT_NAME();
	return RunGuestStream(strm, false, [flush](mz_stream* host) { return mz_deflate(host, flush); });
}

static KYTY_SYSV_ABI int DeflateEnd(void* strm)
{
	PRINT_NAME();
	return RunGuestStream(strm, true, [](mz_stream* host) { return mz_deflateEnd(host); });
}

static KYTY_SYSV_ABI unsigned long Crc32(unsigned long crc, const unsigned char* buf, unsigned int len)
{
	return mz_crc32(crc, buf, len);
}

// Conservative stub: some EOS binaries reference private/internal zlib-like entry
// points with private NIDs. Returning OK keeps execution moving until real
// mappings are recovered from symbol discovery.
static KYTY_SYSV_ABI int InflateCompatReturnZero(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t a0 = 0x%016" PRIx64 "\n", a0);
	KYTY_LOG_DEBUG("\t a1 = 0x%016" PRIx64 "\n", a1);
	KYTY_LOG_DEBUG("\t a2 = 0x%016" PRIx64 "\n", a2);
	KYTY_LOG_DEBUG("\t a3 = 0x%016" PRIx64 "\n", a3);
	KYTY_LOG_DEBUG("\t a4 = 0x%016" PRIx64 "\n", a4);
	KYTY_LOG_DEBUG("\t a5 = 0x%016" PRIx64 "\n", a5);
	return 0;
}

} // namespace EOSSDKPS5Shipping

LIB_DEFINE(InitEOSSDKPS5Shipping_1)
{
	LIB_FUNC("9ET3A90qn2o", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("Ji+98V2xGZA", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("D0odCqXaXgk", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("jTKhlnqi5+o", EOSSDKPS5Shipping::Crc32);
	LIB_FUNC("fKk7unahoVM", EOSSDKPS5Shipping::DeflateInit2);
	LIB_FUNC("Z0pL-Tae6N4", EOSSDKPS5Shipping::DeflateEnd);
	LIB_FUNC("gnWUEMlAxZY", EOSSDKPS5Shipping::Deflate);
	LIB_FUNC("70tCTRcliEQ", EOSSDKPS5Shipping::InflateCompatReturnZero);
	LIB_FUNC("MM-aVBE7p-A", EOSSDKPS5Shipping::InflateInit2);
	LIB_FUNC("dbDvWQUel6A", EOSSDKPS5Shipping::Inflate);
	LIB_FUNC("XVx6JyC0Mv4", EOSSDKPS5Shipping::InflateEnd);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
