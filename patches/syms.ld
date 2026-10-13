/* Host functions the patches call, as manual patch symbols: zero-sized absolute addresses in 0x8F000000-0x8FFFFFFF that N64Recomp turns into direct calls and librecomp resolves through register_manual_patch_symbols. */
rs64_host_puts = 0x8F000000;
rs64_host_ram_ok = 0x8F000004;
