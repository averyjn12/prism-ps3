/* busy.c: the loading screen keeps moving while the app waits (1.00) */
#ifndef BUSY_H
#define BUSY_H
int busy_init(void);		/* after gfx_init: 0, or why not (not the RSX drawing, no mutex, no thread) */
void busy_save(const void *g, const char *head, const char *title, float done, const char *what);
void busy_lend(void);
extern unsigned busy_frames;
#endif
