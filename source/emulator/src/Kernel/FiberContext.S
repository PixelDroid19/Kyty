#ifdef __APPLE__
#define KYTY_ASM_SYMBOL(name) _##name
#else
#define KYTY_ASM_SYMBOL(name) name
#endif

	.text
	.p2align 4
	.globl KYTY_ASM_SYMBOL(KytyFiberSaveContext)
#if defined(__ELF__)
	.type KYTY_ASM_SYMBOL(KytyFiberSaveContext), @function
#endif
KYTY_ASM_SYMBOL(KytyFiberSaveContext):
	movq %rdi, %r10
	movq %rbx, 0(%r10)
	movq %rbp, 8(%r10)
	movq %rdi, 16(%r10)
	movq %rsi, 24(%r10)
	movq %r12, 32(%r10)
	movq %r13, 40(%r10)
	movq %r14, 48(%r10)
	movq %r15, 56(%r10)
	leaq 8(%rsp), %r11
	movq %r11, 64(%r10)
	movq (%rsp), %r11
	movq %r11, 72(%r10)
	xorl %eax, %eax
	ret
#if defined(__ELF__)
	.size KYTY_ASM_SYMBOL(KytyFiberSaveContext), .-KYTY_ASM_SYMBOL(KytyFiberSaveContext)
#endif

	.p2align 4
	.globl KYTY_ASM_SYMBOL(KytyFiberRestoreContext)
#if defined(__ELF__)
	.type KYTY_ASM_SYMBOL(KytyFiberRestoreContext), @function
#endif
KYTY_ASM_SYMBOL(KytyFiberRestoreContext):
	movq %rdi, %r10
	movq %rsi, %rax
	movq 72(%r10), %r11
	movq 0(%r10), %rbx
	movq 8(%r10), %rbp
	movq 16(%r10), %rdi
	movq 24(%r10), %rsi
	movq 32(%r10), %r12
	movq 40(%r10), %r13
	movq 48(%r10), %r14
	movq 56(%r10), %r15
	movq 64(%r10), %rsp
	jmp *%r11
#if defined(__ELF__)
	.size KYTY_ASM_SYMBOL(KytyFiberRestoreContext), .-KYTY_ASM_SYMBOL(KytyFiberRestoreContext)
	.section .note.GNU-stack,"",@progbits
#endif
