#ifndef PRISM_APP_ICONS_H
#define PRISM_APP_ICONS_H

enum { ICON_PS1, ICON_PS2, ICON_PS3, ICON_GEAR, ICON_PSP };

/* the HOME tiles' pictures; t = seconds (animation), chosen = the tile under the cursor */
void console_icon(int which, float cx, float cy, float s, int al, float t, int chosen);
#endif
