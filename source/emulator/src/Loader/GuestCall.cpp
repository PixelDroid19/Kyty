#include "Emulator/Loader/GuestCall.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::GuestCall {

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

uint64_t KYTY_SYSV_ABI Invoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	uint64_t result = 0;

	// The guest ABI permits an RBP walker to follow frames until a frame whose
	// parent is null and whose return slot is zero. Install exactly that frame
	// for every host-to-guest call, then restore the host RBP before returning.
	// The 24-byte reservation holds the 16-byte sentinel plus alignment padding.
	// After saving RBP, RSP must be 16-byte aligned immediately before `call` so
	// the guest observes the SysV-required RSP % 16 == 8 on entry.
	// Use memory operands and consume every input before changing RSP/RBP. This
	// prevents the compiler from assigning a late-read operand to RBP or to a
	// register overwritten while the guest argument registers are populated.
	//
	// An arbitrary guest callee may overwrite every SysV caller-saved integer,
	// x87 and SIMD register. The explicit clobber set makes that call boundary
	// visible to the optimizer instead of treating the asm as an integer-only
	// operation.
	asm volatile("movq %[target], %%rax\n\t"
	             "movq %[arg0], %%rdi\n\t"
	             "movq %[arg1], %%rsi\n\t"
	             "movq %[arg2], %%rdx\n\t"
	             "pushq %%rbp\n\t"
	             "subq $24, %%rsp\n\t"
	             "xorq %%rcx, %%rcx\n\t"
	             "movq %%rcx, 0(%%rsp)\n\t"
	             "movq %%rcx, 8(%%rsp)\n\t"
	             "movq %%rsp, %%rbp\n\t"
	             "call *%%rax\n\t"
	             "addq $24, %%rsp\n\t"
	             "popq %%rbp\n\t"
	             : "=&a"(result)
	             : [target] "m"(target), [arg0] "m"(arg0), [arg1] "m"(arg1), [arg2] "m"(arg2)
	             : "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "st", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
	               "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");

	return result;
}

#else
#error "Kyty guest calls require an x86-64 compiler with a supported SysV assembly boundary"
#endif

} // namespace Kyty::Loader::GuestCall

#endif // KYTY_EMU_ENABLED
