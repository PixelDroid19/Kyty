#include "Emulator/Loader/GuestCall.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "guest-stack-frame integration failure: %s\n", message);
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

struct Frame
{
	Frame*   parent;
	uint64_t return_address;
};

bool HasGuestTerminator(Frame* frame)
{
	if (frame == nullptr)
	{
		return false;
	}

	Frame* parent = frame->parent;
	return parent != nullptr && parent->parent == nullptr && parent->return_address == 0;
}

bool HasSysvEntryAlignment(Frame* frame)
{
	// With a conventional frame prologue, frame points at the saved RBP and the
	// entry RSP (which held the return address) is frame + 8.
	return (reinterpret_cast<uintptr_t>(frame) + sizeof(uint64_t)) % 16 == 8;
}

extern "C" uint64_t KYTY_SYSV_ABI MainEntryProbe(uint64_t marker, uint64_t value)
{
	Frame* frame = reinterpret_cast<Frame*>(__builtin_frame_address(0));
	return HasGuestTerminator(frame) && HasSysvEntryAlignment(frame) ? marker ^ value : 0;
}

extern "C" uint64_t KYTY_SYSV_ABI PthreadEntryProbe(uint64_t marker)
{
	Frame* frame = reinterpret_cast<Frame*>(__builtin_frame_address(0));
	return HasGuestTerminator(frame) && HasSysvEntryAlignment(frame) ? marker : 0;
}

extern "C" uint64_t KYTY_SYSV_ABI ModuleEntryProbe(uint64_t first, uint64_t second, uint64_t third)
{
	Frame* frame = reinterpret_cast<Frame*>(__builtin_frame_address(0));
	return HasGuestTerminator(frame) && HasSysvEntryAlignment(frame) ? first + second + third : 0;
}

} // namespace

int main()
{
	constexpr uint64_t main_marker    = UINT64_C(0x1122334455667788);
	constexpr uint64_t main_value     = UINT64_C(0x8877665544332211);
	constexpr uint64_t pthread_marker = UINT64_C(0xa5a5a5a55a5a5a5a);
	constexpr uint64_t module_first   = UINT64_C(0x0102030405060708);
	constexpr uint64_t module_second  = UINT64_C(0x1112131415161718);
	constexpr uint64_t module_third   = UINT64_C(0x2122232425262728);

	const uint64_t direct_result = MainEntryProbe(main_marker, main_value);
	Expect(direct_result != (main_marker ^ main_value),
	       "a direct host call must not be mistaken for a guest boundary with a zero-return frame");

	const uint64_t main_result = Kyty::Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(&MainEntryProbe), main_marker, main_value, 0);
	Expect(main_result == (main_marker ^ main_value),
	       "main-style guest entry must see a zero-return frame terminator before the host tail");

	const uint64_t pthread_result = Kyty::Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(&PthreadEntryProbe), pthread_marker, 0, 0);
	Expect(pthread_result == pthread_marker, "pthread-style guest entry must see a zero-return frame terminator before the host tail");

	const uint64_t module_result =
	    Kyty::Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(&ModuleEntryProbe), module_first, module_second, module_third);
	Expect(module_result == module_first + module_second + module_third,
	       "module-style guest entry must receive three arguments through the shared boundary");

	return 0;
}
