#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

UT_BEGIN(EmulatorLibcPrintf);

// A guest may pass a null pointer for a %s conversion. The C library the guest
// links against renders that as the literal "(null)" instead of dereferencing
// the pointer, and guest error handlers rely on that behavior. Build a SysV
// register-save frame by hand and drive the shared snprintf formatter directly.
TEST(EmulatorLibcPrintf, NullStringArgumentRendersNullLiteral)
{
	using namespace Kyty::Libs;

	char        destination[64] = {};
	const char* format          = "%s";

	alignas(16) VaContext ctx {};
	ctx.reg_save_area.gp[0] = reinterpret_cast<uint64_t>(destination);
	ctx.reg_save_area.gp[1] = sizeof(destination);
	ctx.reg_save_area.gp[2] = reinterpret_cast<uint64_t>(format);
	ctx.reg_save_area.gp[3] = 0; // the null %s argument

	uint64_t overflow_area          = 0;
	ctx.va_list.gp_offset           = offsetof(VaRegSave, gp);
	ctx.va_list.fp_offset           = offsetof(VaRegSave, fp);
	ctx.va_list.reg_save_area       = &ctx.reg_save_area;
	ctx.va_list.overflow_arg_area   = &overflow_area;

	const int written = GetSnrintfCtxFunc()(&ctx);

	EXPECT_STREQ(destination, "(null)");
	EXPECT_EQ(written, 6);
}

// A guest may also pass a null format pointer. The formatter must report an error
// instead of dereferencing it, so a faulty guest logging path fails safely rather
// than taking down the emulator.
TEST(EmulatorLibcPrintf, NullFormatReportsErrorWithoutDereferencing)
{
	using namespace Kyty::Libs;

	char destination[16] = {'x', 0};

	alignas(16) VaContext ctx {};
	ctx.reg_save_area.gp[0] = reinterpret_cast<uint64_t>(destination);
	ctx.reg_save_area.gp[1] = sizeof(destination);
	ctx.reg_save_area.gp[2] = 0; // null format

	uint64_t overflow_area        = 0;
	ctx.va_list.gp_offset         = offsetof(VaRegSave, gp);
	ctx.va_list.fp_offset         = offsetof(VaRegSave, fp);
	ctx.va_list.reg_save_area     = &ctx.reg_save_area;
	ctx.va_list.overflow_arg_area = &overflow_area;

	const int written = GetSnrintfCtxFunc()(&ctx);

	EXPECT_LT(written, 0);
}

static std::pair<std::string, int> FormatDouble(const char* format, double value)
{
	using namespace Kyty::Libs;

	std::array<char, 128> destination {};
	alignas(16) VaContext ctx {};
	ctx.reg_save_area.gp[0] = reinterpret_cast<uint64_t>(destination.data());
	ctx.reg_save_area.gp[1] = destination.size();
	ctx.reg_save_area.gp[2] = reinterpret_cast<uint64_t>(format);
	std::memcpy(&ctx.reg_save_area.fp[0], &value, sizeof(value));

	uint64_t overflow_area        = 0;
	ctx.va_list.gp_offset         = offsetof(VaRegSave, gp);
	ctx.va_list.fp_offset         = offsetof(VaRegSave, fp);
	ctx.va_list.reg_save_area     = &ctx.reg_save_area;
	ctx.va_list.overflow_arg_area = &overflow_area;

	const int written = GetSnrintfCtxFunc()(&ctx);
	return {destination.data(), written};
}

TEST(EmulatorLibcPrintf, GeneralFloatFormattingFollowsCContract)
{
	struct Case
	{
		const char* format;
		double      value;
		const char* expected;
	};

	const Case cases[] = {
	    {"%.17g", 1.0, "1"},
	    {"%.17g", 0.5, "0.5"},
	    {"%.17g", 1e10, "10000000000"},
	    {"%g", 0.0001, "0.0001"},
	    {"%g", 0.00001, "1e-05"},
	    {"%#.3g", 1.0, "1.00"},
	    {"%.0g", 12.3, "1e+01"},
	    {"%G", 1e10, "1E+10"},
	    {"%+010.4g", 12.3, "+0000012.3"},
	    {"%-10.4g", 12.3, "12.3      "},
	    {"%.17g", -0.0, "-0"},
	    {"%g", std::numeric_limits<double>::infinity(), "inf"},
	    {"%G", std::numeric_limits<double>::quiet_NaN(), "NAN"},
	};

	for (const auto& test: cases)
	{
		const auto [formatted, written] = FormatDouble(test.format, test.value);
		EXPECT_EQ(formatted, test.expected) << "format: " << test.format;
		EXPECT_EQ(written, static_cast<int>(formatted.size())) << "format: " << test.format;
	}
}

// Gen5 NID NC4MSB+BRQg (ObjectDefinition path builder): same format shape as
// snprintf, but guest checks `r == 0`. Map written-length → errno_t-style 0/-1.
static int SnprintfErrnoStyle(int written, size_t n)
{
	if (written < 0)
	{
		return written;
	}
	if (static_cast<size_t>(written) >= n)
	{
		return -1;
	}
	return 0;
}

TEST(EmulatorLibcPrintf, SnprintfErrnoStyleReturnsZeroWhenOutputFits)
{
	using namespace Kyty::Libs;

	char destination[256] = {};
	const char* format    = "%s";
	const char* piece     = "gfx/";

	alignas(16) VaContext ctx {};
	ctx.reg_save_area.gp[0] = reinterpret_cast<uint64_t>(destination);
	ctx.reg_save_area.gp[1] = sizeof(destination);
	ctx.reg_save_area.gp[2] = reinterpret_cast<uint64_t>(format);
	ctx.reg_save_area.gp[3] = reinterpret_cast<uint64_t>(piece);

	uint64_t overflow_area        = 0;
	ctx.va_list.gp_offset         = offsetof(VaRegSave, gp);
	ctx.va_list.fp_offset         = offsetof(VaRegSave, fp);
	ctx.va_list.reg_save_area     = &ctx.reg_save_area;
	ctx.va_list.overflow_arg_area = &overflow_area;

	const int written = GetSnrintfCtxFunc()(&ctx);
	EXPECT_STREQ(destination, "gfx/");
	EXPECT_EQ(written, 4);
	// Standard snprintf length is non-zero; errno-style wrapper used by NC4MSB+BRQg is 0.
	EXPECT_EQ(SnprintfErrnoStyle(written, sizeof(destination)), 0);
	EXPECT_EQ(SnprintfErrnoStyle(written, 2), -1);
}

UT_END();
