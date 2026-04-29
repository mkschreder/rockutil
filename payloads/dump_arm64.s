/*
 * dump_arm64.s — AArch64 hexdump payload for Rockchip MaskROM mode
 *
 * This stub is uploaded to on-chip SRAM via USB control transfer 0x471
 * and jumped to by the MaskROM exception vector.  It hex-dumps the region
 * [param_addr, param_addr + param_len) to the UART in xxd-style format
 * so the output can be captured with a serial terminal (e.g. picocom).
 *
 * Memory layout (offsets from the start of this blob):
 *   +0x00  _start:     cache barriers + branch to _main
 *   +0x0c  param_uart  UART MMIO base (32-bit)   <- patched by host
 *   +0x10  param_addr  start address (32-bit)    <- patched by host
 *   +0x14  param_len   byte count (32-bit)       <- patched by host
 *   +0x18  save_area   32-byte register save buffer (x30/x29/x28/sp, 32-bit each)
 *   +0x38  _main:      save registers, call hexdump, restore + return
 *   +0x9c  uart_putc:  poll UART USR[1] (TX FIFO not full), then write THR
 *   +0xbc  (UDF padding word)
 *   +0xc0  hexdump:    xxd-style hex + ASCII dump, 16 bytes per output line
 *   +0x300 hex_len_zero: immediate ret when len <= 0
 *   +0x304 (GNU build-ID note, linker-generated metadata)
 *
 * Compiled from C by GCC 9.2.1 (aarch64-linux-gnu); the ELF sections were
 * stripped and packed into this flat binary by xrock / rockutil.
 */

    .arch   armv8-a
    .section .text, "ax"
    .balign 4

/* ------------------------------------------------------------------ */
/* _start -- entered by MaskROM; issue barriers before running code    */
/* ------------------------------------------------------------------ */
_start:
    isb                         // instruction synchronisation barrier
    dsb     sy                  // data synchronisation barrier (full system)
    b       _main               // skip over parameter + save areas

/* ------------------------------------------------------------------ */
/* Parameter area -- three 32-bit words patched by the host at runtime */
/* ------------------------------------------------------------------ */
param_uart: .word 0x2ad40000    // +0x0c  UART MMIO base (default placeholder)
param_addr: .word 0xffff0000    // +0x10  first address to dump
param_len:  .word 0x00000010    // +0x14  byte count (default placeholder)

/* CPU state save area -- zero-initialised, written at run time */
save_area:  .space 32           // +0x18  x30(lr) / x29(fp) / x28 / sp (32-bit each)

/* ------------------------------------------------------------------ */
/* _main -- save registers, invoke hexdump, restore and return         */
/* ------------------------------------------------------------------ */
_main:                          // offset +0x38
    // x0 = &save_area.  ADR is PC-relative with imm = save_area - _main = -32.
    adr     x0, save_area       // x0 = &save_area
    mov     x1, x30             // x1 = lr (link register)
    str     w1, [x0]            // save lr  (32-bit)
    mov     x1, x29             // x1 = frame pointer
    str     w1, [x0, #4]       // save fp  (32-bit)
    mov     x1, x28
    str     w1, [x0, #8]       // save x28 (32-bit)
    mov     x1, sp
    str     w1, [x0, #12]      // save sp  (32-bit, low word)

    // Call hexdump(addr, addr, len).
    // uart_putc reads the UART base directly from param_uart; it is not
    // passed as an argument.  Both w0 and w1 carry param_addr: w0 is the
    // printed address label, w1 is the actual memory pointer.
    ldr     w0, param_addr      // w0 = display base address
    ldr     w1, param_addr      // w1 = data pointer (same region)
    ldr     w2, param_len       // w2 = byte count
    bl      hexdump

    // Restore registers.
    mov     x1, xzr             // clear scratch
    adr     x0, save_area       // x0 = &save_area
    ldr     w1, [x0]
    mov     x30, x1             // restore lr
    ldr     w1, [x0, #4]
    mov     x29, x1             // restore fp
    ldr     w1, [x0, #8]
    mov     x28, x1             // restore x28
    ldr     w1, [x0, #12]
    mov     sp, x1              // restore sp
    mov     x0, #0              // return value = 0
    ret

/* ------------------------------------------------------------------ */
/* uart_putc(x0 = char) -- poll UART USR bit 1 (TFNF), then write THR */
/* ------------------------------------------------------------------ */
uart_putc:                      // offset +0x9c
    ldr     w1, param_uart      // w1 = UART MMIO base
.Lwait_tx:
    add     x2, x1, #0x7c      // x2 = &UART_USR (base + 0x7c; recomputed each iteration)
    ldr     w2, [x2]            // read UART_USR
    and     w2, w2, #0x2        // isolate TFNF bit (TX FIFO not full)
    cmp     w2, #0
    b.eq    .Lwait_tx           // spin while FIFO is full
    str     x0, [x1]            // write char to UART_THR (base + 0)
    ret

    .word   0x00000000          // padding / alignment (UDF #0)

/* ------------------------------------------------------------------ */
/* hexdump(w0 = addr, w1 = data_ptr, w2 = len)                         */
/*                                                                      */
/* Prints an xxd-style hex dump.  Each output line covers 16 bytes:    */
/*   <8-digit-addr>: <16 x "xx "> |<16 printable chars>\r\n           */
/*                                                                      */
/* Register allocation:                                                 */
/*   x23 / w23   current line's display address                        */
/*   x24         data base pointer for current line (advances by 16)   */
/*   w22         total byte count (= len)                               */
/*   w26         padded upper bound = ((len-1)&~15) + 16               */
/*   w25         line counter (0, 16, 32, ...)                          */
/*   x20         end pointer of current 16-byte chunk (x24+16, +16/ln) */
/*   x27         walking data pointer for hex pass (starts at x24)     */
/*   x19         walking data pointer for ASCII pass                    */
/*   w21         = w25 - lo32(x24); used as offset base for bounds     */
/* ------------------------------------------------------------------ */
hexdump:                        // offset +0xc0
    cmp     w2, #0
    b.le    hex_len_zero        // nothing to print if len <= 0

    stp     x29, x30, [sp, #-96]!
    mov     x29, sp
    stp     x25, x26, [sp, #64]
    sub     w26, w2, #1         // w26 = len - 1
    and     w26, w26, #0xfffffff0 // round down to 16-byte boundary
    add     w26, w26, #0x10    // w26 = ((len-1)&~15) + 16  (upper bound)
    stp     x19, x20, [sp, #16]
    add     x20, x1, #0x10     // x20 = data_ptr + 16 (first chunk end ptr)
    stp     x21, x22, [sp, #32]
    mov     w22, w2             // w22 = len
    mov     w25, #0             // w25 = line counter (byte offset of current line)
    stp     x23, x24, [sp, #48]
    mov     x23, x0             // x23 = display address
    mov     x24, x1             // x24 = data base pointer for current line
    str     x27, [sp, #80]
    nop

    // ===== outer loop: one iteration per 16-byte output line ==========
hex_line:
    // Print the 8-digit hex representation of the current line address.
    // Uses cmp w1, #9 + csel to choose '0'-'9' base (0x30) or 'a'-'f' base
    // (0x57) for each nibble.
    lsr     w1, w23, #28        // nibble 7 (bits 31:28)
    mov     x27, x24            // x27 = data pointer for this line
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    sub     w21, w25, w24       // w21 = line_counter - lo32(data_ptr) (offset base)
    csel    w0, w1, w0, hi      // hi (C=1 & Z=0): nibble > 9 -> letter
    bl      uart_putc
    ubfx    w1, w23, #24, #4    // nibble 6
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    ubfx    w1, w23, #20, #4    // nibble 5
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    ubfx    w1, w23, #16, #4    // nibble 4
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    ubfx    w1, w23, #12, #4    // nibble 3
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    ubfx    w1, w23, #8, #4     // nibble 2
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    ubfx    w1, w23, #4, #4     // nibble 1
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    and     w1, w23, #0xf       // nibble 0 (bits 3:0)
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi
    bl      uart_putc
    mov     w0, #':'
    bl      uart_putc
    mov     w0, #' '
    bl      uart_putc
    b       hex_loop_check      // enter hex loop from the condition check

    // ----- hex byte loop: "xx " per byte --------------------------------
hex_byte:
    ldrb    w19, [x27]          // w19 = *x27 (current byte)
    add     x27, x27, #1       // advance walking data pointer
    lsr     w1, w19, #4         // w1 = high nibble
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi      // high nibble -> ASCII digit or letter
    bl      uart_putc
    and     w1, w19, #0xf       // w1 = low nibble
    cmp     w1, #9
    add     w0, w1, #0x30
    add     w1, w1, #0x57
    csel    w0, w1, w0, hi      // low nibble -> ASCII digit or letter
    bl      uart_putc
    mov     w0, #' '
    bl      uart_putc
    cmp     x20, x27            // reached end of 16-byte chunk?
    b.eq    ascii_sep           // yes -> switch to ASCII pass
hex_loop_check:
    add     w0, w21, w27        // w0 = absolute byte index (w21 + lo32(x27))
    cmp     w0, w22             // byte index < len?
    b.lt    hex_byte            // yes -> process next byte
    // Pad remaining columns with three spaces per missing byte.
    mov     w0, #' '
    bl      uart_putc
    mov     w0, #' '
    bl      uart_putc
    add     x27, x27, #1       // advance pointer as if a byte was consumed
    mov     w0, #' '
    bl      uart_putc
    cmp     x20, x27            // reached end of chunk?
    b.ne    hex_loop_check      // no -> more padding

    // ----- ASCII pass: "|" separator then printable chars or '.' -----
ascii_sep:
    mov     x19, x24            // x19 = start of this line's data (= x24)
    mov     w0, #'|'
    bl      uart_putc
    b       ascii_loop_check    // enter ASCII loop from condition check

ascii_char:
    mov     w0, w1              // w0 = char (w1 set by ascii_loop_check below)
    bl      uart_putc
ascii_advance_check:
    add     x19, x19, #1
    cmp     x20, x19            // end of 16-byte chunk?
    b.eq    end_of_line
ascii_loop_check:
    add     w0, w21, w19        // w0 = absolute byte index for ASCII position
    cmp     w22, w0             // len > byte index? (more real bytes)
    b.le    ascii_pad           // no -> need padding
    ldrb    w1, [x19]           // w1 = byte
    mov     w0, #'.'            // default: '.' for non-printable
    sub     w2, w1, #0x20       // w2 = byte - 0x20
    cmp     w2, #0x5e           // printable: 0x20 <= byte <= 0x7e (w2 <= 0x5e)
    b.ls    ascii_char          // yes (LS = unsigned <=) -> use actual char in w1
    add     x19, x19, #1       // non-printable: advance without re-entering ascii_char
    bl      uart_putc           // print '.'
    cmp     x20, x19
    b.ne    ascii_loop_check

end_of_line:
    mov     w0, #'\r'
    bl      uart_putc
    add     w25, w25, #0x10    // advance line counter by 16
    mov     w0, #'\n'
    bl      uart_putc
    add     x23, x23, #0x10    // advance display address
    add     x20, x20, #0x10    // advance chunk end pointer
    add     x24, x24, #0x10    // advance data base pointer
    cmp     w25, w26            // reached padded upper bound?
    b.ne    hex_line            // no -> print next line
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldr     x27, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret

ascii_pad:
    mov     w0, #' '
    bl      uart_putc
    b       ascii_advance_check // re-enter after the add x19, x19, #1

hex_len_zero:                   // offset +0x300
    ret

/*
 * GNU build-ID note (from .note.gnu.build-id ELF section, 68 bytes).
 * Fields: namesz=4, descsz=20, type=3 (NT_GNU_BUILD_ID), name="GNU\0",
 * then 20 bytes of SHA1-derived build ID, then zero padding to 840 bytes.
 */
    .byte 0x04, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00  // namesz=4, descsz=20
    .byte 0x03, 0x00, 0x00, 0x00, 0x47, 0x4e, 0x55, 0x00  // type=3, "GNU\0"
    .byte 0x59, 0xc7, 0xed, 0x47, 0xc6, 0xfd, 0xa5, 0xcd  // build ID hash
    .byte 0x23, 0xf7, 0x31, 0x5a, 0x12, 0x98, 0x30, 0xfd
    .byte 0xfe, 0x28, 0xdf, 0xa0, 0x00, 0x00, 0x00, 0x00  // hash cont. + padding
    .byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    .byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    .byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    .byte 0x00, 0x00, 0x00, 0x00
