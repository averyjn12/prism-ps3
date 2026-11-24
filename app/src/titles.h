#ifndef PRISM_APP_TITLES_H
#define PRISM_APP_TITLES_H
/* v0.43: a game's title by its title ID ("SLUS_000.76" -> "Loaded"), from USRDIR/titles.txt.
 * 0 = found (out holds it); -1 = not in the list, or no list. */
int title_lookup(const char *id, char *out, int n);
extern const char *titles_path;		/* the PC tests point it at their own file */
#endif
