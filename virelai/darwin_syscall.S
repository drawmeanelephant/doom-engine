/*
 * virelai/darwin_syscall.S — host-side raw BSD syscalls for the mock
 * kernel (macOS/aarch64 builds only; never linked into DOOM.ELF).
 *
 * darwin_sys6(num, a0..a5): syscall number into x16, args x0..x5,
 * `svc #0x80`. Darwin returns carry-clear with the result in x0, or
 * carry-set with errno in x0 — this wrapper returns -errno so it reads
 * exactly like the guest ABI.
 */

    .text
    .p2align 2
    .globl _darwin_sys6
_darwin_sys6:
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    svc     #0x80
    b.cc    1f
    neg     x0, x0
1:  ret

/*
 * Host executable entry. macOS hands a static binary its stack with argc
 * at [sp], argv pointers immediately after. We pass argc/argv to
 * hosttest_main and exit with its result.
 */
    .p2align 2
    .globl _start
_start:
    mov     x29, xzr
    mov     x30, xzr
    ldr     x0, [sp]
    add     x1, sp, #8
    bl      _hosttest_main
    mov     x16, #1         /* SYS_exit */
    svc     #0x80
