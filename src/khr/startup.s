@ EKA2 DLL entry point for src/khr's DLLs (as kernel/eka/euser/epoc/arm/uc_dll.cia): the version
@ marker, a jump to the body, and the word the loader replaces with the code segment's unique ID
@ (offset 12; Dll::Tls in eglos.cpp reads it). The body runs static constructors on process attach.
	.text
	.arm
	.align 2
	.global _E32Dll
	.type _E32Dll, %function
_E32Dll:
	tst	pc, #0			@ EKA2 entry point version identifier
	ldr	r12, 1f
	bx	r12
	.word	0			@ code seg unique ID (loader fills in), entry + 12
1:	.word	khr_dll_body
