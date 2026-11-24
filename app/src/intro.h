#ifndef PRISM_APP_INTRO_H
#define PRISM_APP_INTRO_H

#define INTRO_LEN	6.5f	/* seconds (v0.29: was 4.5; the finished title now holds ~2.4 s) */
#define INTRO_LEAD	1.0f	/* v0.29: seconds of black before it, while the TV comes back (v0.32: was 1.5) */

/* v0.28: draw the opening as it looks t seconds in (0 .. INTRO_LEN) */
void intro_draw(float t);
#endif
