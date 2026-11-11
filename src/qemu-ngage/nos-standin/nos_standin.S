@ NOS stand-in stub for the N-Gage QD (RH-29) on the ngage board: hack H-nos-standin.
@
@ The cellular software (NOS) runs on the same ARM; its code in the flash file is scrambled
@ (docs/eka1-ngage-questions.md Q20) and we do not run it. The board copies this stub over
@ the NOS entry point (virtual 0x5149E4C4), so nosstarter.ext calls it instead of the NOS.
@
@ The stub registers the interfaces the NOS would register in the variant (Kern::Variant(),
@ ekern export 469; vtable +0xFC = Register(slot, object), Q21). Each interface is a trap
@ object: every virtual method stores its arguments in the board's NOS device (physical
@ 0x0C1F0000, virtual 0x581F0000) and returns what the device answers. The device logs each
@ call ("ngage-nos:"), so the Symbian side's expectations become questions.
@
@ Device registers: +00 call (slot << 8 | method), +04..+10 r0..r3, +14..+20 the caller's
@ first four stack arguments, +28 result (r0), +2C a TDfc to queue after the call (0 = none):
@ how an asynchronous NOS answer is delivered (Q34).
@
@ Build: src/qemu-ngage/nos-standin/build.sh (writes ../hw/arm/ngage_nos_stub.h).

        .equ    DEV,            0x581f0000
        .equ    KERN_VARIANT,   0x5032e7ac      @ ekern export 469: Kern::Variant()
        .equ    SET_POWER_MODEL,0x50336a08      @ ekern export 394: Power::SetPowerModel()
        .equ    POWER_MODEL_CTOR,0x50336694     @ ekern export 493: DPowerModel::DPowerModel()
        .equ    KERN_ADD_DFC,   0x5033f304      @ ekern export 7: Kern::Add(TDfc&)
        .equ    RAM,            0x8000c378      @ the NOS's bss (zeroed by nosstarter): free
        .equ    PM_OBJ,         RAM + 0x100     @ the power model object (data at +4: writable)
        .equ    RTC_DONE,       RAM + 0x1f0     @ the RTC hand-over was made (Q49)
        .equ    RX_DFC,         RAM + 0x200     @ ISI delivery TDfc (board: NGAGE_NOS_RX_DFC, Q50)
        .equ    RX_FLAG,        RAM + 0x1f8     @ board: an ISI delivery waits (NGAGE_NOS_RX_FLAG, Q56)
        .equ    RX_DES,         RAM + 0x220     @ TPtrC8 over RX_BUF
        .equ    RX_BUF,         RAM + 0x240     @ the frame being delivered (512 bytes)
        .equ    THREAD_WRITE,   0x5033bba0      @ ekern export 479: DThread::Write(const TAny*, const TDesC8&)
        .equ    CHANNEL_COMPLETE,0x5032f0c4     @ ekern export 64: DLogicalChannel::Complete(reqNo, reason) [V]
        .equ    NMETHODS,       64

@ Interface ids (the device's "slot" field): 0..8 = variant slots, 0x40 = power model.

        .text
        .arm
        .global _start
_start:                                         @ called by nosstarter.ext, r0 = -1
        stmfd   sp!, {r4, lr}
        ldr     r4, =DEV
        mov     r0, #0xff00                     @ event: stand-in entered
        str     r0, [r4]

        @ the ISI delivery DFC (Q50): TDfc = {0, 0, callback, argument} (export 504's layout)
        ldr     r0, =RX_DFC
        ldr     r1, =rx_dfc_fn
        str     r1, [r0, #8]
        mov     r1, #0
        str     r1, [r0, #12]

        @ variant slot 3: product data (Q21)
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip                              @ r0 = the variant
        ldr     r3, [r0]
        mov     r1, #3
        ldr     r2, =obj3
        mov     lr, pc
        ldr     pc, [r3, #0xfc]                 @ Variant::Register(3, object)
        str     r0, [r4, #4]
        ldr     r1, =0xfe03
        str     r1, [r4]                        @ event: registered (r0 = 1 if accepted)

        @ variant slots 6 and 7: NOS services the flash driver needs while formatting C:
        @ (medint.pdd 0x50381548: Get(6) then method +0x30 with 2; Q45). Slot 7 by analogy [I].
        .irp    slot, 6, 7
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r3, [r0]
        mov     r1, #\slot
        ldr     r2, =obj\slot
        mov     lr, pc
        ldr     pc, [r3, #0xfc]                 @ Variant::Register(slot, object)
        str     r0, [r4, #4]
        ldr     r1, =(0xfe00 | \slot)
        str     r1, [r4]
        .endr

        @ NOS status word (variant +0x90 via setter +0x128, Q24): ecust's start-up waits for
        @ bit 1, then faults (12) unless bit 0 is set too [V]; a running NOS sets both
        @ bit 1 = services up,
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r3, [r0]
        mov     r1, #3
        mov     lr, pc
        ldr     pc, [r3, #0x128]
        mov     r0, #3
        str     r0, [r4, #4]
        ldr     r1, =0xfd00
        str     r1, [r4]                        @ event: status set (r0 = value)

        @ power model (Q22): object in RAM, built like a derived class: base constructor
        @ (its handler queue at +16), then our vtable
        ldr     r0, =PM_OBJ
        ldr     ip, =POWER_MODEL_CTOR
        mov     lr, pc
        bx      ip
        ldr     r0, =PM_OBJ
        ldr     r1, =vtbl64
        str     r1, [r0]
        ldr     ip, =SET_POWER_MODEL
        mov     lr, pc
        bx      ip                              @ Power::SetPowerModel(object)
        ldr     r1, =0xfe40
        mov     r0, #1
        str     r0, [r4, #4]
        str     r1, [r4]

        mov     r0, #0
        ldmfd   sp!, {r4, pc}

@ ---- the NOS's exports to nosdriver.ldd (Q33) --------------------------------------
@ nosdriver.ldd imports exactly two NOS functions (export table at 0x51163EF8). The board
@ puts a jump at each NOS address to these.

@ 0x514A260C: construct the NOS side of a NOSDRIVER channel. r0 = object (8 bytes, vtable
@ pointer at +4: +0x08 delete, +0x0C DoCancel, +0x10 DoRequest, +0x14 DoControl), r1 = unit.
        .global nos_channel_ctor
nos_channel_ctor:
        str     r1, [r0]                        @ +0: the unit [I]
        ldr     r2, =vtbl65
        str     r2, [r0, #4]
        stmfd   sp!, {r0, r4, lr}
        ldr     r4, =DEV
        str     r0, [r4, #0x04]
        str     r1, [r4, #0x08]
        ldr     r1, =0xfc00
        str     r1, [r4]                        @ event: channel object constructed
        ldmfd   sp!, {r0, r4, pc}

@ 0x514A1C90: r0 = the LDD channel (DLogicalChannel), r1 = unit [I: called from DoCreate]
        .global nos_channel_open
nos_channel_open:
        mov     ip, #0
        b       slot66

@ ---- ISI delivery (Q50) ---------------------------------------------------------------
@ The board queues this DFC (device +0x2C) when a reply waits for a channel's posted receive.
@ For each delivery: copy the frame from the device, write it into the client's receive
@ descriptor and complete the channel's request.
rx_dfc_fn:
        stmfd   sp!, {r4-r8, lr}
        ldr     r4, =DEV
1:      mov     r0, #1
        str     r0, [r4, #0x30]                 @ fetch the next delivery
        ldr     r5, [r4, #0x34]                 @ the channel (DLogicalChannel), 0 = none
        cmp     r5, #0
        beq     9f
        ldr     r6, [r4, #0x38]                 @ the client's receive descriptor
        ldr     r7, [r4, #0x3c]                 @ the request number
        ldr     r8, [r4, #0x40]                 @ the frame length
        ldr     r0, =RX_BUF
        add     r1, r4, #0x100
        mov     r2, #0
2:      cmp     r2, r8
        bge     3f
        ldr     r3, [r1, r2]
        str     r3, [r0, r2]
        add     r2, r2, #4
        b       2b
3:      ldr     r2, =RX_DES
        orr     r1, r8, #0x10000000             @ TPtrC8: type 1 | length
        str     r1, [r2]
        str     r0, [r2, #4]
        ldr     r0, [r5, #0x14]                 @ the client thread
        mov     r1, r6
        ldr     ip, =THREAD_WRITE
        mov     lr, pc
        bx      ip                              @ DThread::Write(a1, frame) -> r0
        str     r0, [r4, #0x44]
        mov     r2, r0
        mov     r0, r5
        mov     r1, r7
        ldr     ip, =CHANNEL_COMPLETE
        mov     lr, pc
        bx      ip                              @ Complete(reqNo, result)
        mov     r0, #2
        str     r0, [r4, #0x30]                 @ done
        b       1b
9:      mov     r0, #0
        ldmfd   sp!, {r4-r8, pc}
        .ltorg

@ ---- trap objects -------------------------------------------------------------------
@ obj<slot>: the object (its vtable pointer); the vtable points at 8-byte trampolines,
@ one per method: ip = method, then the slot's entry adds slot << 8 and takes the common path.
        .macro  trapobj slot
        .align  2
obj\slot:
        .word   vtbl\slot
vtbl\slot:
        .set    k, 0
        .rept   NMETHODS
        .word   tramp\slot + 8 * k
        .set    k, k + 1
        .endr
tramp\slot:
        .set    k, 0
        .rept   NMETHODS
        mov     ip, #k
        b       slot\slot
        .set    k, k + 1
        .endr
slot\slot:
        .if     \slot == 3
        @ The null thread's idle hook (method 18 with r1 = 13, r3 = 0) runs in a tight loop.
        @ Answer it from RAM: device accesses mid-block make QEMU recompile, and this was most
        @ of the emulator's time (Q56). The device path is taken only while the RTC hand-over
        @ is pending or the board has flagged an ISI delivery (RX_FLAG).
        cmp     ip, #18
        bne     2f
        cmp     r1, #13
        cmpeq   r3, #0
        bne     2f
        stmfd   sp!, {r0}
        ldr     r0, =RTC_DONE
        ldr     r0, [r0]
        cmp     r0, #0
        beq     3f
        ldr     r0, =RX_FLAG
        ldr     r0, [r0]
        cmp     r0, #0
        bne     3f
        ldmfd   sp!, {r0}
        mov     r0, #0
        mov     pc, lr
        .ltorg                                  @ never reached: the return is above
3:      ldmfd   sp!, {r0}
2:
        .endif
        orr     ip, ip, #(\slot << 8)
        b       common
        .endm

        .ltorg
        trapobj 3
        trapobj 64
        trapobj 6               @ variant slot 6 (Q45)
        trapobj 7               @ variant slot 7 (Q45)
        trapobj 65              @ NOS channel object: the requests (Q33)
        trapobj 66              @ plain NOS calls (method 0 = nos_channel_open)

common:
        @ RTC hand-over (Q49): when its RTC service comes up the NOS calls rtc.dll's object
        @ (the variant's +8) with vtable +0x20 (request 1, three 64-bit times by pointer).
        @ Request 1 caches Get(3) at +0x120 and moves the RTC to state 2; without it the
        @ first alarm request (DosServer) calls through a NULL object [V]. The variant gets
        @ the RTC object after this stub ran, so do it once, on the first NOS call after.
        @ The times: the RTC's own current time (+0x10) for all three [I].
        stmfd   sp!, {r0-r4, ip, lr}
        ldr     r4, =RTC_DONE
        ldr     r0, [r4]
        cmp     r0, #0
        bne     2f
        ldr     ip, =KERN_VARIANT
        mov     lr, pc
        bx      ip
        ldr     r0, [r0, #8]                    @ the RTC object
        cmp     r0, #0
        beq     2f
        str     r0, [r4]                        @ once
        add     r2, r0, #0x10                   @ A, B, C = &rtc->time
        mov     r3, r2
        sub     sp, sp, #8
        str     r2, [sp]
        mov     r1, #1
        ldr     ip, [r0]
        mov     lr, pc
        ldr     pc, [ip, #0x20]                 @ request 1 (entry 6: GCC 2.9 vtables start with 2 zero words)
        add     sp, sp, #8
        ldr     r4, =DEV
        str     r0, [r4, #4]
        mov     r0, #0xfb00
        str     r0, [r4]                        @ event: RTC handed over (r0 = 1: state 2)
2:      ldmfd   sp!, {r0-r4, ip, lr}

        @ The exchange with the device must be atomic: a call from another thread (or the
        @ null thread's idle hook) in between would overwrite the argument registers.
        stmfd   sp!, {r4, r5, lr}
        mrs     r5, cpsr
        orr     r4, r5, #0xc0
        msr     cpsr_c, r4                      @ IRQ and FIQ off
        ldr     r4, =DEV
        str     r0, [r4, #0x04]
        str     r1, [r4, #0x08]
        str     r2, [r4, #0x0c]
        str     r3, [r4, #0x10]
        ldr     r0, [sp, #12]                   @ the caller's stack arguments
        str     r0, [r4, #0x14]
        ldr     r0, [sp, #16]
        str     r0, [r4, #0x18]
        ldr     r0, [sp, #20]
        str     r0, [r4, #0x1c]
        ldr     r0, [sp, #24]
        str     r0, [r4, #0x20]
        str     ip, [r4]                        @ the call: the device handles it here
        ldr     r0, [r4, #0x2c]                 @ a DFC to queue (asynchronous completion)?
        ldr     r4, [r4, #0x28]                 @ the result
        msr     cpsr_c, r5                      @ interrupts as before
        cmp     r0, #0
        beq     1f
        ldr     ip, =KERN_ADD_DFC
        mov     lr, pc
        bx      ip                              @ Kern::Add(dfc)
1:      mov     r0, r4
        ldmfd   sp!, {r4, r5, pc}
        .ltorg
