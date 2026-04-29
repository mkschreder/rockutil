/*
 * dump_arm32.s — ARM32 hexdump payload for Rockchip MaskROM mode
 *
 * This stub is uploaded to on-chip SRAM via USB control transfer 0x471
 * and jumped to by the MaskROM exception vector.  It hex-dumps the region
 * [param_addr, param_addr + param_len) to the UART in xxd-style format
 * so the output can be captured with a serial terminal (e.g. picocom).
 *
 * Memory layout (offsets from the start of this blob):
 *   +0x00  _start:     cache/TLB flush + branch to _main
 *   +0x1c  param_uart  UART MMIO base           ← patched by host
 *   +0x20  param_addr  start address to dump    ← patched by host
 *   +0x24  param_len   byte count               ← patched by host
 *   +0x28  save_area   24-byte CPU state buffer (sp/lr/CPSR/SCTLR/VBAR/SCTLR)
 *   +0x40  _main:      save CPU state, call hexdump, restore + return
 *   +0xac  uart_putc:  poll UART USR[1] (TX FIFO not full), then write to THR
 *   +0xcc  (ARM EABI attribute metadata, GCC-generated)
 *   +0xec  hexdump:    xxd-style hex + ASCII dump, 16 bytes per output line
 *   +0x294 (GCC compiler version string + second EABI attribute block)
 *
 * Compiled from C by GCC 9.2.1 (arm-linux-gnueabihf); the ELF code and
 * data sections were then stripped and packed into this flat binary by
 * xrock / rockutil.
 */

    .arch   armv7-a
    .arm
    .section .text, "ax"
    .balign 4

/* ------------------------------------------------------------------ */
/* _start — entered by MaskROM; flush stale caches before running code */
/* ------------------------------------------------------------------ */
_start:
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0      @ TLBIALL  – invalidate unified TLBs
    mcr     p15, 0, r0, c7, c5, 0      @ ICIALLU  – invalidate I-cache
    mcr     p15, 0, r0, c7, c5, 6      @ BPIALL   – flush branch-predictor cache
    mcr     p15, 0, r0, c7, c10, 4     @ DSB      – data synchronisation barrier
    mcr     p15, 0, r0, c7, c5, 4      @ ISB      – instruction synchronisation barrier
    b       _main

/* ------------------------------------------------------------------ */
/* Parameter area — three 32-bit words patched by the host at runtime  */
/* ------------------------------------------------------------------ */
param_uart: .word 0xff4c0000    @ +0x1c  UART MMIO base (default: RV1106 UART2)
param_addr: .word 0x00000000    @ +0x20  first address to dump
param_len:  .word 0x00000000    @ +0x24  byte count

/* CPU state save area — zero-initialised, written at run time by _main */
save_area:  .space 24           @ +0x28  sp / lr / CPSR / SCTLR / VBAR / SCTLR(copy)

/* ------------------------------------------------------------------ */
/* _main — save CPU context, invoke hexdump, restore and return        */
/* ------------------------------------------------------------------ */
_main:                                  @ offset +0x40
    @ r0 = &save_area.  ARM pc is 8 bytes ahead of the instruction,
    @ so pc = 0x48 here and 0x48 - 32 = 0x28 = save_area.
    sub     r0, pc, #32                 @ r0 = &save_area
    str     sp, [r0]                    @ save_area[0]  = sp
    str     lr, [r0, #4]               @ save_area[4]  = lr
    mrs     lr, CPSR
    str     lr, [r0, #8]               @ save_area[8]  = CPSR
    mrc     p15, 0, lr, c1, c0, 0      @ read SCTLR
    str     lr, [r0, #12]              @ save_area[12] = SCTLR
    mrc     p15, 0, lr, c12, c0, 0     @ read VBAR
    str     lr, [r0, #16]              @ save_area[16] = VBAR
    mrc     p15, 0, lr, c1, c0, 0      @ read SCTLR again (stored in slot 5)
    str     lr, [r0, #20]              @ save_area[20] = SCTLR (copy)

    @ Call hexdump(addr, addr, len).
    @ uart_putc fetches the UART base directly from param_uart, so it is
    @ not passed as an argument.  r0 and r1 both carry param_addr: r0 is
    @ the printed address label, r1 is the actual data pointer.
    ldr     r0, param_addr              @ r0 = display base address
    ldr     r1, param_addr              @ r1 = data pointer (same region)
    ldr     r2, param_len               @ r2 = byte count
    bl      hexdump

    @ Restore CPU state.  pc = 0x84, so 0x84 - 92 = 0x28 = save_area.
    sub     r0, pc, #92                 @ r0 = &save_area
    ldr     sp, [r0]                    @ restore sp
    ldr     lr, [r0, #4]               @ restore lr
    ldr     r1, [r0, #20]
    mcr     p15, 0, r1, c1, c0, 0      @ restore SCTLR (copy)
    ldr     r1, [r0, #16]
    mcr     p15, 0, r1, c12, c0, 0     @ restore VBAR
    ldr     r1, [r0, #12]
    mcr     p15, 0, r1, c1, c0, 0      @ restore SCTLR
    ldr     r1, [r0, #8]
    msr     CPSR_fc, r1                 @ restore CPSR
    bx      lr                          @ return to MaskROM

/* ------------------------------------------------------------------ */
/* uart_putc(r0 = char) — poll UART USR bit 1 (TFNF), then write THR  */
/* ------------------------------------------------------------------ */
uart_putc:                              @ offset +0xac
    ldr     r1, param_uart              @ r1 = UART MMIO base
.Lwait_tx:
    add     r2, r1, #0x7c              @ r2 = &UART_USR  (base + 0x7c; recomputed each iteration)
    ldr     r2, [r2]                    @ read UART_USR
    and     r2, r2, #2                  @ isolate TFNF bit (TX FIFO not full)
    cmp     r2, #0
    beq     .Lwait_tx                   @ spin while FIFO is full
    str     r0, [r1]                    @ write char to UART_THR (base + 0)
    bx      lr

/*
 * ARM EABI attributes embedded inline (32 bytes).
 * Section format: 'A' version byte, then one subsection with vendor "aeabi"
 * and the following attribute tags:
 *   Tag_CPU_arch (5)      = ARMv7-A (10)
 *   Tag_CPU_arch_profile (7) = Application ('A' = 0x41)
 *   Tag_ARM_ISA_use (8)   = yes (1)
 *   Tag_THUMB_ISA_use (9) = Thumb-2 (2)
 *   Tag_FP_arch (10)      = VFPv3 (5) — but encoded as part of toolchain attrs
 */
    .byte 0x41, 0x1e, 0x00, 0x00, 0x00, 0x61, 0x65, 0x61
    .byte 0x62, 0x69, 0x00, 0x01, 0x14, 0x00, 0x00, 0x00
    .byte 0x05, 0x37, 0x2d, 0x41, 0x00, 0x06, 0x0a, 0x07
    .byte 0x41, 0x08, 0x01, 0x09, 0x02, 0x0a, 0x05, 0x00

/* ------------------------------------------------------------------ */
/* hexdump(r0 = addr, r1 = data_ptr, r2 = len)                         */
/*                                                                      */
/* Prints an xxd-style hex dump.  Each output line covers 16 bytes:    */
/*   <8-digit-addr>: <16 × "xx "> |<16 printable chars>\r\n           */
/*                                                                      */
/* Register allocation:                                                 */
/*   r9  current line's display address                                 */
/*   r8  base data pointer (unchanged; fp indexes into it)              */
/*   r7  total byte count (= len)                                       */
/*   r6  column limit for the current 16-byte line (starts 16, +16/ln) */
/*   r5  byte-position counter for the ASCII pass                       */
/*   r4  current byte value / low nibble (reused)                       */
/*   r3  high nibble / byte scratch                                     */
/*   fp  byte offset within the current line (hex pass, monotonic)      */
/*   sl  padded upper bound = ((len-1)&~15) + 32 (loop terminator)     */
/* ------------------------------------------------------------------ */
hexdump:                                @ offset +0xec
    push    {r3, r4, r5, r6, r7, r8, r9, sl, fp, lr}
    subs    r7, r2, #0                  @ r7 = len; Z=1 if len == 0
    pople   {r3, r4, r5, r6, r7, r8, r9, sl, fp, pc} @ return now if len == 0

    @ sl = ((len-1) & ~15) + 32: pads len up to the next 16-byte boundary
    @ and adds one extra line so the outer loop's cmp sl, r6 terminates.
    sub     sl, r7, #1
    mov     r9, r0                      @ r9 = display base address
    bic     sl, sl, #15
    mov     r8, r1                      @ r8 = data pointer
    mov     r6, #16                     @ r6 = initial column limit (first line)
    add     sl, sl, #32                 @ sl = padded upper bound

    @ ===== outer loop: one iteration per 16-byte output line ===========
hex_line:
    @ Print the 8-digit hex representation of the current line address.
    @
    @ Trick for the most-significant nibble:
    @   cmn r9, #0x60000001 sets C=1 iff r9 + 0x60000001 carries out,
    @   i.e. iff r9 >= 0xa0000000, meaning the top nibble is A–F.
    @   The subsequent addls/addhi then choose the '0'–'9' or 'a'–'f' base.
    cmn     r9, #0x60000001             @ set C: top nibble 0-9 or A-F?
    sub     r5, r6, #16                 @ r5 = start offset of this line
    lsr     r0, r9, #28                 @ nibble 7 (bits 31:28)
    mov     fp, r5                      @ fp = byte offset for hex pass
    addls   r0, r0, #0x30               @ C=0 (digit 0-9):  '0' base
    addhi   r0, r0, #0x57               @ C=1 (letter A-F): 'a' base (0x61-0x0a)
    bl      uart_putc
    ubfx    r0, r9, #24, #4             @ nibble 6 (bits 27:24)
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    ubfx    r0, r9, #20, #4             @ nibble 5
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    ubfx    r0, r9, #16, #4             @ nibble 4
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    ubfx    r0, r9, #12, #4             @ nibble 3
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    ubfx    r0, r9, #8, #4              @ nibble 2
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    ubfx    r0, r9, #4, #4              @ nibble 1
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    and     r0, r9, #15                 @ nibble 0 (bits 3:0)
    cmp     r0, #9
    addls   r0, r0, #0x30
    addhi   r0, r0, #0x57
    bl      uart_putc
    mov     r0, #':'
    bl      uart_putc
    mov     r0, #' '
    bl      uart_putc
    b       hex_loop_check              @ enter hex loop from the condition

    @ ----- hex byte loop: "xx " for each byte in the current line -----
hex_byte:
    ldrb    r4, [r8, fp]                @ r4 = byte at data_ptr[fp]
    lsr     r3, r4, #4                  @ r3 = high nibble
    @ If the full byte value > 0x9f, then its upper nibble >= 0xa,
    @ so we use the 'a'–'f' base (0x57); otherwise '0'–'9' (0x30).
    cmp     r4, #0x9f
    add     r0, r3, #0x57               @ assume letter (overridden below if digit)
    and     r4, r4, #15                 @ r4 = low nibble (reclaim register)
    addls   r0, r3, #0x30               @ byte <= 0x9f: high nibble is a digit
    bl      uart_putc                   @ emit high nibble
    cmp     r4, #9
    add     r0, r4, #0x57               @ assume letter
    addls   r0, r4, #0x30               @ low nibble is a digit
    @ *** pad_continue: the "pad short lines" code below branches HERE ***
    @ On the normal path: fp is incremented AFTER the low nibble is ready
    @ in r0 but before uart_putc, so the low-nibble and trailing-space
    @ calls share the tail of this block.  On the padding path: r0 = ' '
    @ and fp is incremented as if we consumed a missing byte.
pad_continue:
    add     fp, fp, #1                  @ fp++ (advance byte index)
    bl      uart_putc                   @ emit low nibble (or 2nd padding space)
    mov     r0, #' '
    bl      uart_putc                   @ emit trailing space (or 3rd padding space)
    cmp     r6, fp                      @ fp == column limit? (16 bytes done?)
    beq     ascii_sep                   @ yes → switch to ASCII pass
hex_loop_check:
    cmp     r7, fp                      @ fp < len? (more real bytes?)
    bgt     hex_byte                    @ yes → process next byte
    @ Pad remaining columns with three spaces for each missing byte.
    mov     r0, #' '
    bl      uart_putc                   @ 1st padding space
    mov     r0, #' '
    b       pad_continue                @ emit 2nd + 3rd space via shared tail

    @ ----- ASCII pass: " |" separator then printable chars or '.' ----
ascii_sep:
    mov     r0, #'|'
    bl      uart_putc
    b       ascii_loop_check            @ enter ASCII loop from condition

ascii_char:
    ldrb    r3, [r8, r5]                @ r3 = byte at data_ptr[r5]
    mov     r0, #'.'                    @ default replacement for non-printable
    add     r5, r5, #1
    sub     r2, r3, #0x20               @ r2 = byte - 0x20
    cmp     r2, #0x5e                   @ printable: 0x20 ≤ byte ≤ 0x7e → r2 ≤ 0x5e
    movls   r0, r3                      @ in range → use the actual character
    bl      uart_putc
    cmp     r6, r5                      @ end of 16-column window?
    beq     end_of_line
ascii_loop_check:
    cmp     r7, r5                      @ more real bytes in this line?
    bgt     ascii_char
    @ Pad trailing columns with spaces for short last lines.
    mov     r0, #' '
    add     r5, r5, #1
    bl      uart_putc
    cmp     r6, r5
    bne     ascii_loop_check

end_of_line:
    mov     r0, #'\r'
    add     r6, r6, #16                 @ advance column limit to next line
    add     r9, r9, #16                 @ advance display address
    bl      uart_putc
    mov     r0, #'\n'
    bl      uart_putc
    cmp     sl, r6                      @ reached padded upper bound?
    bne     hex_line                    @ no → print next line
    pop     {r3, r4, r5, r6, r7, r8, r9, sl, fp, pc}  @ yes → return

/*
 * GCC compiler version string (from the .comment ELF section) followed
 * by a second ARM EABI attribute block (from hexdump.c's translation unit).
 * Neither block affects execution; they are present because the flat binary
 * was produced by stripping and concatenating the linked object sections.
 *
 * Decoded:
 *   "GCC: (15:9-2019-q4-0ubuntu1) 9.2.1 20191025 (release)
 *          [ARM/arm-9-branch revision 277599]\0"
 *   followed by a second "aeabi" attribute subsection.
 */
    .byte 0x47, 0x43, 0x43, 0x3a, 0x20, 0x28, 0x31, 0x35  @ "GCC: (15"
    .byte 0x3a, 0x39, 0x2d, 0x32, 0x30, 0x31, 0x39, 0x2d  @ ":9-2019-"
    .byte 0x71, 0x34, 0x2d, 0x30, 0x75, 0x62, 0x75, 0x6e  @ "q4-0ubun"
    .byte 0x74, 0x75, 0x31, 0x29, 0x20, 0x39, 0x2e, 0x32  @ "tu1) 9.2"
    .byte 0x2e, 0x31, 0x20, 0x32, 0x30, 0x31, 0x39, 0x31  @ ".1 20191"
    .byte 0x30, 0x32, 0x35, 0x20, 0x28, 0x72, 0x65, 0x6c  @ "025 (rel"
    .byte 0x65, 0x61, 0x73, 0x65, 0x29, 0x20, 0x5b, 0x41  @ "ease) [A"
    .byte 0x52, 0x4d, 0x2f, 0x61, 0x72, 0x6d, 0x2d, 0x39  @ "RM/arm-9"
    .byte 0x2d, 0x62, 0x72, 0x61, 0x6e, 0x63, 0x68, 0x20  @ "-branch "
    .byte 0x72, 0x65, 0x76, 0x69, 0x73, 0x69, 0x6f, 0x6e  @ "revision"
    .byte 0x20, 0x32, 0x37, 0x37, 0x35, 0x39, 0x39, 0x5d  @ " 277599]"
    .byte 0x00, 0x41, 0x2e, 0x00, 0x00, 0x00, 0x61, 0x65  @ "\0A.\0\0\0ae"
    .byte 0x61, 0x62, 0x69, 0x00, 0x01, 0x24, 0x00, 0x00  @ "abi\0\x01$\0\0"
    .byte 0x00, 0x05, 0x37, 0x2d, 0x41, 0x00, 0x06, 0x0a  @ EABI attrs
    .byte 0x07, 0x41, 0x08, 0x01, 0x09, 0x02, 0x0a, 0x05
    .byte 0x12, 0x04, 0x14, 0x01, 0x15, 0x01, 0x17, 0x03
    .byte 0x18, 0x01, 0x19, 0x01, 0x1a, 0x01, 0x1e, 0x02
