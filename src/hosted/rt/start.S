@ hrt process entry, thread trampoline and signal restorer (ARM state, EABI).
	.syntax unified
	.arm
	.text
	.global _start
_start:
	mov	fp, #0
	ldr	r0, [sp]		@ argc
	add	r1, sp, #4		@ argv
	add	r2, r1, r0, lsl #2
	add	r2, r2, #4		@ envp
	bic	sp, sp, #7
	bl	hrt_main
	b	hexit

@ int hclone_raw(flags, stack, ptid, tls, ctid, fn, arg)
@ the child starts on `stack` with fn/arg in callee-saved registers, calls fn(arg), exits the thread.
	.global hclone_raw
hclone_raw:
	push	{r4, r5, r6, r7, lr}
	ldr	r4, [sp, #20]		@ ctid
	ldr	r5, [sp, #24]		@ fn
	ldr	r6, [sp, #28]		@ arg
	mov	r7, #120		@ clone
	svc	#0
	cmp	r0, #0
	beq	1f
	pop	{r4, r5, r6, r7, pc}
1:	mov	fp, #0
	mov	r0, r6
	blx	r5
	mov	r7, #1			@ exit (this thread only)
	svc	#0
	b	.

	.global hrt_restorer
hrt_restorer:
	mov	r7, #173		@ rt_sigreturn
	svc	#0
