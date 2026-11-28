/* v0.82: PS3 game ISOs in a card slot, played from the card through webMAN's rawseciso (ps3sd.c) */
#ifndef PRISM_APP_PS3SD_H
#define PRISM_APP_PS3SD_H
/* 1 = a PS3 ISO in a card slot (either part of the card): ps3sd_prepare() mounts it, not launch.c's mount() */
int ps3sd_card_game(const void *g);
/* the game's ISO checked on the card, its sector list written, mounted by webMAN, /dev_bdvd checked. 0, or -1 (why) */
int ps3sd_prepare(const void *g, char *why, int wn);
#endif
