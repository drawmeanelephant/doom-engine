/*
 * virelai/sys_guest.S — the trap itself.
 *
 * vl_sys(num, a0..a5): slot number into x8, arguments into x0..x5,
 * `svc #0`, result stays in x0. AArch64 EL0 only.
 */

    .text
    .p2align 2
    .globl vl_sys
vl_sys:
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    svc     #0
    ret
