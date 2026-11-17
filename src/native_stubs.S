// Host entry points for Java native methods that the 32-bit code registers.
// Entry n is 8 bytes: it loads n and jumps to the common code, which saves the argument
// registers and calls boz_native_dispatch(n, regs, caller_stack). The dispatcher returns the
// integer result in x0 and leaves a float/double result in the saved d0 slot.
    .text
    .global boz_native_stubs
    .type boz_native_stubs, %function
    .p2align 3
boz_native_stubs:
    .set stub_index, 0
    .rept 512
    mov w16, #stub_index
    b boz_native_common
    .set stub_index, stub_index + 1
    .endr

boz_native_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #128
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]
    mov w0, w16
    mov x1, sp
    add x2, x29, #16
    bl boz_native_dispatch
    ldr d0, [sp, #64]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .size boz_native_stubs, . - boz_native_stubs

    .section .note.GNU-stack, "", %progbits
