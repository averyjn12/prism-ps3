/* The PS2 launcher's drawing primitives (gsKit there) for the PS3 app, in the PS2 program's own
 * 640 x 448 coordinates, drawn in software onto the 1280 x 720 canvas (x 720/448, centred).
 * Colours are RGBA with the GS convention: alpha 0x80 is opaque. So launcher/ui code ports
 * nearly line for line. */
#ifndef PRISM_APP_UI_H
#define PRISM_APP_UI_H
#include <ppu-types.h>

typedef u32 col_t;
#define RGBA(r, g, b, a)	((col_t)(((u32)(r) << 24) | ((u32)(g) << 16) | ((u32)(b) << 8) | ((u32)(a) & 0xFF)))

/* the launcher's palette (launcher/ui/main.c) */
#define INK	RGBA(0xE9, 0xED, 0xFF, 0x80)
#define DIM	RGBA(0x8D, 0x96, 0xBD, 0x80)
#define FAINT	RGBA(0x4A, 0x54, 0x82, 0x80)
#define GLOW	RGBA(0x7F, 0xE3, 0xFF, 0x80)
#define AMBER	RGBA(0xFF, 0xC4, 0x6B, 0x80)
#define LINE	RGBA(0x25, 0x30, 0x5F, 0x80)
#define PANEL	RGBA(0x0D, 0x14, 0x33, 0x74)
#define VOID	RGBA(0x06, 0x0A, 0x1A, 0x80)

#define F_SMALL	0
#define F_BODY	1
#define F_BOLD	2
#define F_HEAD	3
#define DOT	"\x7f"		/* the middle dot in the font */

#define LX0	(-78.2f)	/* the logical x at the canvas's left edge (16:9 is wider than 640) */
#define LX1	(718.2f)

/* a texture: w x h pixels of R, G, B, A bytes (A 0x80 = opaque), as the art file holds covers */
typedef struct
{
	int w, h;
	const u8 *px;
} tex_t;

void ui_clear(col_t c);
void rect(float x0, float y0, float x1, float y1, col_t c);
void hgrad(float x0, float y0, float x1, float y1, col_t ca, col_t cb);
void vgrad(float x0, float y0, float x1, float y1, col_t ca, col_t cb);
/* gsKit vertex order: 0 top left, 1 top right, 2 bottom left, 3 bottom right */
void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, col_t c0, col_t c1, col_t c2, col_t c3);
void tri(float x0, float y0, float x1, float y1, float x2, float y2, col_t c0, col_t c1, col_t c2);
void line(float x0, float y0, float x1, float y1, col_t c);
void frame_rect(float x0, float y0, float x1, float y1, float t, col_t c);
void stroke(float x0, float y0, float x1, float y1, col_t c);
/* (u, v) in texels; the colour modulates (0x80 = unchanged) */
void sprite_tex(const tex_t *t, float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1, col_t c);
/* a textured triangle with colours (the 3D memory-card icons) */
void tri_tex(const tex_t *t, const float *xy, const float *uv, const col_t *c);
/* v0.72: the same with a depth for each corner (0 nearest .. 1 farthest), for a solid object drawn between
 * ui_depth(1) and ui_depth(0): the nearer triangle then wins at every pixel. Only the RSX has a depth
 * buffer; the CPU drawing ignores both and relies on the caller drawing far to near. */
void tri_tex_z(const tex_t *t, const float *xy, const float *depth, const float *uv, const col_t *c);
void ui_depth(int on);
/* v0.27: copy a canvas-resolution picture (w x h, pixels equal to key skipped) with its top left at
 * canvas (XC(lx) + ox, YC(ly) + oy), blended with alpha a (GS scale, 0x80 = opaque) */
void blit_keyed(const u32 *src, int w, int h, u32 key, float lx, float ly, int ox, int oy, int a);
/* the canvas point of a logical point (x, y), rounded */
void canvas_point(float x, float y, int *cx, int *cy);
void scissor(float x0, float x1, float y0, float y1);
void scissor_reset(void);

int  text_w(int f, const char *s, int track);
int  text_ex(int f, int x, int y, float scale, int track, col_t c, const char *s);
int  text(int f, int x, int y, col_t c, const char *s);
void text_centre(int f, int cx, int y, int track, col_t c, const char *s);
void fit(char *out, unsigned n, int f, const char *s, int w);
int  font_h(int f);	/* logical */

#endif
