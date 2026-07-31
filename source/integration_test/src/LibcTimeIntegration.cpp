#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace Kyty;

namespace {

struct GuestTm
{
	int32_t sec;
	int32_t min;
	int32_t hour;
	int32_t mday;
	int32_t mon;
	int32_t year;
	int32_t wday;
	int32_t yday;
	int32_t isdst;
};

static_assert(sizeof(GuestTm) == 36);

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "libc-time integration failure: %s\n", message);
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

} // namespace

int main()
{
	Loader::SymbolDatabase symbols;
	Expect(Libs::Init(U"libc_1", &symbols), "libc HLE registration must succeed");

	const auto* localtime_s_record = FindLibcFunction(symbols, "fiiNDnNBKVY");
	const auto* gmtime_record      = FindLibcFunction(symbols, "1mecP7RgI2A");
	Expect(localtime_s_record != nullptr, "localtime_s NID must resolve");
	Expect(gmtime_record != nullptr, "gmtime NID must resolve");

	using LocaltimeSafeFn = GuestTm*(KYTY_SYSV_ABI*)(const int64_t*, GuestTm*);
	using GmtimeFn        = GuestTm*(KYTY_SYSV_ABI*)(const int64_t*);
	const auto localtime_safe = reinterpret_cast<LocaltimeSafeFn>(localtime_s_record->vaddr);
	const auto gmtime          = reinterpret_cast<GmtimeFn>(gmtime_record->vaddr);

	struct GuardedTm
	{
		uint64_t before;
		GuestTm  value;
		uint64_t after;
	};

	const int64_t epoch = 0;
	GuardedTm     guarded {0x1122334455667788ULL, {}, 0x8877665544332211ULL};
	Expect(localtime_safe(&epoch, &guarded.value) == &guarded.value, "localtime_s must return the guest output buffer");
	Expect(guarded.before == 0x1122334455667788ULL && guarded.after == 0x8877665544332211ULL,
	       "localtime_s must write exactly one 36-byte guest tm");
	Expect(guarded.value.sec >= 0 && guarded.value.sec <= 60, "localtime_s seconds must be normalized");
	Expect(guarded.value.min >= 0 && guarded.value.min <= 59, "localtime_s minutes must be normalized");
	Expect(guarded.value.hour >= 0 && guarded.value.hour <= 23, "localtime_s hours must be normalized");
	Expect(guarded.value.mon >= 0 && guarded.value.mon <= 11, "localtime_s month must be normalized");

	const GuestTm* utc = gmtime(&epoch);
	Expect(utc != nullptr, "gmtime must return guest storage");
	Expect(utc->year == 70 && utc->mon == 0 && utc->mday == 1, "gmtime must expose guest tm fields");
	return 0;
}
