"""Wire the prism/ module into a ported gxemu stage2 tree (idempotent).

usage: python apply_prism.py <tree-dir>

Edits gxemu/main.c, common/common.c and gxemu/Makefile. Works on a fresh port_tree.sh copy
and on a tree that carries the 2026-11-19 wiring (that wiring is removed first). Every anchor
must occur exactly once, or nothing is written.

What gets wired:
  main.c    #include "lr.h"; lr_poll() at the top of the ATAPI hook; in main(), lr_init()
            BEFORE apply_patches() and, for LR_GATE >= 5, lr_carrier(iso_fd) ahead of it (LR-G6:
            the mounted title may carry a marker) and lr_take_card() after it, which points
            Cobra's ISO variables at the file on the card when lr_iso_take() says so.
  common.c  every ufs_read(iso_fd, ...) becomes ISO_READ(iso_fd, ...); ISO_READ is ufs_read
            unless LR_GATE >= 5, where it is lr_iso_read().
  Makefile  the LR_GATE block.
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])
gx = tree / "ps2emu_stage2" / "gxemu"
common = tree / "ps2emu_stage2" / "common"
MARK = "PRISM-WIRING-4"


def load(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def once(s, anchor, new, name):
    if s.count(anchor) != 1:
        raise SystemExit("%s: anchor occurs %d times: %r" % (name, s.count(anchor), anchor))
    return s.replace(anchor, new)


# ---------------------------------------------------------------- main.c
p = gx / "main.c"
s = load(p)
if MARK in s:
    print("already applied:", p.name)
else:
    # remove the 2026-11-19 wiring if it is there
    old_inc = '#include "common.h"\n\n#ifdef PRISM_LR\n#include "lr.h"\n#endif\n'
    old_poll = "#ifdef PRISM_LR\n\tlr_poll();\n#endif\n"
    old_init = "\tapply_patches();\t\n\n#ifdef PRISM_LR\n\tlr_init();\n#endif\n"
    if old_inc in s:
        s = once(s, old_inc, '#include "common.h"\n', p.name)
        s = once(s, old_poll, "", p.name)
        s = once(s, old_init, "\tapply_patches();\t\n", p.name)
    if "PRISM_LR" in s:
        raise SystemExit("main.c carries wiring this script does not know")

    a = '#include "common.h"\n'
    s = once(s, a, a + '\n/* %s */\n#ifdef PRISM_LR\n#include "lr.h"\n#endif\n' % MARK, p.name)

    b = "\tint ret = DO_POSTCALL;\n\tuint8_t *out = get_buffer_from_lpar(outlpar);\n"
    s = once(s, b, "#ifdef PRISM_LR\n\tlr_atapi_op(cmd[0]);\n\tlr_poll();\n#endif\n" + b, p.name)
    e = "\tint tri = process_scsi_cmd_iso(cmd, out, outlen);\n"
    s = once(s, e, "#if defined(PRISM_LR) && PRISM_LR >= 5\n\tif (lr_event(cmd, out))\n\t\treturn 0;\t/* a disc change in progress: answered by lr_card.c */\n#endif\n" + e, p.name)
    d = "\tif (cmd != STORAGE_COMMAND_GET_DEVICE_SIZE)\n"
    s = once(s, d, "#ifdef PRISM_LR\n\tlr_atapi_op(0x100);\t/* the emulator asks for the disc's size */\n#endif\n" + d, p.name)

    take = '''#if defined(PRISM_LR) && PRISM_LR >= 5
static ScsiTrackDescriptor lr_track;

/* Point Cobra's ISO state at the file on the card (the carrier ISO stays open, unused).
 * Called from main() and, in menu mode, from lr_iso_read() while the emulator is running. */
void lr_take_card(void)
{
	uint64_t size;
	int cd, raw;

	int r = lr_iso_take(&size, &cd, &raw);

	if (r == 2)
	{
		/* a launcher title: it keeps running from the hard disk, but says it is a full disc */
		iso_size_sectors = LR_MENU_SECTORS;
		layer0_size = iso_size_sectors;
		return;
	}
	if (!r)
		return;

	iso_size = size;
	base_offset = 0;
	device_type = cd ? DEVICE_TYPE_PS2_CD : DEVICE_TYPE_PS2_DVD;
	is_dual_layer = 0;
	num_tracks = 0;
	tracks = NULL;
	iso_size_sectors = size / 2048;

	if (raw)
	{
		/* 2352-byte sectors: one data track, the layout Cobra gives a single-track image */
		lr_track.adr_control = 0x14;
		lr_track.track_number = 1;
		lr_track.track_start_addr = 0;
		tracks = &lr_track;
		num_tracks = 1;
		iso_size_sectors = size / 2352;
	}

	layer0_size = iso_size_sectors;

	if (device_type == DEVICE_TYPE_PS2_DVD)
		check_double_layer();
}
#endif

'''
    c = "int main(void)\n{\n"
    s = once(s, c, take + c, p.name)

    d = "\tapply_patches();\t\n"
    s = once(s, d, "#ifdef PRISM_LR\n#if PRISM_LR >= 5\n\tif (condition_iso)\n\t\tlr_carrier(iso_fd);\n#endif\n\tlr_init();\n#if PRISM_LR >= 5\n\tif (condition_iso)\n\t\tlr_take_card();\n#endif\n#endif\n\n" + d, p.name)
    p.write_text(s, encoding="utf-8", newline="\n")
    print("patched:", p.name)

# ---------------------------------------------------------------- common.c
p = common / "common.c"
s = load(p)
if MARK in s:
    print("already applied:", p.name)
else:
    n = s.count("ufs_read(iso_fd,")
    if n != 11:
        raise SystemExit("common.c: expected 11 ufs_read(iso_fd, ...) calls, found %d" % n)
    s = s.replace("ufs_read(iso_fd,", "ISO_READ(iso_fd,")
    a = '#include "scsi.h"\n'
    s = once(s, a, a + '''
/* %s: every read of the mounted disc image goes through ISO_READ */
#if defined(PRISM_LR) && PRISM_LR >= 5
#define ISO_READ	lr_iso_read
#else
#define ISO_READ	ufs_read
#endif
''' % MARK, p.name)
    p.write_text(s, encoding="utf-8", newline="\n")
    print("patched:", p.name)

# ---------------------------------------------------------------- Makefile
p = gx / "Makefile"
s = load(p)
block = """# --- PlayStation Prism S36 (LR gates). LR_GATE=0 builds stock Cobra. %s ---
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

""" % MARK
if MARK in s:
    print("already applied:", p.name)
else:
    i = s.find("# --- PlayStation Prism S36")
    if i >= 0:
        j = s.index("all: $(OUTPUT)\n")
        s = s[:i] + s[j:]
    if "LR_GATE" in s:
        raise SystemExit("Makefile carries wiring this script does not know")
    d = "all: $(OUTPUT)\n"
    s = once(s, d, block + d, p.name)
    p.write_text(s, encoding="utf-8", newline="\n")
    print("patched:", p.name)
