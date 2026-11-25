/* v0.67: what was played from Prism, when, and how often: for "Continue" on the home screen and the
 * "played N times" line of a game. Kept in one small text file (USRDIR/played.txt). Portable: the PC
 * tests it (host/played_test.c). A game is known by its platform and its file's name, so the same game
 * on another device keeps its history. */
#ifndef PRISM_APP_PLAYED_H
#define PRISM_APP_PLAYED_H

#define PLAYED_MAX	300
#define PLAYED_NAME	104	/* games.h game_t.name */

typedef struct
{
	unsigned char plat;
	unsigned count;
	unsigned long long seq;		/* the order of starts: the console's clock may be wrong or reset */
	unsigned long long last;	/* the console clock at the last start, seconds; 0 = not known */
	char name[PLAYED_NAME];
} played_t;

void played_file(const char *path);	/* where it is kept; before played_load() */
int  played_load(void);			/* the number of games in it (0 when there is no file yet) */
/* a game is being started: count it, make it the most recent, save. 0 = saved */
int  played_mark(int plat, const char *name, unsigned long long now);
void played_undo(void);			/* the start failed after all: the last played_mark() is taken back and saved */
const played_t *played_find(int plat, const char *name);
int  played_count(void);
const played_t *played_recent(int i);	/* 0 = the game started last, 1 the one before ...; NULL past the end */
/* "3 hours ago" for a start at last seen from now; empty when either clock value makes no sense */
void played_ago(char *out, unsigned n, unsigned long long last, unsigned long long now);

#endif
