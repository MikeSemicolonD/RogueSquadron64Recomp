TARGET = patches.elf

# Default toolchain. Override on cmdline if your install lives elsewhere:
#   make CC="C:/mips-toolchain/bin/mips64-elf-gcc.exe" LD="C:/mips-toolchain/bin/mips64-elf-ld.exe"
# The CMake integration sets these absolute paths automatically.
CC      ?= mips64-elf-gcc
LD      ?= mips64-elf-ld

# Adapted from Zelda64Recompiled/patches/Makefile, gcc flavor.
# -G0 disables small-data optimization (we don't have a $gp setup in
# the patch ELF). -mno-abicalls avoids PIC code (relocs we don't want).
# -mips2 -mabi=32 produce N64-compatible 32-bit MIPS code even with the
# mips64-elf-gcc toolchain.
CFLAGS   := -mips2 -mabi=32 -O2 -G0 -mno-abicalls \
            -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset \
            -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused-variable \
            -Wno-missing-braces
CPPFLAGS := -nostdinc -D_LANGUAGE_C -DMIPS \
            -I dummy_headers \
            -I ../lib/N64ModernRuntime/librecomp/include
LDFLAGS  := -nostdlib -T patches.ld -T syms.ld -Map patches.map \
            --unresolved-symbols=ignore-all --emit-relocs

C_SRCS := $(wildcard *.c)
C_OBJS := $(C_SRCS:.c=.o)
C_DEPS := $(C_SRCS:.c=.d)

# Allow build with no source files yet — produces an empty patches.elf so
# the rest of the toolchain (recomp, linker) can still wire up.
ifeq ($(C_SRCS),)
EMPTY_DUMMY := __empty_patches.c
$(EMPTY_DUMMY):
	@echo "// Auto-generated placeholder so make has something to compile" > $@
	@echo "void __patches_placeholder(void) { }" >> $@
C_SRCS := $(EMPTY_DUMMY)
C_OBJS := $(EMPTY_DUMMY:.c=.o)
C_DEPS := $(EMPTY_DUMMY:.c=.d)
endif

$(TARGET): $(C_OBJS) patches.ld syms.ld
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(C_OBJS): %.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

clean:
	rm -rf $(C_OBJS) $(TARGET) $(C_DEPS) patches.map __empty_patches.c

-include $(C_DEPS)

.PHONY: clean
