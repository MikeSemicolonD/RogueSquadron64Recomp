RAMBASE = 0x80800100;

MEMORY {
    extram : ORIGIN = RAMBASE, LENGTH = 64M
    rom : ORIGIN = 0, LENGTH = 64M
}

SECTIONS {
    .ctors              : { *(.ctors*) *(.init_array*) } >extram AT >rom
    .dtors              : { *(.dtors*) } >extram AT >rom
    .text               : { *(.text*) } >extram AT >rom
    .recomp_patch       : { *(.recomp_patch*) } >extram AT >rom
    .recomp_force_patch : { *(.recomp_force_patch*) } >extram AT >rom
    .recomp_export      : { *(.recomp_export*) } >extram AT >rom
    .recomp_event       : { *(.recomp_event*) } >extram AT >rom
    .recomp_hook        : { *(.recomp_hook*) } >extram AT >rom
    .recomp_hook_return : { *(.recomp_hook_return*) } >extram AT >rom
    .rodata             : { *(.rodata*) } >extram AT >rom
    .data               : { *(.data*) } >extram AT >rom
    .bss (NOLOAD)       : { *(.bss*) *(COMMON) } >extram

    .reloc 0 : { *(.reloc*) }
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }

    /DISCARD/ : { *(*); }
}
