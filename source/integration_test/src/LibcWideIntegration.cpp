#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Kyty;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "libc-wide integration failure: %s\n", message);
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

bool WideEquals(const uint16_t* left, const char* right)
{
	while (*right != '\0')
	{
		if (*left++ != static_cast<uint8_t>(*right++))
		{
			return false;
		}
	}
	return *left == 0;
}

void AsciiToWide(const char* source, uint16_t* destination, size_t capacity)
{
	size_t index = 0;
	while (source[index] != '\0')
	{
		Expect(index + 1 < capacity, "wide fixture capacity must include the terminator");
		destination[index] = static_cast<uint8_t>(source[index]);
		index++;
	}
	destination[index] = 0;
}

} // namespace

int main(int argc, char** argv)
{
	Loader::SymbolDatabase symbols;
	Expect(Libs::Init(U"libc_1", &symbols), "libc HLE registration must succeed");

	const auto* iswctype_record = FindLibcFunction(symbols, "CyXs2l-1kNA");
	const auto* wcslen_record   = FindLibcFunction(symbols, "WkkeywLJcgU");
	const auto* wcsncpy_record  = FindLibcFunction(symbols, "0nV21JjYCH8");
	const auto* vsw_record      = FindLibcFunction(symbols, "u0XOsuOmOzc");
	Expect(iswctype_record != nullptr, "_Iswctype NID must resolve");
	Expect(wcslen_record != nullptr, "wcslen NID must resolve");
	Expect(wcsncpy_record != nullptr, "wcsncpy NID must resolve");
	Expect(vsw_record != nullptr, "vswprintf NID must resolve");

	using IswctypeFn = int(KYTY_SYSV_ABI*)(uint32_t, int);
	using WcslenFn   = size_t(KYTY_SYSV_ABI*)(const uint16_t*);
	using WcsncpyFn  = uint16_t*(KYTY_SYSV_ABI*)(uint16_t*, const uint16_t*, size_t);
	using VswFn      = int(KYTY_SYSV_ABI*)(uint16_t*, size_t, const uint16_t*, Libs::VaList*);

	const auto iswctype = reinterpret_cast<IswctypeFn>(iswctype_record->vaddr);
	const auto wcslen   = reinterpret_cast<WcslenFn>(wcslen_record->vaddr);
	const auto wcsncpy  = reinterpret_cast<WcsncpyFn>(wcsncpy_record->vaddr);
	const auto vsw      = reinterpret_cast<VswFn>(vsw_record->vaddr);

	if (argc == 2 && std::strcmp(argv[1], "reject_class") == 0)
	{
		return iswctype('0', 3);
	}
	if (argc == 2 && std::strcmp(argv[1], "reject_non_ascii") == 0)
	{
		return iswctype(0x80, 2);
	}
	Expect(argc == 1, "unknown integration scenario");

	const char* captured_sequence = "08x  size: %%ld";
	for (const char* cursor = captured_sequence; *cursor != '\0'; cursor++)
	{
		const bool expected_digit = *cursor >= '0' && *cursor <= '9';
		Expect((iswctype(static_cast<uint8_t>(*cursor), 2) != 0) == expected_digit,
		       "class 2 must classify only decimal digits in the captured formatter sequence");
	}

	const uint16_t text[] = {'A', 'b', 'c', 0};
	Expect(wcslen(text) == 3, "wcslen must count UTF-16 code units");

	const uint16_t short_source[] = {'A', 'B', 0};
	uint16_t       padded[5]      = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
	Expect(wcsncpy(padded, short_source, 5) == padded, "wcsncpy must return its destination");
	Expect(padded[0] == 'A' && padded[1] == 'B' && padded[2] == 0 && padded[3] == 0 && padded[4] == 0,
	       "wcsncpy must zero-pad after an early terminator");

	const uint16_t long_source[] = {'A', 'B', 'C', 'D', 0};
	uint16_t       truncated[4]  = {0xffff, 0xffff, 0xffff, 0xbeef};
	Expect(wcsncpy(truncated, long_source, 3) == truncated, "truncating wcsncpy must return its destination");
	Expect(truncated[0] == 'A' && truncated[1] == 'B' && truncated[2] == 'C' && truncated[3] == 0xbeef,
	       "wcsncpy must not append a terminator when the source occupies count units");

	Libs::VaRegSave registers {};
	registers.gp[0] = 42;
	Libs::VaList args {};
	args.gp_offset         = 0;
	args.fp_offset         = offsetof(Libs::VaRegSave, fp);
	args.overflow_arg_area = nullptr;
	args.reg_save_area     = &registers;

	const char* observed_format = "sceKernelReserveVirtualRange failed with error code: 0x%08x  size: %%ld";
	const char* expected_output = "sceKernelReserveVirtualRange failed with error code: 0x0000002a  size: %ld";
	uint16_t    format[128] {};
	uint16_t    output[128] {};
	AsciiToWide(observed_format, format, 128);
	const int written = vsw(output, 128, format, &args);
	Expect(written == static_cast<int>(std::strlen(expected_output)), "vswprintf must report UTF-16 units written");
	Expect(WideEquals(output, expected_output), "vswprintf must preserve %% and format the observed %08x argument");

	const uint16_t unsupported[] = {'%', 's', 0};
	Expect(vsw(output, 64, unsupported, &args) == -1, "unsupported wide string conversion must fail explicitly");
	Expect(output[0] == 0, "failed conversion must clear output");
	return 0;
}
