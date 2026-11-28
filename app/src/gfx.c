/* RSX set-up, the two frame paths of gfx.h, and (v0.67) the RSX drawing itself.
 *
 * The RSX path follows PSL1GHT's own samples call for call, because nothing here could be tried on a
 * console while it was written: samples/graphics/rsxtest (rsxutil.cpp: surface, flip; main.cpp
 * setDrawEnv / setTexture / drawFrame: the state, the texture set-up) and
 * samples/graphics/debugfont_renderer (rsxdebugfontrenderer.cpp: pass-through vertex program,
 * fragment program copied into RSX memory, vertex arrays in RSX memory, blending). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include "gfx.h"
#include "version.h"

#define CB_SIZE		0x100000
#define HOST_SIZE	(32 * 1024 * 1024)
#define LABEL		255

static gcmContextData *ctx;
static void *host;
static u32 *vbuf[2], voff[2];
static int cur, dw, dh;
static u16 *xmap;		/* display column -> canvas column, when scaling */
u32 *fb;

int gfx_gpu, gfx_soft_wanted, gfx_gpu_why, gx_exact;
float gx_iu = 1.0f, gx_iv = 1.0f;
unsigned gx_stat_verts, gx_stat_draws, gx_stat_dropped, gx_stat_uploads;

static u32 label_v = 1;

/* ---- v0.69: the command buffer is rewound after every frame the RSX draws, and no wait is endless ----
 *
 * What the console showed (2026-11-25, /dev_hdd0/tmp/prism_perf.log and prism_gfx.log):
 *
 * v0.67, the RSX drawing for the first time: 60 frames a second (18.7 before), then Prism died 11 to 23
 * seconds in, three times, on different screens. Each time about 1 MB of commands had been queued (1 KB a
 * frame on HOME, 2 KB on the Memory Card screen): the whole command buffer (CB_SIZE). The library hands
 * the buffer out in pieces of 32 KB (begin 0x1000, end 0x7ffc after rsxInit) and its callback moved to
 * the next piece some thirty times without trouble; what failed is the step at the buffer's very end.
 * That step is the system library's own code and was not looked into. Up to v0.66 a frame queued one
 * flip, about 24 bytes, and that end was 35 minutes of menus away.
 *
 * v0.68 tried to never get there: after each frame's flip, a jump back to the buffer's start, as
 * PSL1GHT's rsxResetCommandBuffer does it (PUT set to the start, wait for GET to arrive). It showed
 * nothing at all. The RSX runs from GET to PUT and stops where they meet; the frame had been written
 * FROM the start, so when PUT was set to the start the RSX, not yet moving, stood exactly there: GET
 * equal to PUT, nothing to do. The frame and its flip were never carried out (log: STUCK waiting for a
 * flip, PUT 0x1000, GET 0x1000). v0.68 also wrote over the library's own set-up commands at
 * 0x1000 .. 0x13a0, which may or may not matter; v0.69 leaves them alone.
 *
 * v0.69, two steps, each with the RSX somewhere else than where it is sent:
 *   1. PUT = the end of what was written (an ordinary flush); wait until GET is there. The frame is
 *      drawn, its flip has happened (the wait-for-flip command is among what was run), the RSX is idle.
 *   2. a jump to where the buffer stood after rsxInit (cb_start, 0x13a0 on the console) is written at
 *      that end, PUT = cb_start; GET is at the end, not at cb_start, so the RSX takes the jump and
 *      stops at cb_start. The context is set back to what rsxInit left.
 * A frame's commands are a few KB of a 27 KB piece that starts empty each time; the callback is never
 * needed. host/fake_rsx.c models GET, PUT and the jump, and refuses v0.68's sequence (gx_mutants.py).
 *
 * The CPU drawing (gfx_gpu 0) is left exactly as v0.66 had it: no rewind. It is the way back if this
 * fails: when a wait runs out, Prism writes RSX_FAILED with its version and closes (gfx_dead, main.c),
 * and the next start of the same version draws on the CPU. */
#define TB_HZ		79800000ULL
#define GFX_LOG		"/dev_hdd0/tmp/prism_gfx.log"

int gfx_dead;
/* 1.00: busy.c may draw the loading screen from its own thread while the app waits; every way into the drawing
 * below first takes the drawing back from it (GX_TAKE), which waits out its frame */
volatile int gx_lent;
void (*gx_take_back)(void);
#define GX_TAKE()	do { if (gx_lent && gx_take_back) gx_take_back(); } while (0)
static gcmContextData ctx0;		/* the context as rsxInit left it */
static u32 cb_start = 0xFFFFFFFF;	/* the offset of its "current" then; all ones = not known: no rewind */
static unsigned glog_lines, n_present, cb_max;

#ifdef __powerpc__
static u64 tbr(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}
#define SYNC()	__asm__ volatile("sync")
#else	/* the PC test build (host/fake_rsx.c): no time base, no waits that run out */
static u64 tbr(void) { return 0; }
#define SYNC()	(void)0
#endif

static void glog(const char *what, long a, long b, long c)
{
	FILE *f;

	if (glog_lines >= 300)
		return;
	f = fopen(GFX_LOG, glog_lines ? "a" : "w");
	if (!f)
		return;
	fprintf(f, "%s: %ld (0x%lx), %ld (0x%lx), %ld (0x%lx)\n", what, a, (unsigned long)a, b, (unsigned long)b, c, (unsigned long)c);
	fclose(f);
	glog_lines++;
}

static void dead(const char *where)
{
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	u32 cur = 0;

	gcmAddressToOffset(ctx->current, &cur);
	glog(where, (long)n_present, (long)cur, 0);
	glog("   the RSX's put, get, ref", (long)ctrl->put, (long)ctrl->get, (long)ctrl->ref);
	gfx_dead = 1;
	if (gfx_gpu)
	{
		/* the next start of this version draws on the CPU (main.c reads this) */
		FILE *f = fopen(GFX_RSX_FAILED, "w");

		if (f)
		{
			fprintf(f, "%s\n", APP_VERSION);
			fclose(f);
		}
	}
}

static void wait_label(void)
{
	u32 v = ++label_v;
	u64 t0 = tbr();

	rsxSetWriteBackendLabel(ctx, LABEL, v);
	rsxFlushBuffer(ctx);
	while (*(vu32 *)gcmGetLabelAddress(LABEL) != v)
	{
		usleep(30);
		if (tbr() - t0 > 3 * TB_HZ)
		{
			dead("STUCK waiting for a label: frame, command offset");
			return;
		}
	}
}

static void wait_flip(void)
{
	u64 t0 = tbr();

	while (gcmGetFlipStatus() != 0)
	{
		usleep(200);
		if (tbr() - t0 > 3 * TB_HZ)
		{
			dead("STUCK waiting for a flip: frame, command offset");
			return;
		}
	}
	gcmResetFlipStatus();
}

/* everything queued so far is carried out (the flip in it has happened), and the buffer is empty again:
 * the two steps described above */
static void cb_rewind(void)
{
	gcmControlRegister volatile *ctrl;
	u32 cur = 0;
	u64 t0 = tbr();

	if (cb_start == 0xFFFFFFFF || gfx_dead)
		return;
	ctrl = gcmGetControlRegister();
	gcmAddressToOffset(ctx->current, &cur);
	if (cur - cb_start > cb_max)
		cb_max = cur - cb_start;
	SYNC();
	ctrl->put = cur;
	while (ctrl->get != cur)
	{
		usleep(30);
		if (tbr() - t0 > 3 * TB_HZ)
		{
			dead("STUCK waiting for the RSX to finish the frame: frame, command offset");
			return;
		}
	}
	n_present++;
	if (n_present == 1 || n_present == 60 || n_present % 3600 == 0)
		glog("frame, most command bytes in one frame so far, this frame's", (long)n_present, (long)cb_max, (long)(cur - cb_start));
	if (cur == cb_start)
		return;		/* nothing was queued: the RSX is at the start already */
	rsxSetJumpCommand(ctx, cb_start);
	SYNC();
	ctrl->put = cb_start;
	t0 = tbr();
	while (ctrl->get != cb_start)
	{
		usleep(30);
		if (tbr() - t0 > 3 * TB_HZ)
		{
			dead("STUCK waiting for the RSX to jump back to the buffer's start: frame, command offset");
			return;
		}
	}
	ctx->begin = ctx0.begin;
	ctx->end = ctx0.end;
	ctx->current = ctx0.current;
}

static void flip(int i)
{
	if (gfx_dead)
		return;
	if (gcmSetFlip(ctx, i) == 0)
	{
		rsxFlushBuffer(ctx);
		gcmSetWaitFlip(ctx);
	}
	else
		glog("gcmSetFlip refused: frame, buffer", (long)n_present, (long)i, 0);
	if (gfx_gpu)
		cb_rewind();	/* the CPU drawing queues 24 bytes a frame and is left as v0.66 had it */
}

/* ------------------------------------------------------------------ the RSX draws (v0.67) */

/* the two programs, compiled by cgcomp at build time and linked in by bin2s (build_app.sh) */
extern const u8 prism_v_vpo[], prism_f_fpo[];

#define VB_MAX		65536			/* vertices a frame; 32 bytes each */
#define TEX_DIM		128			/* the largest kept texture (covers and save icons are 128 x 128) */
#define TEX_SLOTS	160
#define TEX_PITCH	(TEX_DIM * 4)

typedef struct
{
	const void *px;		/* NULL = free */
	int w, h;
	u32 sum;		/* a sparse checksum of the pixels as sent */
	u32 *mem, off;
	unsigned used, checked;	/* the frame it was last drawn in / last compared with its pixels */
} texent;

static rsxVertexProgram *vpo;
static rsxFragmentProgram *fpo;
static void *vp_ucode;
static u32 fp_off, depth_off, vb_off[2];
static u32 *depth_buf, *font_mem;
static u32 font_off;
static gxv_t *vb[2];
static int a_pos = -1, a_tex = -1, a_col = -1, t_unit = -1;
static unsigned nv, batch0, frame_no = 8;
static texent texs[TEX_SLOTS], *bound, *last_hit;
static texent font_ent;			/* stands for "the font texture" in bound */
static int clip_empty, sc_x, sc_y, sc_w, sc_h;
static unsigned st_draws, st_dropped, st_uploads;

static void gx_flush(void)
{
	if (nv > batch0)
	{
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, batch0, nv - batch0);
		batch0 = nv;
		st_draws++;
	}
}

static void set_target(int i)
{
	gcmSurface sf;

	memset(&sf, 0, sizeof sf);
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = voff[i];
	sf.colorPitch[0] = dw * 4;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1] = 64;
	sf.colorPitch[2] = 64;
	sf.colorPitch[3] = 64;
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_off;
	sf.depthPitch = dw * 4;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = dw;
	sf.height = dh;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(ctx, &sf);
}

static void set_scissor(void)
{
	/* canvas pixels -> the TV's (the same when the TV runs 1280 x 720) */
	int x0 = sc_x * dw / CW, y0 = sc_y * dh / CH, x1 = (sc_x + sc_w) * dw / CW, y1 = (sc_y + sc_h) * dh / CH;

	rsxSetScissor(ctx, x0, y0, x1 - x0, y1 - y0);
}

static void bind(texent *e)
{
	gcmTexture t;
	int smooth = 1;

	memset(&t, 0, sizeof t);
	t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	t.mipmap = 1;
	t.dimension = GCM_TEXTURE_DIMS_2D;
	t.cubemap = GCM_FALSE;
	t.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	t.depth = 1;
	t.location = GCM_LOCATION_RSX;
	if (e == &font_ent)
	{
		t.width = GX_FONT_W;
		t.height = GX_FONT_H + GX_FONT_BAND;
		t.pitch = GX_FONT_W * 4;
		t.offset = font_off;
		/* at 1280 x 720 a glyph's texels land one to a pixel, and scaled text is sampled the way
		 * ui.c's CPU text is (nearest); at another resolution everything is scaled, so smoothed */
		smooth = !gx_exact;
	}
	else
	{
		t.width = e->w;
		t.height = e->h;
		t.pitch = TEX_PITCH;
		t.offset = e->off;
	}
	gx_iu = 1.0f / (float)t.width;
	gx_iv = 1.0f / (float)t.height;
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(ctx, t_unit, &t);
	rsxTextureControl(ctx, t_unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, t_unit, 0, smooth ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, smooth ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
		GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, t_unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/* 64 pixels spread over the texture: enough to tell that the memory under a kept address now holds
 * another picture (the art file reloaded, a memory card read again) */
static u32 sparse_sum(const u8 *px, int w, int h)
{
	unsigned n = (unsigned)(w * h), step = n / 64 ? n / 64 : 1, i;
	u32 s = (u32)n;

	for (i = step / 2; i < n; i += step)
	{
		const u8 *p = px + i * 4;

		s = s * 31 + (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
	}
	return s;
}

static texent *tex_get(const void *px, int w, int h)
{
	texent *e, *spare = NULL;
	const u8 *s = (const u8 *)px;
	u32 sum;
	int i, x, y;

	if (last_hit && last_hit->px == px && last_hit->w == w && last_hit->h == h && last_hit->checked == frame_no)
	{
		last_hit->used = frame_no;
		return last_hit;
	}
	if (w < 1 || h < 1 || w > TEX_DIM || h > TEX_DIM)
		return NULL;
	sum = sparse_sum(s, w, h);
	for (i = 0; i < TEX_SLOTS; i++)
	{
		e = &texs[i];
		if (e->px == px && e->w == w && e->h == h)
		{
			if (e->sum == sum)
			{
				e->used = e->checked = frame_no;
				last_hit = e;
				return e;
			}
			e->px = NULL;	/* other pixels under the same address: it ages out, a new one is sent */
		}
	}
	/* a slot nothing queued can still be reading: free, or not drawn for three frames (the oldest) */
	for (i = 0; i < TEX_SLOTS; i++)
	{
		e = &texs[i];
		if (e->used + 3 > frame_no)
			continue;
		if (!e->px)
		{
			spare = e;
			break;
		}
		if (!spare || e->used < spare->used)
			spare = e;
	}
	if (!spare)
		return NULL;
	e = spare;
	for (y = 0; y < h; y++)
	{
		u32 *d = e->mem + y * TEX_DIM;

		for (x = 0; x < w; x++, s += 4)
		{
			u32 a = s[3] >= 0x80 ? 0xFF : (u32)s[3] * 2;	/* the GS's alpha: 0x80 is opaque */

			d[x] = (a << 24) | ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];
		}
	}
	e->px = px;
	e->w = w;
	e->h = h;
	e->sum = sum;
	e->used = e->checked = frame_no;
	last_hit = e;
	st_uploads++;
	return e;
}

gxv_t *gx_prims(const void *px, int w, int h, int n)
{
	texent *e = &font_ent;
	gxv_t *p;

	GX_TAKE();
	if (clip_empty || n <= 0)
		return NULL;
	if (px)
	{
		e = tex_get(px, w, h);
		if (!e)
		{
			st_dropped += n;
			return NULL;
		}
	}
	if (nv + (unsigned)n > VB_MAX)
	{
		st_dropped += n;
		return NULL;
	}
	if (e != bound)
	{
		gx_flush();
		bind(e);
		bound = e;
	}
	p = vb[cur] + nv;
	nv += n;
	return p;
}

void gx_clear(u32 rgb)
{
	GX_TAKE();
	gx_flush();
	rsxSetScissor(ctx, 0, 0, dw, dh);
	rsxSetClearColor(ctx, rgb & 0xFFFFFF);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	set_scissor();
}

/* v0.72: see gfx.h. The depth buffer is the one every frame's surface has carried since v0.67 (set_target);
 * the clear and the test are rsxtest's own calls (PSL1GHT samples/graphics/rsxtest main.cpp drawFrame). */
void gx_depth(int on)
{
	GX_TAKE();
	gx_flush();
	if (on)
	{
		rsxSetScissor(ctx, 0, 0, dw, dh);
		rsxSetClearDepthStencil(ctx, 0xffffff00);
		rsxClearSurface(ctx, GCM_CLEAR_Z | GCM_CLEAR_S);
		set_scissor();
		rsxSetDepthFunc(ctx, GCM_LESS);
	}
	rsxSetDepthTestEnable(ctx, on ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, on ? GCM_TRUE : GCM_FALSE);
}

void gx_scissor(int x0, int y0, int x1, int y1)
{
	GX_TAKE();
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > CW) x1 = CW;
	if (y1 > CH) y1 = CH;
	if (!clip_empty && x0 == sc_x && y0 == sc_y && x1 - x0 == sc_w && y1 - y0 == sc_h)
		return;
	gx_flush();
	clip_empty = x1 <= x0 || y1 <= y0;
	if (clip_empty)
		return;
	sc_x = x0;
	sc_y = y0;
	sc_w = x1 - x0;
	sc_h = y1 - y0;
	set_scissor();
}

void gx_font(const unsigned char *cov)
{
	int i;

	GX_TAKE();
	if (!gfx_gpu)
		return;
	for (i = 0; i < GX_FONT_W * GX_FONT_H; i++)
		font_mem[i] = ((u32)cov[i] << 24) | 0xFFFFFF;
	if (bound == &font_ent)
		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
}

void gx_tex_forget(void)
{
	int i;

	GX_TAKE();
	/* the slots keep their age: one drawn a frame ago may still be read by what is queued */
	for (i = 0; i < TEX_SLOTS; i++)
		texs[i].px = NULL;
	last_hit = NULL;
}

/* the state of a frame: everything is set again each frame, as the samples do */
static void frame_begin(void)
{
	f32 scale[4], offset[4];
	int i;

	frame_no++;
	set_target(cur);
	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	scale[0] = dw * 0.5f;
	scale[1] = dh * -0.5f;
	scale[2] = 0.5f;
	scale[3] = 0.0f;
	offset[0] = dw * 0.5f;
	offset[1] = dh * 0.5f;
	offset[2] = 0.5f;
	offset[3] = 0.0f;
	rsxSetViewport(ctx, 0, 0, dw, dh, 0.0f, 1.0f, scale, offset);
	sc_x = sc_y = 0;
	sc_w = CW;
	sc_h = CH;
	clip_empty = 0;
	set_scissor();
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	for (i = 0; i < 8; i++)
		rsxSetViewportClip(ctx, i, dw, dh);
	rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetLogicOpEnable(ctx, GCM_FALSE);
	rsxLoadVertexProgram(ctx, vpo, vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, fpo, fp_off, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, a_pos, 0, vb_off[cur], sizeof(gxv_t), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, a_tex, 0, vb_off[cur] + 12, sizeof(gxv_t), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, a_col, 0, vb_off[cur] + 20, sizeof(gxv_t), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
	nv = batch0 = 0;
	bound = NULL;
	st_draws = st_dropped = st_uploads = 0;
}

/* 0, or the negative step that failed (gfx_gpu_why): then nothing here is used and the CPU draws */
static int gpu_init(void)
{
	rsxProgramAttrib *a;
	void *uc, *fp_mem;
	u32 size;
	int i;

	vpo = (rsxVertexProgram *)prism_v_vpo;
	fpo = (rsxFragmentProgram *)prism_f_fpo;
	rsxVertexProgramGetUCode(vpo, &vp_ucode, &size);
	rsxFragmentProgramGetUCode(fpo, &uc, &size);
	if (!vp_ucode || !uc || !size)
		return -20;
	fp_mem = rsxMemalign(64, size);
	if (!fp_mem || rsxAddressToOffset(fp_mem, &fp_off) != 0)
		return -21;
	memcpy(fp_mem, uc, size);
	a = rsxVertexProgramGetAttrib(vpo, "position");
	if (a) a_pos = a->index;
	a = rsxVertexProgramGetAttrib(vpo, "texcoord");
	if (a) a_tex = a->index;
	a = rsxVertexProgramGetAttrib(vpo, "color");
	if (a) a_col = a->index;
	a = rsxFragmentProgramGetAttrib(fpo, "texture");
	if (a) t_unit = a->index;
	if (a_pos < 0 || a_tex < 0 || a_col < 0 || t_unit < 0)
		return -22;
	depth_buf = (u32 *)rsxMemalign(64, dw * dh * 4);
	if (!depth_buf || rsxAddressToOffset(depth_buf, &depth_off) != 0)
		return -23;
	for (i = 0; i < 2; i++)
	{
		vb[i] = (gxv_t *)rsxMemalign(128, VB_MAX * sizeof(gxv_t));
		if (!vb[i] || rsxAddressToOffset(vb[i], &vb_off[i]) != 0)
			return -24;
	}
	font_mem = (u32 *)rsxMemalign(128, GX_FONT_W * (GX_FONT_H + GX_FONT_BAND) * 4);
	if (!font_mem || rsxAddressToOffset(font_mem, &font_off) != 0)
		return -25;
	memset(font_mem, 0, GX_FONT_W * GX_FONT_H * 4);
	for (i = GX_FONT_W * GX_FONT_H; i < GX_FONT_W * (GX_FONT_H + GX_FONT_BAND); i++)
		font_mem[i] = 0xFFFFFFFF;
	{
		u32 *m = (u32 *)rsxMemalign(128, TEX_SLOTS * TEX_DIM * TEX_PITCH);

		if (!m)
			return -26;
		for (i = 0; i < TEX_SLOTS; i++)
		{
			texs[i].mem = m + i * TEX_DIM * TEX_DIM;
			if (rsxAddressToOffset(texs[i].mem, &texs[i].off) != 0)
				return -27;
		}
	}
	return 0;
}

int gx_dump(const char *path)
{
	const u32 *s;
	FILE *f;
	int i;

	GX_TAKE();
	if (!gfx_gpu)
		return -1;
	wait_label();		/* everything queued is drawn, and the flip before it has happened */
	s = vbuf[cur ^ 1];
	f = fopen(path, "wb");
	if (!f)
		return -2;
	fprintf(f, "P6\n%d %d\n255\n", dw, dh);
	for (i = 0; i < dw * dh; i++)
	{
		u32 c = s[i];
		unsigned char px[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };

		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ both paths */

int gfx_init(void)
{
	videoState st;
	videoResolution res;
	videoConfiguration vc;
	int i;

	host = memalign(1024 * 1024, HOST_SIZE);
	if (!host)
		return -1;
	rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);
	if (!ctx)
		return -2;
	{
		/* v0.68: the buffer as the library set it up (cb_rewind) */
		u32 ob = 0, oe = 0;

		ctx0 = *ctx;
		if (gcmAddressToOffset(ctx->begin, &ob) != 0 || gcmAddressToOffset(ctx->end, &oe) != 0
			|| gcmAddressToOffset(ctx->current, &cb_start) != 0 || oe <= ob || cb_start < ob || cb_start + 0x2000 > oe)
			cb_start = 0xFFFFFFFF;
		glog("command buffer after rsxInit: offsets of its begin, end, current", (long)ob, (long)oe, (long)cb_start);
	}
	if (videoGetState(0, 0, &st) != 0 || st.state != 0)
		return -3;
	if (videoGetResolution(st.displayMode.resolution, &res) != 0)
		return -4;

	memset(&vc, 0, sizeof(vc));
	vc.resolution = st.displayMode.resolution;
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	vc.pitch = res.width * sizeof(u32);
	vc.aspect = st.displayMode.aspect;
	wait_label();
	if (videoConfigure(0, &vc, NULL, 0) != 0)
		return -5;
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	dw = res.width;
	dh = res.height;
	gx_exact = dw == CW && dh == CH;
	for (i = 0; i < 2; i++)
	{
		vbuf[i] = (u32 *)rsxMemalign(64, dw * dh * 4);
		if (!vbuf[i] || rsxAddressToOffset(vbuf[i], &voff[i]) != 0)
			return -6;
		memset(vbuf[i], 0, dw * dh * 4);
		if (gcmSetDisplayBuffer(i, voff[i], dw * 4, dw, dh) != 0)
			return -7;
	}
	gfx_gpu_why = gfx_soft_wanted ? 1 : gpu_init();
	gfx_gpu = gfx_gpu_why == 0;
	if (!gfx_gpu)
	{
		fb = (u32 *)memalign(128, CW * CH * 4);
		if (!fb)
			return -8;
		memset(fb, 0, CW * CH * 4);
		if (dw != CW || dh != CH)
		{
			xmap = (u16 *)malloc(dw * sizeof(u16));
			if (!xmap)
				return -9;
			for (i = 0; i < dw; i++)
				xmap[i] = (u16)(i * CW / dw);
		}
	}
	gcmResetFlipStatus();
	flip(1);
	cur = 0;
	if (gfx_gpu)
		frame_begin();
	return 0;
}

void gfx_present(void)
{
	GX_TAKE();
	if (gfx_dead)
		return;
	if (gfx_gpu)
	{
		/* the frame is already queued for the RSX, into buffer cur. Queue its flip; flip() then waits
		 * until the RSX has drawn and shown it (cb_rewind, v0.68), so the next frame starts on an empty
		 * command buffer and the RSX never draws into the buffer on the screen */
		gx_flush();
		gx_stat_verts = nv;
		gx_stat_draws = st_draws;
		gx_stat_dropped = st_dropped;
		gx_stat_uploads = st_uploads;
		wait_flip();
		flip(cur);
		cur ^= 1;
		frame_begin();
		return;
	}
	wait_flip();
	if (!xmap)
		memcpy(vbuf[cur], fb, CW * CH * 4);
	else
	{
		int x, y;

		for (y = 0; y < dh; y++)
		{
			const u32 *s = fb + (y * CH / dh) * CW;
			u32 *d = vbuf[cur] + y * dw;

			for (x = 0; x < dw; x++)
				d[x] = s[xmap[x]];
		}
	}
	flip(cur);
	cur ^= 1;
}

void gfx_end(void)
{
	int i;

	GX_TAKE();
	if (!ctx)
		return;
	glog("closing: frames shown, most command bytes in one frame, stuck", (long)n_present, (long)cb_max, (long)gfx_dead);
	if (gfx_gpu && !gfx_dead)
		gx_flush();
	if (!gfx_dead)
		wait_flip();
	gcmSetWaitFlip(ctx);
	for (i = 0; i < 2; i++)
		if (vbuf[i])
			rsxFree(vbuf[i]);
	rsxFinish(ctx, 1);
	ctx = NULL;
}
