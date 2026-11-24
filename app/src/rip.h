#ifndef PRISM_APP_RIP_H
#define PRISM_APP_RIP_H
#include "games.h"
/* Copy the PS3 game disc in the drive to <a device the player picks>/GAMES/<TITLE_ID>. */
int rip_disc(unsigned (*pad)(void), char *msg, int msg_n);
/* v0.15: copy a PS1 / PS3 game from its storage device to another one the player picks. */
int copy_game(unsigned (*pad)(void), int plat, const game_t *g, char *msg, int msg_n);
/* v0.30: a PSP game on the SD card runs from a copy on the hard disk; 0 = path holds the copy's
 * path (made now, or kept from before), else msg says why not (-9 = the player stopped it) */
int psp_card_copy(unsigned (*pad)(void), const game_t *g, char *path, int path_n, char *msg, int msg_n);
#endif
