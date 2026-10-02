#ifndef RS64_MEM_WU_H
#define RS64_MEM_WU_H

// Force-included into RecompiledFuncs. Upstream recomp.h defines MEM_W (signed
// int32) and MEM_HU/MEM_BU (unsigned half/byte) but no unsigned word variant;
// the recompiled output needs it (e.g. funcs_7 pointer comparisons).
#ifndef MEM_WU
#define MEM_WU(offset, reg) \
    (*(uint32_t*)(rdram + ((((reg) + (offset))) - 0xFFFFFFFF80000000)))
#endif

#endif // RS64_MEM_WU_H
