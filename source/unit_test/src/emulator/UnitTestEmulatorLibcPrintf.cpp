#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"

#include <cstddef>
#include <cstdint>

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

UT_END();
