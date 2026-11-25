/* Stand-ins for the PC renderer (render.sh): the parts of v0.55-v0.61 that settings.c and screens.c call but
 * that need the console (covers.c, launch.c, setup.c). RENDER_PSPL picks the launchers a shot shows. */
#include <stdlib.h>
#include "covers.h"

int render_pspl_have = 3, render_pspl_default = 1;

void covers_open(void) {}
void covers_draw(void) {}
int covers_pad(unsigned pressed) { (void)pressed; return -1; }
int psp_launchers_installed(void) { return render_pspl_have; }
int psp_launcher_for(const void *g) { (void)g; return render_pspl_default; }
int psp_launcher_set(const char *key, int pref) { (void)key; if (pref) render_pspl_default = pref; return 0; }
int wm_shortcut_has(const void *g) { (void)g; return 0; }
