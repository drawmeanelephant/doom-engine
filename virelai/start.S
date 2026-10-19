/*
 * virelai/start.S — guest entry.
 *
 * The kernel enters at e_entry with x0 = argc, x1 = the packed argv
 * block VA (8 slots x 256 B), SP_EL0 = the mapped stack top, SPSR =
 * EL0t. vl_entry() (doomgeneric_virelai.c) takes it from there.
 */

    .text
    .p2align 2
    .globl _start
_start:
    mov     x29, xzr
    mov     x30, xzr
    bl      vl_entry
    mov     x8, #3          /* VL_SYS_EXIT — vl_entry never returns */
    svc     #0
