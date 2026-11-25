/* v0.65: which card slots give back the card's real bytes (cardcheck_core.h says how it is judged).
 * Run by games.c scan_ps2 on every slot that holds a card, at every look for games. A slot that fails is not read for
 * PS2 games, and Prism starts, copies and deletes nothing in it (Bananafuzzbuddy 11-24: an SD-to-CompactFlash adaptor slipped
 * the bytes; XMB error 80028F17 on a PS2 game, junk names in the system's own listing). */
#ifndef PRISM_CARDCHECK_H
#define PRISM_CARDCHECK_H
#include <stdio.h>
#include <ppu-types.h>

#define LR_E_BADSLOT	(-30)	/* card_state: the only card is in a slot that gives damaged data */

/* per slot, in the card reader's own LUN order: 0 Memory Stick, 1 SD, 2 CompactFlash */
extern int slot_cc[3];			/* -1 no card; else CC_OK / CC_BAD / CC_IOERR / CC_THIN */
extern char slot_cc_why[3][120];
extern int card_bad_slot;		/* the first slot that failed, or -1 */
extern int card_read_mask;		/* v0.66: bit s = the PS2 games of slot s's card are listed */

/* v0.66: the LUN to tell the PS2 helper (lun= in its trigger or a shortcut's marker) for a game in slot s: s when a card
 * sits in an earlier slot (the helper would take that one: it uses the FIRST slot that answers), else -1 (no line).
 * LUN = slot (lr/README.md: the reader's LUNs are 0 MS, 1 SD, 2 CF). */
int card_lun_for(int s);
u64 slot_storage(int s);			/* ST_DEV_MS / SD / CF */

const char *slot_name(int s);		/* "Memory Stick", "SD", "CompactFlash" */
int slot_of_dev(u64 dev);
int slot_of_path(const char *path);	/* /dev_ms, /dev_sd, /dev_cf and below; else -1 */
int slot_of_game(const void *g);	/* a game_t on a card slot (raw or through the system); else -1 */
int slot_bad(int s);			/* the check failed there (damaged data or read errors) */
/* 1 and msg filled when slot s is bad: "<doing>: the X slot is giving back damaged data ..." */
int slot_bad_msg(int s, const char *doing, char *msg, int n);
/* the slot's device is attached (fsport_attach): judge it, log a line to lg */
int cardcheck_attached(int s, FILE *lg);
#endif
