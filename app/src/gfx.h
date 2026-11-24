/* Display for the Prism app: the CPU draws into a 1280 x 720 main-memory canvas (fb, 0x00RRGGBB);
 * gfx_present() copies it (scaled when the TV runs another resolution) into one of two RSX
 * display buffers and flips at the next vsync. All drawing is in ui.c. */
#ifndef PRISM_APP_GFX_H
#define PRISM_APP_GFX_H
#include <ppu-types.h>

#define CW	1280
#define CH	720

extern u32 *fb;			/* CW x CH */

int  gfx_init(void);		/* 0 or a negative step number */
void gfx_present(void);
void gfx_end(void);

#endif
