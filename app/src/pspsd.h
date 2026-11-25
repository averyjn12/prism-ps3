/* v0.60: PSP games straight from a card slot (pspsd.c; why and how: pspsd_core.h) */
#ifndef PRISM_APP_PSPSD_H
#define PRISM_APP_PSPSD_H

/* g: a game_t, a PSP .iso in a card slot. 0 = webMAN has mounted it from the card's sectors and the image
 * read back through /dev_bdvd equals the card's file; path_out is the .ntfs[PSPISO] file now mounted.
 * Else why says why not, and nothing is left mounted */
int pspsd_prepare(const void *g, char *path_out, int n, char *why, int wn);
int pspsd_mounted(const char *path);	/* path is what pspsd_prepare mounted: launch.c skips its own mount */
#endif
