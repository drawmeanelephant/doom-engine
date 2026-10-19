/*
 * virelai/virelai.ld — shapes DOOM.ELF to the VirelaiOS loader contract:
 *
 *   * at most three PT_LOAD program headers, ordered R+X / R / RW;
 *   * segment 0 R+X at the fixed 0x400000 text aperture with e_entry
 *     inside its initialized file bytes;
 *   * every segment's end strictly below gap_base_max 0x1000_0000;
 *   * sum(filesz) <= 32 MiB, sum(memsz) <= 64 MiB.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R+X, headers included */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.eh_frame*)
        *(.init_array*)
        *(.fini_array*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data)
        *(.data.*)
        *(.got*)
    } :data

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.exidx*)
        *(.ARM.attributes*)
    }
}
