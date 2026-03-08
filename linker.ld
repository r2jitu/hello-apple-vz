ENTRY(_start)
SECTIONS
{
    /* Position-independent: AArch64 adrp/add is PC-relative, so the base
       doesn't matter for a flat binary.  Use 0 to avoid assumptions about
       where VZLinuxBootLoader places us in guest RAM. */
    . = 0;
    .text : {
        *(.text.boot)
        *(.text)
        *(.text.*)
    }
    
    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    . = ALIGN(0x1000);
    .data : {
        *(.data)
        *(.data.*)
    }
    
    . = ALIGN(0x1000);
    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
        . += 0x10000; /* 64KB stack */
        __stack_top = .;
        . = ALIGN(8);
        __bss_end = .;
    }
}
