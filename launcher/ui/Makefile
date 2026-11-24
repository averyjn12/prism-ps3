# prismui: the Prism launcher's PS2 program (see main.c).
# Built with the ps2dev toolchain in WSL distro PSBBN (~/ps2dev); see ../build_ui.sh.
# make         -> PRISMUI.ELF
# make DEMO=1  -> the pad is played by a script (for an emulator on a PC): the games tour
# make DEMO=2  -> the same, the memory card tour (copies and deletes a save: emulator only)
EE_BIN = PRISMUI.ELF
EE_OBJS = main.o
EE_INCS = -I$(GSKIT)/include -I$(PS2SDK)/ports/include
EE_LDFLAGS = -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib
EE_LIBS = -lgskit -ldmakit -lpadx -lmc -lpatches -lc -lkernel
# fio* called directly (rom0 FILEIO), not through newlib's file layer
EE_CFLAGS = -DNEWLIB_PORT_AWARE -Wall -Wextra
ifneq ($(DEMO),0)
ifneq ($(DEMO),)
EE_CFLAGS += -DDEMO=$(DEMO)
endif
endif

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
