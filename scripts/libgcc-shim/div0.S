/* GCC 4.4.1's lib1funcs.asm division routines report divide-by-zero via the legacy
   __div0 (from _dvmd_tls.o, which also defines __aeabi_idiv0/__aeabi_ldiv0 and so clashes
   with Symbian's own definitions). Forward __div0 to the Symbian-provided EABI handler
   (kernel: common/arm/gccehlp.cpp -> FAULT(); user side: drtaeabi.dll). Tail call, so the
   handler sees the original return address. */
	.text
	.arm
	.align	2
	.global	__div0
	.type	__div0, %function
__div0:
	ldr	ip, 1f
	bx	ip
	.align	2
1:	.word	__aeabi_idiv0
	.size	__div0, .-__div0
