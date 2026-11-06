@ Minimal EKA2 EXE entry point for devagent (replaces SDK eexe.lib, which is an RVCT
@ object needing armlink region symbols). Same entry protocol as the 9.x uc_exe.cia:
@ r4 = entry reason (0 process init, 1 thread init, 4 exception), sp -> parameter block.
@ Word at entry+12 is overwritten by the loader with the code segment unique ID.
	.text
	.arm
	.align 2
	.global _E32Startup
	.type _E32Startup, %function
_E32Startup:
	tst	pc, #0			@ EKA2 entry point version identifier
	cmp	r4, #1			@ KModuleEntryReasonThreadInit
	b	1f
	.word	0			@ code seg unique ID (loader fills in)
1:	movls	r0, r4			@ aNotFirst
	movls	r1, sp			@ -> SStdEpocThreadCreateInfo
	bls	DaRunThread
	cmp	r4, #4			@ KModuleEntryReasonException
	bne	DaInvariant
	mov	r0, sp
	ldr	r4, [sp, #16]		@ saved CPSR
	tst	r4, #0x20		@ THUMB?
	ldrne	r1, [sp, #80]
	orrne	r1, r1, #1
	strne	r1, [sp, #80]
	mov	r11, sp
	bic	sp, sp, #4
	bl	DaHandleException
	add	sp, r11, #16
	ldr	r4, [sp], #4
	bic	r4, r4, #0x20
	msr	cpsr_f, r4
	ldmia	sp, {r0-r15}
	.size _E32Startup, .-_E32Startup
