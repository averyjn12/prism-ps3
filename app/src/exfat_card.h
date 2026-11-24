#ifndef PRISM_APP_EXFAT_CARD_H
#define PRISM_APP_EXFAT_CARD_H
#include <stdio.h>
#include "exfat_w.h"
/* v0.37: the SD card as an exw_dev (exfat_card.c); close it when done */
int  exc_open(exw_dev *d, char *why, int wn);
void exc_close(void);
#endif
