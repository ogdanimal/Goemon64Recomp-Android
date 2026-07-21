# Output definitions
TARGET_NAME := patches.elf
BUILD_DIR   := build
TARGET      := $(BUILD_DIR)/$(TARGET_NAME)

# Tools
CC      ?= clang
LD      ?= ld.lld
NM      ?= llvm-nm
CMAKE   ?= cmake

# Flags
CFLAGS   := -target mips -mips2 -mabi=32 -O2 -G0 \
            -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
            -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations \
            -fno-builtin-memset -Wall -Wextra \
            -Wno-incompatible-library-redeclaration -Wno-unused-parameter \
            -Wno-unknown-pragmas -Wno-unused-variable \
            -Wno-missing-braces -Wno-unsupported-floating-point-opt

CPPFLAGS := -nostdinc -D_LANGUAGE_C -DMIPS -DNDEBUG \
            -I ../lib/mnsg/include \
            -I ../lib/mnsg/libultra/include \
            -I ../lib/mnsg/libultra/include/PR \
            -I ../lib/rt64/include

ASFLAGS  := -target mips -mips2 -mabi=32 -G0 \
            -mno-abicalls -mno-check-zero-division \
            -x assembler-with-cpp -modd-spreg

LDSCRIPTS := patches.ld syms.ld
LDFLAGS  := -nostdlib $(addprefix -T ,$(LDSCRIPTS)) \
            -Map $(BUILD_DIR)/patches.map \
            --unresolved-symbols=ignore-all --emit-relocs

# Source processing
C_SRCS := $(wildcard *.c)
C_OBJS := $(C_SRCS:%.c=$(BUILD_DIR)/%.o)
C_DEPS := $(C_OBJS:.o=.d)

# Assembly processing
S_SRCS := $(wildcard *.s)
S_OBJS := $(addprefix $(BUILD_DIR)/, $(S_SRCS:.s=.o))

# Default target
all: $(TARGET)

# Linker Rule
$(TARGET): $(C_OBJS) $(S_OBJS) $(LDSCRIPTS)
	@$(CMAKE) -E echo "Linking $@"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(LD) $(C_OBJS) $(S_OBJS) $(LDFLAGS) -o $@
	@# --unresolved-symbols=ignore-all lets a mistyped host export (one referenced
	@# from patch C but missing from syms.ld) link as address 0 instead of failing,
	@# so it silently becomes a call into the wrong host function at runtime. Every
	@# recomp_* shim is DEFINED in syms.ld, so an *undefined* recomp_* symbol here
	@# is unambiguously that typo. Game symbols (func_/D_/...) and libultra symbols
	@# are resolved by name downstream by N64Recomp and must stay undefined, so they
	@# are deliberately not flagged.
	@bad=`$(NM) $(TARGET) | awk '$$1=="U"||$$2=="U"{print $$NF}' | grep '^recomp_' || true`; \
	if [ -n "$$bad" ]; then \
		$(CMAKE) -E echo "ERROR: undefined recomp_* host export(s) — add to syms.ld:"; \
		for s in $$bad; do $(CMAKE) -E echo "  $$s"; done; \
		exit 1; \
	fi

# Compiler Rule
$(BUILD_DIR)/%.o: %.c
	@$(CMAKE) -E echo "Compiling $<"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

$(BUILD_DIR)/%.o: %.s
	@$(CMAKE) -E echo "Compiling $<"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(CC) $(ASFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

clean:
	@$(CMAKE) -E echo "Cleaning build directory..."
	$(CMAKE) -E remove_directory $(BUILD_DIR)

-include $(C_DEPS)

.PHONY: all clean