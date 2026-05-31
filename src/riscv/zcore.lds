/*
 * zcore.lds
 *
 * Linker script for DOOM on Z-Core
 *
 * SDRAM memory map (64 MB total: 0x10000000 – 0x13FFFFFF):
 *
 *   0x10000000  Code + .data + .bss
 *   _heap_start Heap (grows up via sbrk, guarded by __heap_limit)
 *   0x11FF0000  Stack top  (grows down, 64 KB → 0x11FE0000)
 *   0x12010000  WAD file   (loaded by bootloader, ~4 MB)
 *   0x12410000+ Free SDRAM
 *
 * Stack (11FE0000..11FF0000) and WAD (12010000+) are separated by a
 * 64 KB guard region at 0x12000000–0x1200FFFF to catch stack or WAD
 * overruns before they corrupt each other.
 */

MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x10000000, LENGTH = 0x02000000  /* 32 MB */
}

ENTRY(_start)

SECTIONS {
    __stacktop   = 0x11FF0000;
    __heap_limit = 0x11FE0000; /* 64KB stack below __stacktop */

    .text :
    {
        . = ALIGN(4);
        *(.text.start)
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        *(.srodata)
        *(.srodata*)
        . = ALIGN(4);
        _etext = .;
    } > SDRAM

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.sdata)
        *(.sdata*)
        . = ALIGN(4);
        _edata = .;
    } > SDRAM

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(.sbss)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SDRAM

    .heap :
    {
        . = ALIGN(4);
        _heap_start = .;
    } > SDRAM
}
