/* v0.65: does a card slot give back the card's real bytes? (Bananafuzzbuddy 11-24: a microSD in an SD-to-CompactFlash adaptor
 * read with its bytes slipped by half-bytes; the system's own FAT driver listed junk names, a PS2 game failed with XMB
 * error 80028F17, and a write through it would have damaged the card.)
 *
 * No reference copy of the card is needed. Two kinds of test, both read-only:
 *   1. The same sectors read three ways must agree: in one transfer, one sector at a time, and from a second transfer
 *      that starts halfway in. An adaptor that loses bits in a long transfer gives different bytes for the same
 *      sectors depending on where they sit in it. A sound reader gives the same bytes every way.
 *   2. Things the card carries twice or with a checksum: the exFAT boot region's checksum and its backup copy, the
 *      FAT32 backup boot sector, the FAT32 FSInfo signatures, and the FAT32 FAT against its mirror.
 * Pure C with a read callback: tested on the PC by host/cardcheck_test.py against real mkfs volumes and simulated
 * bad adaptors. */
#ifndef PRISM_CARDCHECK_CORE_H
#define PRISM_CARDCHECK_CORE_H
#include <stdint.h>

#define CC_SPAN		64			/* sectors in one test transfer: the PS2 helper's own read size (LR_DATA_SZ) */
#define CC_BUF		(CC_SPAN * 512)		/* bytes in each of the two buffers cc_card needs */

#define CC_OK		0	/* every test agreed */
#define CC_BAD		1	/* the slot gave different bytes for the same sectors, or a copy / checksum did not match */
#define CC_IOERR	2	/* a read failed */
#define CC_THIN		3	/* nothing failed, but too little on the card to judge by */

/* read n sectors from lba into buf; 0 = read */
typedef int (*cc_read)(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf);

/* the whole card: sector 0, then each FAT32 / exFAT volume it names (or the volume at sector 0). total = the card's
 * sector count. a and b: CC_BUF bytes each. why: one line for the log and the player. judged (may be NULL): how many
 * tests ran. */
int cc_card(cc_read rd, void *ctx, uint32_t total, uint8_t *a, uint8_t *b, char *why, int wn, int *judged);

/* the exFAT boot checksum of sectors 0..10 (spec 3.4: bytes 106, 107 and 112 left out) */
uint32_t cc_exfat_sum(const uint8_t *s11);
#endif
