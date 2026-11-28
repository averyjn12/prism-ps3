CC			:= ppu-gcc
CXX			:= ppu-g++
LD			:= ppu-ld 
OBJCOPY		:= ppu-objcopy
OBJDUMP		:= ppu-objdump
AR 			:= ppu-ar
STRIP		:= ppu-strip
INCLUDE		=  ../common ../../ps2emu/include ../../lv1/include ../../debug/include
INCLUDE		+= $(PS3DEV)/ppu/ppu/include
LIBSDIR     =
LIBS 		=
BUILD_TYPE 	= release
OUTPUT		= ps2hwemu_stage2.bin

CFLAGS = -ffunction-sections -fdata-sections -O2 -m64 -fno-builtin -fno-exceptions \
			   -O2 -Wall -Wno-strict-aliasing $(foreach dir,$(INCLUDE),-I$(dir)) \
			   -DPS2EMU -DPS2HWEMU -DFIRMWARE_CEX -ffreestanding 
CFLAGS += --std=gnu99 -mno-pltseq -fno-strict-aliasing -mno-altivec -mno-vsx -msoft-float

ifeq ($(BUILD_TYPE), debug)
CFLAGS += -DDEBUG
endif

ifeq ($(BUILD_TYPE), release)
endif

LDFLAGS= -T ldscript.ld -nostartfiles -nostdlib -nodefaultlibs -Wl,-static -Wl,-s -L. $(foreach dir,$(LIBSDIR),-L$(dir)) $(LIBS) \
	-Wl,--gc-sections -Wl,-Map=ps2hwemu_stage2.map

OBJS = start.o crt0.o main.o ../../ps2emu/src/libc.o ../../ps2emu/src/libc_c.o ../../ps2emu/src/memory.o ../../ps2emu/src/io.o ../../ps2emu/src/patch.o 

ifeq ($(BUILD_TYPE), debug)
OBJS += ../../lv1/src/hvcall.o ../../lv1/src/device.o ../../debug/src/debug.o ../../debug/src/printf.o 
endif

# --- PlayStation Prism S36 (LR gate 5 on ps2_emu). LR_GATE=0 builds stock Cobra. PRISM-WIRING-HW ---
LR_GATE ?= 0
ifneq ($(LR_GATE),0)
ifneq ($(LR_GATE),5)
$(error hwemu: only LR_GATE=5 is supported)
endif
CFLAGS += -DPRISM_LR=$(LR_GATE) -I../prism -fno-delete-null-pointer-checks
# 2026-11-24 (A01 kit): no alignment padding - layout only; -Os ICEs gcc 13 (port_tree.sh #3)
CFLAGS += -falign-functions=4 -falign-jumps=4 -falign-loops=4 -falign-labels=4
OBJS += ../prism/lr_main.o ../prism/lr_log.o ../prism/lr_probe.o
OBJS += ../prism/lr_hc.o ../prism/lr_usb.o ../prism/lr_fs.o ../prism/lr_card.o
endif

all: $(OUTPUT)
	@cp $(OUTPUT) ../../../BIN
	@rm -f *.o *.elf *.map ../../ps2emu/src/*.o ../../lv1/src/*.o ../../debug/src/*.o

objdump: $(OUTPUT)
	$(OBJDUMP) -D -EB -b binary -m powerpc:common64 $(OUTPUT)

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	
ps2hwemu_stage2.elf: $(OBJS) ldscript.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo -e "\e[1;32mCleaning hwemu...\e[0m"
	@rm -f *.o *.elf *.bin *.map ../../ps2emu/src/*.o ../../lv1/src/*.o ../../debug/src/*.o
