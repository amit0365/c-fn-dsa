Q ?= @

BINARY ?= f2_selector_clear
VARIANT ?= clear
MODE ?= full_dispatch
SINK_SHARE ?= 0
OPT ?= -Os

ELMO_DIR := ../../../external/ELMO
ARM_TOOLCHAIN := ../../../external/arm-toolchain/arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi/bin
PREFIX ?= $(ARM_TOOLCHAIN)/arm-none-eabi

CC := $(PREFIX)-gcc
LD := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
OBJDUMP := $(PREFIX)-objdump

ARCH_FLAGS := -mthumb -mcpu=cortex-m0 -msoft-float
CFLAGS := $(OPT) -g -std=c99 -Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS += -Wmissing-prototypes -Wredundant-decls
CFLAGS += -fno-common -ffunction-sections -fdata-sections
CPPFLAGS := -MD -Wall -Wundef
LDFLAGS := --static -nostartfiles -Tf2_selector.ld
LDFLAGS += -Wl,-Map=$(BINARY).map -Wl,--gc-sections
LDLIBS := -Wl,--start-group -lc -lgcc -lnosys -Wl,--end-group

VECTOR_OBJ := $(ELMO_DIR)/Examples/FixedvsRandom/MBedAES/vector.o

ifeq ($(VARIANT),clear)
VARIANT_ID := 0
ORDER_FLAG := -DF2_SELECTOR_ORDER=0
DISPATCH_SRC := dispatch_clear.c
else ifeq ($(VARIANT),masked_d1)
VARIANT_ID := 1
ORDER_FLAG := -DF2_SELECTOR_ORDER=1
DISPATCH_SRC := dispatch_masked_d1.c
else ifeq ($(VARIANT),masked_d2)
VARIANT_ID := 2
ORDER_FLAG := -DF2_SELECTOR_ORDER=2
DISPATCH_SRC := dispatch_masked_d2.c
else ifeq ($(VARIANT),masked_d3)
VARIANT_ID := 3
ORDER_FLAG := -DF2_SELECTOR_ORDER=3
DISPATCH_SRC := dispatch_masked_d3.c
else ifeq ($(VARIANT),masked_d4)
VARIANT_ID := 4
ORDER_FLAG := -DF2_SELECTOR_ORDER=4
DISPATCH_SRC := dispatch_masked_d4.c
else
$(error unknown VARIANT=$(VARIANT))
endif

ifeq ($(MODE),load_only)
TRACE_MODE_ID := 0
else ifeq ($(MODE),eq_only)
TRACE_MODE_ID := 1
else ifeq ($(MODE),select_only)
TRACE_MODE_ID := 2
else ifeq ($(MODE),open_only)
TRACE_MODE_ID := 3
else ifeq ($(MODE),full_dispatch)
TRACE_MODE_ID := 4
else ifeq ($(MODE),bit_linear_only)
TRACE_MODE_ID := 5
else ifeq ($(MODE),isw_and_only)
TRACE_MODE_ID := 6
else ifeq ($(MODE),eq_no_sink)
TRACE_MODE_ID := 7
else ifeq ($(MODE),eq_one_share_sink)
TRACE_MODE_ID := 8
else ifeq ($(MODE),isw_and_one_share_sink)
TRACE_MODE_ID := 9
else ifeq ($(MODE),isw_and_d2_asm)
TRACE_MODE_ID := 10
else ifeq ($(MODE),eq_d2_asm)
TRACE_MODE_ID := 11
else
$(error unknown MODE=$(MODE))
endif

OBJS := elmo_target_$(VARIANT)_$(MODE)_s$(SINK_SHARE).o dispatch_$(VARIANT).o
ifneq (,$(filter $(MODE),isw_and_d2_asm eq_d2_asm))
OBJS += d2_isw_asm.o
endif

all: $(BINARY).bin $(BINARY).list

$(BINARY).elf: $(OBJS) f2_selector.ld
	$(Q)$(LD) $(LDFLAGS) $(VECTOR_OBJ) $(ARCH_FLAGS) $(OBJS) $(LDLIBS) -o $@

$(BINARY).bin: $(BINARY).elf
	$(Q)$(OBJCOPY) -Obinary $< $@

$(BINARY).list: $(BINARY).elf
	$(Q)$(OBJDUMP) -S $< > $@

elmo_target_$(VARIANT)_$(MODE)_s$(SINK_SHARE).o: elmo_target.c mask_gadgets.h
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(ARCH_FLAGS) $(ORDER_FLAG) -DF2_SELECTOR_VARIANT=$(VARIANT_ID) -DF2_SELECTOR_TRACE_MODE=$(TRACE_MODE_ID) -DF2_SELECTOR_SINK_SHARE=$(SINK_SHARE) -c $< -o $@

dispatch_$(VARIANT).o: $(DISPATCH_SRC) mask_gadgets.h
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(ARCH_FLAGS) $(ORDER_FLAG) -c $(DISPATCH_SRC) -o $@

d2_isw_asm.o: d2_isw_asm.S
	$(Q)$(CC) $(CPPFLAGS) $(ARCH_FLAGS) -c $< -o $@

clean:
	$(Q)$(RM) *.o *.d *.elf *.bin *.list *.map
