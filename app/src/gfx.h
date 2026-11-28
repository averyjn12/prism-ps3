/* Display for the Prism app. Two ways a frame reaches the TV:
 *
 *   gfx_gpu = 1 (v0.67, the default): the RSX draws it. ui.c hands gfx.c triangles (gx_prims) with
 *   canvas coordinates, texel coordinates and a colour; one vertex and one fragment program
 *   (src/shaders) blend them straight into the back display buffer, at the TV's own resolution.
 *
 *   gfx_gpu = 0 (v0.66 and before; kept as the fallback): the CPU draws into a 1280 x 720
 *   main-memory canvas (fb, 0x00RRGGBB) and gfx_present() copies it (scaled when the TV runs another
 *   resolution) into one of two RSX display buffers. Chosen when gfx_soft_wanted is set before
 *   gfx_init(), or when the RSX set-up fails.
 *
 * Either way gfx_present() shows the frame at the next vsync. All drawing goes through ui.c. */
#ifndef PRISM_APP_GFX_H
#define PRISM_APP_GFX_H
#include <ppu-types.h>

#define CW	1280
#define CH	720

extern u32 *fb;			/* CW x CH; NULL while the RSX draws */

int  gfx_init(void);		/* 0 or a negative step number */
void gfx_present(void);
void gfx_end(void);

/* ---- v0.67: the RSX draws ---- */

extern int gfx_gpu;		/* 1 = the RSX draws (set by gfx_init) */
#ifdef UI_RSX
#define GFX_GPU	gfx_gpu
#else
#define GFX_GPU	0		/* the PC renderer (host/render.c): the CPU path only */
#endif
extern int gfx_soft_wanted;	/* set before gfx_init() to keep the CPU drawing */
#define GFX_RSX_FAILED	"/dev_hdd0/tmp/prism_rsx_failed"	/* v0.69: holds the version whose RSX drawing stopped answering */
extern int gfx_dead;		/* v0.68: the RSX stopped answering (prism_gfx.log says where): nothing more is drawn, main.c closes Prism */
extern volatile int gx_lent;		/* 1.00: busy.c holds the drawing (a loading screen while the app waits) */
extern void (*gx_take_back)(void);	/* busy.c: take it back */
extern int gfx_gpu_why;		/* why the RSX is not drawing: 0 it is, 1 asked for, negative = the set-up step that failed */

/* the font texture: the coverage map of font_ps3.h (GX_FONT_W x GX_FONT_H) with a band of opaque
 * white rows under it, which plain shapes sample, so text and shapes share one texture and batch */
#define GX_FONT_W	1024
#define GX_FONT_H	512
#define GX_FONT_BAND	8
#define GX_WHITE_U	4.0f
#define GX_WHITE_V	((float)(GX_FONT_H + GX_FONT_BAND / 2))

/* a vertex as the RSX reads it. Written through gx_put() only, in order, never read back: the
 * buffer is in RSX memory */
typedef struct
{
	float x, y, z;		/* clip space; z -1 near .. 1 far, used only between gx_depth(1) and gx_depth(0) */
	float u, v;		/* 0..1 over the bound texture */
	float r, g, b, a;	/* 0..1; r, g, b may pass 1 over a texture (the GS's 0x80 = unchanged) */
} gxv_t;

extern float gx_iu, gx_iv;	/* 1 / the bound texture's width and height */
extern int gx_exact;		/* the TV runs 1280 x 720: a canvas pixel is a TV pixel, and the font texture is sampled unsmoothed */

/* Room for n vertices (triangles: n is a multiple of 3) drawn with a texture: px NULL = the font
 * texture; else w x h pixels of R, G, B, A bytes (A 0x80 = opaque), kept in RSX memory under its
 * address and re-sent when its pixels change (smoothed, bilinear, as ui.c's CPU sprites are).
 * NULL = nothing may be drawn (clipped away, buffer full, texture too big): skip the shape. */
gxv_t *gx_prims(const void *px, int w, int h, int n);

static inline void gx_put(gxv_t *p, float X, float Y, float U, float V, float r, float g, float b, float a)
{
	p->x = X * (2.0f / CW) - 1.0f;
	p->y = 1.0f - Y * (2.0f / CH);
	p->z = 0.0f;
	p->u = U * gx_iu;
	p->v = V * gx_iv;
	p->r = r;
	p->g = g;
	p->b = b;
	p->a = a;
}

/* v0.72: a depth buffer for a solid object (a 3D save icon). gx_depth(1) empties it and turns it on: of
 * the triangles drawn until gx_depth(0), the nearer one wins at every pixel, whatever order they come in.
 * Their vertices get their depth with gx_z() after gx_put(): 0 = nearest, 1 = farthest. */
void gx_depth(int on);
static inline void gx_z(gxv_t *p, float depth01)
{
	p->z = depth01 * 2.0f - 1.0f;
}
void gx_clear(u32 rgb);				/* the whole screen, whatever the scissor */
void gx_scissor(int x0, int y0, int x1, int y1);	/* canvas pixels, x1 / y1 exclusive */
void gx_font(const unsigned char *cov);		/* the coverage map, once */
void gx_tex_forget(void);			/* every kept texture is dropped (their memory was freed or reloaded) */
int  gx_dump(const char *path);			/* the frame last presented, read back, as a PPM; 0 = written */

/* for the perf log: the frame last presented */
extern unsigned gx_stat_verts, gx_stat_draws, gx_stat_dropped, gx_stat_uploads;

#endif
