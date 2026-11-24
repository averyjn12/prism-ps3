/* The stage2's card reader (lr_fs.c, compiled unchanged from lr/tree) on another block device.
 * fsport_ps3.c: the console's raw SD device (lv2 sys_storage_*). fsport_host.c: an image file on
 * the PC, for tests against the Linux-made cards. Both provide what lr_fs.c takes from lr.h:
 * lr_dma, lr_blk_count, lr_blk_read, lr_log, lr_fmt. */
#ifndef PRISM_APP_FSPORT_H
#define PRISM_APP_FSPORT_H
#include <stdio.h>
#include "lr.h"

extern FILE *fsport_log;	/* lr_log() writes here (flushed per line) */

#endif
