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

uint64_t KYTY_SYSV_ABI Invoke4(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	uint64_t result = 0;

	asm volatile("movq %[target], %%rax\n\t"
	             "movq %[arg0], %%rdi\n\t"
	             "movq %[arg1], %%rsi\n\t"
	             "movq %[arg2], %%rdx\n\t"
	             "movq %[arg3], %%rcx\n\t"
	             "pushq %%rbp\n\t"
	             "subq $24, %%rsp\n\t"
	             "xorq %%r8, %%r8\n\t"
	             "movq %%r8, 0(%%rsp)\n\t"
	             "movq %%r8, 8(%%rsp)\n\t"
	             "movq %%rsp, %%rbp\n\t"
	             "call *%%rax\n\t"
	             "addq $24, %%rsp\n\t"
	             "popq %%rbp\n\t"
	             : "=&a"(result)
	             : [target] "m"(target), [arg0] "m"(arg0), [arg1] "m"(arg1), [arg2] "m"(arg2), [arg3] "m"(arg3)
	             : "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "st", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
	               "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");

	return result;
}

uint64_t KYTY_SYSV_ABI InvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top)
{
	EXIT_IF(target == 0 || stack_top == nullptr);

	uint64_t result = 0;

	const uintptr_t guest_rsp = reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0xf);
	const uintptr_t guest_rbp = guest_rsp - 4u * sizeof(uint64_t);
	auto* const     root      = reinterpret_cast<uintptr_t*>(guest_rbp);
	root[0]                   = 0;
	root[1]                   = 0;

	asm volatile("movq %[target], %%rax\n\t"
	             "movq %[arg0], %%rdi\n\t"
	             "movq %[arg1], %%rsi\n\t"
	             "movq %[arg2], %%rdx\n\t"
	             "movq %[guest_rsp], %%r10\n\t"
	             "movq %[guest_rbp], %%r11\n\t"
	             "pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rsp, %%r12\n\t"
	             "movq %%rbp, %%r13\n\t"
	             "movq %%r10, %%rsp\n\t"
	             "movq %%r11, %%rbp\n\t"
	             "callq *%%rax\n\t"
	             "movq %%r13, %%rbp\n\t"
	             "movq %%r12, %%rsp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             : "=&a"(result)
	             : [target] "m"(target), [arg0] "m"(arg0), [arg1] "m"(arg1), [arg2] "m"(arg2), [guest_rsp] "m"(guest_rsp),
	               [guest_rbp] "m"(guest_rbp)
	             : "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "st", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
	               "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory",
	               "cc");

	return result;
}

#else
#error "Kyty guest calls require an x86-64 compiler with a supported SysV assembly boundary"
#endif

} // namespace Kyty::Loader::GuestCall

#endif // KYTY_EMU_ENABLED
