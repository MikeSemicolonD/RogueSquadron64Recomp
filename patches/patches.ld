/* Patches ELF layout. Only the code sections and its symbols survive; everything else is discarded. */
/* librecomp loads the patches at 0x80801000. The end stops short of the host-only tables (F5 scratch 0x80A00000, terrain grid 0x80B00000, co-op relocation 0x80B3xxxx). */
RAMBASE = 0x80801000;
PATCH_RAM_END = 0x80A00000;

MEMORY {
    extram : ORIGIN = RAMBASE, LENGTH = 64M
    rom : ORIGIN = 0, LENGTH = 64M
}

SECTIONS {
    .ctors         : { *(.ctors*) *(.init_array*)                } >extram AT >rom
    .dtors         : { *(.dtors*)                                } >extram AT >rom
    .text          : { *(.text*)                                 } >extram AT >rom
    .recomp_patch  : { *(.recomp_patch*) *(.recomp_force_patch*) } >extram AT >rom
    .recomp_export : { *(.recomp_export*)                        } >extram AT >rom
    .recomp_event  : { *(.recomp_event*)                         } >extram AT >rom
    .rodata        : { *(.rodata*)                               } >extram AT >rom
    .data          : { *(.data*)                                 } >extram AT >rom
    .bss  (NOLOAD) : { *(.bss*)  *(COMMON)                       } >extram
    ASSERT(. <= PATCH_RAM_END, "Maxed out recomp extra ram")

    .reloc 0 : { *(.reloc*) }
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }

    /DISCARD/ : { *(*); }
}
