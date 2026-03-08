.global _start
.section .text.boot

_start:
    // Linux ARM64 Image Header (64 bytes)
    b       real_start                // code0
    .word   0                         // code1
    .quad   0x80000                   // text_offset
    .quad   __bss_end - _start        // image_size
    .quad   8                         // flags: bit 3 = text_offset is valid
    .quad   0                         // res2
    .quad   0                         // res3
    .quad   0                         // res4
    .ascii  "ARM\x64"                 // magic
    .long   0                         // res5

real_start:
    // Save FDT pointer (passed in x0 by VZLinuxBootLoader)
    mov     x19, x0

    // Install exception vector table
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     vbar_el1, x0
    isb

    // Setup stack (inside BSS)
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    // Clear BSS
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:

    // Restore FDT pointer and call kernel
    mov     x0, x19
    bl      kernel_main

    // If it returns, shutdown via PSCI
power_off:
    mov     w0, #0x0008
    movk    w0, #0x8400, lsl #16
    hvc     0
hang:
    wfi
    b       hang

// ── Exception Vector Table ──────────────────────────────────────────
// AArch64 requires 2048-byte alignment. Each entry is 128 bytes (32 insns).
// We only handle Synchronous EL1t (offset 0x000) and
// Synchronous EL1h (offset 0x200) — the two places a Data Abort from
// our kernel code would land.

.balign 2048
vectors:
    // ── Current EL, SP_EL0 ──
    // 0x000: Synchronous
    b       sync_handler
    .balign 128
    // 0x080: IRQ
    b       hang
    .balign 128
    // 0x100: FIQ
    b       hang
    .balign 128
    // 0x180: SError
    b       hang
    .balign 128

    // ── Current EL, SP_ELx ──
    // 0x200: Synchronous
    b       sync_handler
    .balign 128
    // 0x280: IRQ
    b       hang
    .balign 128
    // 0x300: FIQ
    b       hang
    .balign 128
    // 0x380: SError
    b       hang
    .balign 128

    // ── Lower EL, AArch64 ──
    // 0x400..0x480..0x500..0x580
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128

    // ── Lower EL, AArch32 ──
    // 0x600..0x680..0x700..0x780
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128

// ── Synchronous exception handler ───────────────────────────────────
// Check if it's a Data Abort (ESR_EL1.EC == 0x25 for data abort same EL).
// If so, set x0 = 0xFFFFFFFF and advance ELR_EL1 past the faulting insn.
// Otherwise, hang.
sync_handler:
    mrs     x0, esr_el1
    lsr     x1, x0, #26          // EC field
    cmp     x1, #0x25            // Data Abort (same EL)
    b.ne    hang

    // Set x0 = 0xFFFFFFFF (so ldr returns "no device")
    mov     x0, #0xFFFFFFFF

    // Skip the faulting instruction (always 4 bytes in AArch64)
    mrs     x1, elr_el1
    add     x1, x1, #4
    msr     elr_el1, x1
    eret
