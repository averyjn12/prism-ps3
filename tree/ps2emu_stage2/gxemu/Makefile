CC			:= ppu-gcc
CXX			:= ppu-g++
LD			:= ppu-ld 
OBJCOPY		:= ppu-objcopy
OBJDUMP		:= ppu-objdump
AR 			:= ppu-ar
STRIP		:= ppu-strip
INCLUDE		=  ../common ../../ps2emu/include ../../lv2/include ../../lv1/include ../../debug/include
INCLUDE		+= $(PS3DEV)/ppu/ppu/include
LIBSDIR     =
LIBS        =
BUILD_TYPE  = release
OUTPUT		= ps2gxemu_stage2.bin

CFLAGS = -ffunction-sections -fdata-sections -O2 -m64 -fno-builtin -fno-exceptions \
			   -O2 -Wall -Wno-strict-aliasing $(foreach dir,$(INCLUDE),-I$(dir)) \
			   -DPS2EMU -DPS2GXEMU -DFIRMWARE_CEX -ffreestanding 
CFLAGS += --std=gnu99 -mno-pltseq -fno-strict-aliasing -mno-altivec -mno-vsx -msoft-float

ifeq ($(BUILD_TYPE), debug)
CFLAGS += -DDEBUG
endif

ifeq ($(BUILD_TYPE), release)
endif

LDFLAGS= -T ldscript.ld -nostartfiles -nostdlib -nodefaultlibs -Wl,-static -Wl,-s -L. $(foreach dir,$(LIBSDIR),-L$(dir)) $(LIBS) \
	-Wl,--gc-sections -Wl,-Map=ps2gxemu_stage2.map

OBJS = start.o crt0.o main.o ../../ps2emu/src/libc.o ../../ps2emu/src/libc_c.o ../../ps2emu/src/memory.o ../../ps2emu/src/io.o \
	../../ps2emu/src/vuart.o ../../ps2emu/src/patch.o 

ifeq ($(BUILD_TYPE), debug)
OBJS += ../../lv1/src/hvcall.o ../../lv1/src/device.o ../../debug/src/debug.o ../../debug/src/printf.o 
endif

# --- PlayStation Prism S36 (LR gates). LR_GATE=0 builds stock Cobra. PRISM-WIRING-3 ---
LR_GATE ?= 0
ifneq ($(LR_GATE),0)
CFLAGS += -DPRISM_LR=$(LR_GATE) -I../prism -fno-delete-null-pointer-checks
OBJS += ../prism/lr_main.o ../prism/lr_log.o ../prism/lr_probe.o
ifeq ($(LR_GATE),5)
# gate 5 makes no hypercall at all: no UDP mirror, no lv1 wrappers (and it needs the room)
OBJS += ../prism/lr_hc.o ../prism/lr_usb.o ../prism/lr_fs.o ../prism/lr_card.o
else
OBJS += ../prism/lr_net.o ../../lv1/src/hvcall.o ../../lv1/src/device.o
endif
ifeq ($(LR_GATE),3)
OBJS += ../prism/lr_ehci.o
endif
endif

all: $(OUTPUT)
	@cp $(OUTPUT) ../../../BIN	
	@rm -f *.o *.elf *.map ../../ps2emu/src/*.o ../../lv1/src/*.o ../../debug/src/*.o

objdump: $(OUTPUT)
	$(OBJDUMP) -D -EB -b binary -m powerpc:common64 $(OUTPUT)

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	
ps2gxemu_stage2.elf: $(OBJS) ldscript.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo -e "\e[1;32mCleaning gxemu...\e[0m"
	@rm -f *.o *.elf *.bin *.map ../../ps2emu/src/*.o ../../lv1/src/*.o ../../debug/src/*.o
