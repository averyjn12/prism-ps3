/* v0.67: a PC stand-in for the RSX calls src/gfx.c makes, so the REAL gfx.c and the gx_ half of ui.c run
 * on the PC (host/rsx_host.sh) and their frames can be compared with the CPU renders (host/rsx_compare.py).
 *
 * What it is: PSL1GHT's own headers (structures, constants, prototypes) with these bodies behind them. A
 * draw call is carried out by a plain triangle rasteriser written from the textbook rules (a pixel is
 * covered when its centre is inside, top-left rule on a tie; texel centres at +0.5; source-alpha blend).
 * It shares no code with ui.c's CPU drawing.
 *
 * What it is NOT: the RSX. It proves the vertices, colours, texel coordinates, batches, scissor boxes,
 * texture slots and buffer offsets gfx.c produces are right, and it refuses misuse it can see (fake_die).
 * It cannot prove the RSX accepts the same calls: that takes a console (or RPCS3). The calls themselves
 * follow PSL1GHT's samples for that reason (see the head of gfx.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <ppu-types.h>
#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>

#define ARENA		(128u << 20)
#define MAX_ALLOC	64

static u8 *arena;
static u32 top;
static struct { u32 off, size; } allocs[MAX_ALLOC];
static int n_alloc;

static gcmContextData the_ctx;
static u32 labels[256];
static int flip_status = 1, on_screen = -1;
static struct { u32 off, pitch, w, h; int set; } disp[8];
static gcmSurface surf;
static int surf_set, vp_loaded, fp_loaded;
static float vscale[4], voffset[4];
static int sx0, sy0, sx1, sy1;
static u32 clear_color;
static int blend_on;
static int depth_test, depth_write;	/* v0.72: the depth buffer (float per pixel, in the surface's depth memory) */
static u32 blend_sf, blend_df;
static struct { int set; u32 off; int stride, elems, dtype; } attr[16];
static gcmTexture tex;
static int tex_set, tex_on, tex_linear;
static int subpix;	/* 0 = exact; n = snap screen coordinates to 1/n pixel (FAKE_RSX_SUBPIX) */

unsigned long fake_tris, fake_pixels, fake_draws, fake_clears;

/* v0.68: the command buffer as the console has it (prism_gfx.log, 2026-11-25: begin 0x1000, end 0x7ffc,
 * current 0x13a0 after rsxInit - the library's own set-up commands fill 0x1000 .. 0x13a0).
 *
 * Every command the program queues sits at an OFFSET (here: 4 bytes each, at the context's "current",
 * which moves on). The RSX carries out the commands from its GET register up to the PUT register and
 * stops when the two are equal; a jump moves GET. Nothing is carried out when it is queued, and nothing
 * BEYOND PUT ever: rsxFlushBuffer and the program's own writes to PUT are what let the RSX go on.
 * The RSX here runs as LATE as it may: only when the program waits (usleep, the flip status, a label).
 * So a frame that was never flushed, a PUT set equal to GET (v0.68's rewind: the RSX stood at the
 * buffer's start, was told "up to the buffer's start", and drew nothing) and a vertex buffer or texture
 * changed before its frame ran all show here as they did, or would, on the console. */
enum { C_SURFACE, C_VIEWPORT, C_SCISSOR, C_CLEARCOLOR, C_CLEAR, C_BLENDFUNC, C_BLENDEN, C_BIND, C_TEX, C_TEXCTL, C_TEXFILTER,
	C_VP, C_FP, C_DRAW, C_LABEL, C_FLIP, C_JUMP, C_WAITFLIP, C_DEPTHTEST, C_DEPTHWRITE, C_DEPTHFUNC };
typedef struct
{
	int op, valid;
	u32 a[8];
	f32 f[8];
	gcmSurface sf;
	gcmTexture tx;
} cmd;
static void fake_die(const char *why, long a, long b);
#define CB_BEGIN	0x1000u
#define CB_SETUP	0x13a0u		/* the library's own commands end here */
#define CB_END		0x7ffcu
#define CB_SLOTS	((CB_END - CB_BEGIN) / 4)
static cmd slots[CB_SLOTS];
static gcmControlRegister ctrl_reg;
static u8 *cb_mem;
static int flip_queued;
static unsigned long idle_polls;
unsigned long fake_jumps, fake_cmds;

static cmd *push(int op)
{
	u32 off = (u32)((u8 *)the_ctx.current - cb_mem);
	cmd *c;

	if (!cb_mem || (u8 *)the_ctx.current < cb_mem + CB_BEGIN || off + 4 > CB_END)
		fake_die("a command written outside the command buffer's piece (the callback that would move on is not modelled)", off, 0);
	if (off < CB_SETUP)
		fake_die("a command written over the library's own set-up commands at the buffer's start", off, CB_SETUP);
	c = &slots[(off - CB_BEGIN) / 4];
	memset(c, 0, sizeof *c);
	c->op = op;
	c->valid = 1;
	the_ctx.current++;
	return c;
}

static void run_queue(int to_flip);

static void fake_die(const char *why, long a, long b)
{
	fprintf(stderr, "FAKE RSX: refused: %s (%ld, %ld)\n", why, a, b);
	exit(70);
}

/* the allocation an offset range lies in, or refuse */
static void in_alloc(u32 off, u32 len, const char *what)
{
	int i;

	for (i = 0; i < n_alloc; i++)
		if (off >= allocs[i].off && off + len <= allocs[i].off + allocs[i].size)
			return;
	fake_die(what, (long)off, (long)len);
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

/* ---- memory ---- */

void *rsxMemalign(u32 alignment, u32 size)
{
	u32 off;

	if (!arena)
	{
		arena = malloc(ARENA);
		memset(arena, 0xFF, ARENA);	/* unwritten vertices read as NaN, and are refused */
	}
	if (!alignment || (alignment & (alignment - 1)))
		fake_die("rsxMemalign alignment is not a power of two", alignment, size);
	off = (top + alignment - 1) & ~(alignment - 1);
	if (off + size > ARENA || n_alloc >= MAX_ALLOC)
		return NULL;
	top = off + size;
	allocs[n_alloc].off = off;
	allocs[n_alloc].size = size;
	n_alloc++;
	return arena + off;
}

void rsxFree(void *p) { (void)p; }

s32 gcmAddressToOffset(const void *ptr, u32 *offset)
{
	const u8 *p = ptr;

	if (!arena || p < arena || p >= arena + top)
		return -1;
	*offset = (u32)(p - arena);
	return 0;
}

/* ---- set-up, video, flips ---- */

s32 rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress)
{
	(void)cmdSize; (void)ioSize; (void)ioAddress;
	subpix = getenv("FAKE_RSX_SUBPIX") ? atoi(getenv("FAKE_RSX_SUBPIX")) : 0;
	cb_mem = rsxMemalign(0x1000, 0x10000);	/* the arena's first allocation: offsets 0 .. 0x10000 */
	the_ctx.begin = (u32 *)(cb_mem + CB_BEGIN);
	the_ctx.current = (u32 *)(cb_mem + CB_SETUP);	/* as on the console: the set-up commands are already there */
	the_ctx.end = (u32 *)(cb_mem + CB_END);
	ctrl_reg.put = ctrl_reg.get = CB_SETUP;
	ctrl_reg.ref = 0xFFFFFFFF;
	*context = &the_ctx;
	return 0;
}

gcmControlRegister *gcmGetControlRegister() { return &ctrl_reg; }

void rsxSetJumpCommand(gcmContextData *context, u32 offset)
{
	(void)context;
	if (offset < CB_BEGIN || offset >= CB_END || (offset & 3))
		fake_die("a jump to outside the command buffer", offset, 0);
	push(C_JUMP)->a[0] = offset;
}

int usleep(useconds_t usec)
{
	(void)usec;
	run_queue(0);
	if (++idle_polls > 200000)
		fake_die("the program waits for something the RSX never does (PUT, GET)", ctrl_reg.put, ctrl_reg.get);
	return 0;
}

void rsxFinish(gcmContextData *context, u32 ref_value) { (void)context; (void)ref_value; }

static int res_id(void)
{
	const char *e = getenv("FAKE_RSX_RES");

	return e && !strcmp(e, "1080") ? VIDEO_RESOLUTION_1080 : e && !strcmp(e, "480") ? VIDEO_RESOLUTION_480 : VIDEO_RESOLUTION_720;
}

s32 videoGetState(s32 videoOut, s32 deviceIndex, videoState *state)
{
	(void)videoOut; (void)deviceIndex;
	memset(state, 0, sizeof *state);
	state->displayMode.resolution = (u8)res_id();
	state->displayMode.aspect = VIDEO_ASPECT_16_9;
	return 0;
}

s32 videoGetResolution(s32 resolutionId, videoResolution *resolution)
{
	resolution->width = resolutionId == VIDEO_RESOLUTION_1080 ? 1920 : resolutionId == VIDEO_RESOLUTION_480 ? 720 : 1280;
	resolution->height = resolutionId == VIDEO_RESOLUTION_1080 ? 1080 : resolutionId == VIDEO_RESOLUTION_480 ? 480 : 720;
	return 0;
}

s32 videoConfigure(s32 videoOut, videoConfiguration *config, void *option, s32 blocking)
{
	(void)videoOut; (void)option; (void)blocking;
	if (config->format != VIDEO_BUFFER_FORMAT_XRGB)
		fake_die("videoConfigure: not XRGB", config->format, 0);
	return 0;
}

void gcmSetFlipMode(const u32 mode) { (void)mode; }

s32 gcmSetDisplayBuffer(const u8 bufferId, const u32 offset, const u32 pitch, const u32 width, const u32 height)
{
	if (bufferId >= 8 || pitch != width * 4)
		fake_die("gcmSetDisplayBuffer: id or pitch", bufferId, pitch);
	in_alloc(offset, pitch * height, "gcmSetDisplayBuffer: the buffer is not inside one allocation");
	disp[bufferId].off = offset;
	disp[bufferId].pitch = pitch;
	disp[bufferId].w = width;
	disp[bufferId].h = height;
	disp[bufferId].set = 1;
	return 0;
}

s32 gcmSetFlip(gcmContextData *context, const u8 bufferId)
{
	(void)context;
	if (bufferId >= 8 || !disp[bufferId].set)
		fake_die("gcmSetFlip: no such display buffer", bufferId, 0);
	if (flip_queued)
		fake_die("gcmSetFlip: a second flip queued before the first was carried out (the PPU would run frames ahead)", bufferId, 0);
	flip_queued = 1;
	push(C_FLIP)->a[0] = bufferId;
	return 0;
}

void gcmSetWaitFlip(gcmContextData *context) { (void)context; push(C_WAITFLIP); }
/* waiting on the flip: the RSX has got as far as that flip, and (the worst case) no further */
u32 gcmGetFlipStatus()
{
	run_queue(0);
	if (flip_status != 0 && ++idle_polls > 200000)
		fake_die("the program waits for a flip the RSX never carries out (PUT, GET)", ctrl_reg.put, ctrl_reg.get);
	return flip_status;
}
void gcmResetFlipStatus(void) { flip_status = 1; }
u32 *gcmGetLabelAddress(const u8 index) { run_queue(0); return &labels[index]; }
void rsxSetWriteBackendLabel(gcmContextData *context, u8 index, u32 value) { cmd *c = push(C_LABEL); (void)context; c->a[0] = index; c->a[1] = value; }
void rsxFlushBuffer(gcmContextData *context) { (void)context; ctrl_reg.put = (u32)((u8 *)the_ctx.current - cb_mem); }

/* ---- programs: the real cgcomp output, read as the big-endian file it is ---- */

void rsxVertexProgramGetUCode(const rsxVertexProgram *vp, void **ucode, u32 *size)
{
	const u8 *d = (const u8 *)vp;

	if (be16(d) != 0x5650)
		fake_die("the vertex program has no VP magic", be16(d), 0);
	*ucode = (void *)(d + be32(d + 20));
	*size = be16(d + 10) * 16;
}

void rsxFragmentProgramGetUCode(const rsxFragmentProgram *fp, void **ucode, u32 *size)
{
	const u8 *d = (const u8 *)fp;

	if (be16(d) != 0x4650)
		fake_die("the fragment program has no FP magic", be16(d), 0);
	*ucode = (void *)(d + be32(d + 20));
	*size = be16(d + 10) * 16;
}

static rsxProgramAttrib *find_attr(const u8 *d, const char *name)
{
	static rsxProgramAttrib out[8];
	static int n_out;
	u32 n = be16(d + 6), off = be32(d + 12), i;

	for (i = 0; i < n; i++)
	{
		const u8 *a = d + off + 12 * i;

		if (be32(a) && !strcmp((const char *)d + be32(a), name))
		{
			rsxProgramAttrib *r = &out[n_out++ & 7];

			r->index = be32(a + 4);
			return r;
		}
	}
	return NULL;
}

rsxProgramAttrib *rsxVertexProgramGetAttrib(const rsxVertexProgram *vp, const char *name) { return find_attr((const u8 *)vp, name); }
rsxProgramAttrib *rsxFragmentProgramGetAttrib(const rsxFragmentProgram *fp, const char *name) { return find_attr((const u8 *)fp, name); }

static void x_vp(const rsxVertexProgram *program, const void *ucode)
{
	if (!program || !ucode)
		fake_die("rsxLoadVertexProgram: null", 0, 0);
	vp_loaded = 1;
}

static void x_fp(const rsxFragmentProgram *program, u32 offset, u32 location)
{
	if (!program || location != GCM_LOCATION_RSX)
		fake_die("rsxLoadFragmentProgramLocation: null, or not in RSX memory", location, 0);
	in_alloc(offset, 32, "the fragment program is not in RSX memory");
	if (be32(arena + offset) != be32((const u8 *)program + be32((const u8 *)program + 20)))
		fake_die("the fragment program in RSX memory is not the program's code", offset, 0);
	fp_loaded = 1;
}

/* ---- state ---- */

static void x_surface(const gcmSurface *s)
{
	if (s->colorFormat != GCM_SURFACE_X8R8G8B8 || s->colorTarget != GCM_SURFACE_TARGET_0 || s->type != GCM_SURFACE_TYPE_LINEAR
		|| s->colorLocation[0] != GCM_LOCATION_RSX || s->colorPitch[0] != (u32)s->width * 4 || (s->colorPitch[0] & 63))
		fake_die("rsxSetSurface: format, target, type, location or pitch", s->colorFormat, s->colorPitch[0]);
	in_alloc(s->colorOffset[0], s->colorPitch[0] * s->height, "rsxSetSurface: the colour buffer is not inside one allocation");
	in_alloc(s->depthOffset, s->depthPitch * s->height, "rsxSetSurface: the depth buffer is not inside one allocation");
	surf = *s;
	surf_set = 1;
}

static void x_viewport(const f32 *scale, const f32 *offset)
{
	memcpy(vscale, scale, sizeof vscale);
	memcpy(voffset, offset, sizeof voffset);
}

static void x_scissor(u32 x, u32 y, u32 w, u32 h)
{
	if (!surf_set || w == 0 || h == 0 || x + w > surf.width || y + h > surf.height || x > 4095 || y > 4095)
		fake_die("rsxSetScissor: empty, or outside the surface", x + w, y + h);
	sx0 = x; sy0 = y; sx1 = x + w; sy1 = y + h;
}

static void x_clearcolor(u32 color) { clear_color = color; }

static u32 *target_row(int y)
{
	int i;

	if (!surf_set)
		fake_die("drawing with no surface set", 0, 0);
	for (i = 0; i < 8; i++)
		if (disp[i].set && disp[i].off == surf.colorOffset[0] && i == on_screen)
			fake_die("drawing into the display buffer that is on the screen", i, 0);
	return (u32 *)(arena + surf.colorOffset[0] + (u32)y * surf.colorPitch[0]);
}

static void x_clear(u32 clear_mask)
{
	int x, y;

	if (clear_mask & GCM_CLEAR_Z)
	{
		if (!surf_set)
			fake_die("a depth clear with no surface set", 0, 0);
		for (y = sy0; y < sy1; y++)
		{
			float *zrow = (float *)(arena + surf.depthOffset + (u32)y * surf.depthPitch);

			for (x = sx0; x < sx1; x++)
				zrow[x] = 1.0f;
		}
	}
	if (!(clear_mask & (GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B)))
		return;
	for (y = sy0; y < sy1; y++)
	{
		u32 *row = target_row(y);

		for (x = sx0; x < sx1; x++)
			row[x] = clear_color & 0xFFFFFF;
	}
	fake_clears++;
}

static void x_blendfunc(u32 sfcolor, u32 dfcolor)
{
	blend_sf = sfcolor;
	blend_df = dfcolor;
}

static void x_blenden(u32 enable) { blend_on = (int)enable; }
void rsxSetBlendEquation(gcmContextData *context, u16 color, u16 alpha) { (void)context; (void)alpha; if (color != GCM_FUNC_ADD) fake_die("blend equation", color, 0); }
void rsxSetColorMask(gcmContextData *context, u32 mask) { (void)context; (void)mask; }
void rsxSetColorMaskMrt(gcmContextData *context, u32 mask) { (void)context; (void)mask; }
void rsxSetDepthTestEnable(gcmContextData *context, u32 enable) { (void)context; push(C_DEPTHTEST)->a[0] = enable; }
void rsxSetDepthWriteEnable(gcmContextData *context, u32 enable) { (void)context; push(C_DEPTHWRITE)->a[0] = enable; }
void rsxSetDepthFunc(gcmContextData *context, u32 func) { (void)context; push(C_DEPTHFUNC)->a[0] = func; }
void rsxSetClearDepthStencil(gcmContextData *context, u32 value)
{
	(void)context;
	if (value != 0xffffff00)
		fake_die("a depth clear value other than the farthest", value, 0);
}
void rsxSetCullFaceEnable(gcmContextData *context, u32 enable) { (void)context; if (enable) fake_die("culling on", 0, 0); }
void rsxSetAlphaTestEnable(gcmContextData *context, u32 enable) { (void)context; if (enable) fake_die("alpha test on", 0, 0); }
void rsxSetShadeModel(gcmContextData *context, u32 shadeModel) { (void)context; (void)shadeModel; }
void rsxSetZMinMaxControl(gcmContextData *context, u8 cullNearFar, u8 zClampEnable, u8 cullIgnoreW) { (void)context; (void)cullNearFar; (void)zClampEnable; (void)cullIgnoreW; }
void rsxSetViewportClip(gcmContextData *context, u8 sel, u16 width, u16 height) { (void)context; (void)sel; (void)width; (void)height; }
void rsxSetUserClipPlaneControl(gcmContextData *context, u32 p0, u32 p1, u32 p2, u32 p3, u32 p4, u32 p5) { (void)context; (void)p0; (void)p1; (void)p2; (void)p3; (void)p4; (void)p5; }
void rsxSetLogicOpEnable(gcmContextData *context, u32 enable) { (void)context; (void)enable; }
void rsxInvalidateTextureCache(gcmContextData *context, u32 type) { (void)context; (void)type; }
void rsxInvalidateVertexCache(gcmContextData *context) { (void)context; }

static void x_bind(u32 a, u32 offset, u32 stride, u32 elems, u32 dtype, u32 location)
{
	if (a >= 16 || location != GCM_LOCATION_RSX || dtype != GCM_VERTEX_DATA_TYPE_F32 || elems < 1 || elems > 4 || stride < elems * 4)
		fake_die("rsxBindVertexArrayAttrib: attribute, location, type, count or stride", a, stride);
	attr[a].set = 1;
	attr[a].off = offset;
	attr[a].stride = stride;
	attr[a].elems = elems;
	attr[a].dtype = dtype;
}

static void x_tex(u32 index, const gcmTexture *t)
{
	if (index != 0 || t->format != (GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN) || t->mipmap != 1 || t->dimension != GCM_TEXTURE_DIMS_2D
		|| t->location != GCM_LOCATION_RSX || t->pitch < (u32)t->width * 4 || t->width < 1 || t->height < 1 || t->width > 4096 || t->height > 4096)
		fake_die("rsxLoadTexture: unit, format, mipmaps, location, pitch or size", t->format, t->pitch);
	in_alloc(t->offset, t->pitch * (t->height - 1) + t->width * 4, "rsxLoadTexture: the texture is not inside one allocation");
	tex = *t;
	tex_set = 1;
}

static void x_texctl(u32 enable)
{
	tex_on = (int)enable;
}

static void x_texfilter(u32 min, u32 mag)
{
	if ((min != GCM_TEXTURE_LINEAR && min != GCM_TEXTURE_NEAREST) || min != mag)
		fake_die("rsxTextureFilter: a mipmap filter on a texture with no mipmaps, or min != mag", min, mag);
	tex_linear = min == GCM_TEXTURE_LINEAR;
}

void rsxTextureWrapMode(gcmContextData *context, u8 index, u8 wraps, u8 wrapt, u8 wrapr, u8 unsignedRemap, u8 zfunc, u8 gamma)
{
	(void)context; (void)index; (void)wrapr; (void)unsignedRemap; (void)zfunc; (void)gamma;
	if (wraps != GCM_TEXTURE_CLAMP_TO_EDGE || wrapt != GCM_TEXTURE_CLAMP_TO_EDGE)
		fake_die("rsxTextureWrapMode: not clamp-to-edge (the only wrap a non-power-of-two texture may use)", wraps, wrapt);
}

/* ---- drawing ---- */

typedef struct { double x, y, z, u, v, c[4]; } vert;

static void fetch(u32 i, vert *o)
{
	static const int need[3] = { GCM_VERTEX_ATTRIB_POS, GCM_VERTEX_ATTRIB_TEX0, GCM_VERTEX_ATTRIB_TEX1 };
	float f[3][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
	int k, e;

	for (k = 0; k < 3; k++)
	{
		u32 off = attr[need[k]].off + i * (u32)attr[need[k]].stride;

		{
			int j, home = -1;

			for (j = 0; j < n_alloc; j++)
				if (attr[need[k]].off >= allocs[j].off && attr[need[k]].off < allocs[j].off + allocs[j].size)
					home = j;
			if (home < 0 || off + (u32)attr[need[k]].elems * 4 > allocs[home].off + allocs[home].size)
				fake_die("a vertex is read from outside the buffer its array was bound in", (long)i, k);
		}
		memcpy(f[k], arena + off, (size_t)attr[need[k]].elems * 4);
		for (e = 0; e < attr[need[k]].elems; e++)
			if (isnan(f[k][e]))
				fake_die("a vertex that was never written is drawn", (long)i, k);
	}
	o->x = voffset[0] + vscale[0] * f[0][0];
	o->y = voffset[1] + vscale[1] * f[0][1];
	o->z = voffset[2] + vscale[2] * (attr[need[0]].elems >= 3 ? f[0][2] : 0.0f);
	if (o->z < 0.0 || o->z > 1.0)
		fake_die("a vertex outside the depth range (it would be clipped away)", (long)i, (long)(o->z * 1000));
	if (subpix)
	{
		o->x = floor(o->x * subpix + 0.5) / subpix;
		o->y = floor(o->y * subpix + 0.5) / subpix;
	}
	o->u = f[1][0];
	o->v = f[1][1];
	for (e = 0; e < 4; e++)
		o->c[e] = f[2][e];
}

static void texel(int i, int j, double *o)
{
	u32 t;

	if (i < 0) i = 0;
	if (j < 0) j = 0;
	if (i >= tex.width) i = tex.width - 1;
	if (j >= tex.height) j = tex.height - 1;
	t = *(u32 *)(arena + tex.offset + (u32)j * tex.pitch + (u32)i * 4);	/* A8R8G8B8, one word a texel */
	o[0] = ((t >> 16) & 0xFF) / 255.0;
	o[1] = ((t >> 8) & 0xFF) / 255.0;
	o[2] = (t & 0xFF) / 255.0;
	o[3] = (t >> 24) / 255.0;
}

static void sample(double u, double v, double *o)
{
	double x = u * tex.width, y = v * tex.height;
	int k;

	if (!tex_linear)
	{
		texel((int)floor(x), (int)floor(y), o);
		return;
	}
	{
		double fx = x - 0.5, fy = y - 0.5, a[4], b[4], c[4], d[4];
		int i = (int)floor(fx), j = (int)floor(fy);

		fx -= i;
		fy -= j;
		texel(i, j, a); texel(i + 1, j, b); texel(i, j + 1, c); texel(i + 1, j + 1, d);
		for (k = 0; k < 4; k++)
			o[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
	}
}

static double orient(const vert *a, const vert *b, double px, double py)
{
	return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* with y down and the inside on the positive side: an edge pointing right is a top edge, one pointing
 * up is a left edge */
static int top_left(const vert *a, const vert *b)
{
	return (b->y == a->y && b->x > a->x) || b->y < a->y;
}

static int covers(double w, const vert *a, const vert *b)
{
	return w > 0.0 || (w == 0.0 && top_left(a, b));
}

static void triangle(vert *a, vert *b, vert *c)
{
	double area = orient(a, b, c->x, c->y), mnx, mxx, mny, mxy;
	int x0, x1, y0, y1, x, y, k;

	if (area == 0.0)
		return;
	if (area < 0.0)
	{
		vert *t = b;

		b = c;
		c = t;
		area = -area;
	}
	mnx = fmin(a->x, fmin(b->x, c->x)); mxx = fmax(a->x, fmax(b->x, c->x));
	mny = fmin(a->y, fmin(b->y, c->y)); mxy = fmax(a->y, fmax(b->y, c->y));
	x0 = (int)floor(mnx) - 1; x1 = (int)ceil(mxx) + 1; y0 = (int)floor(mny) - 1; y1 = (int)ceil(mxy) + 1;
	if (x0 < sx0) x0 = sx0;
	if (y0 < sy0) y0 = sy0;
	if (x1 > sx1) x1 = sx1;
	if (y1 > sy1) y1 = sy1;
	fake_tris++;
	for (y = y0; y < y1; y++)
	{
		u32 *row = NULL;

		for (x = x0; x < x1; x++)
		{
			double px = x + 0.5, py = y + 0.5;
			double w0 = orient(b, c, px, py), w1 = orient(c, a, px, py), w2 = orient(a, b, px, py), t[4], s[4], al;
			u32 d;
			int o[3];

			if (!covers(w0, b, c) || !covers(w1, c, a) || !covers(w2, a, b))
				continue;
			w0 /= area; w1 /= area; w2 /= area;
			if (depth_test || depth_write)
			{
				float *zp = (float *)(arena + surf.depthOffset + (u32)y * surf.depthPitch) + x, z = (float)(w0 * a->z + w1 * b->z + w2 * c->z);

				if (depth_test && !(z < *zp))
					continue;
				if (depth_write)
					*zp = z;
			}
			sample(w0 * a->u + w1 * b->u + w2 * c->u, w0 * a->v + w1 * b->v + w2 * c->v, t);
			for (k = 0; k < 4; k++)
			{
				s[k] = t[k] * (w0 * a->c[k] + w1 * b->c[k] + w2 * c->c[k]);
				s[k] = s[k] < 0.0 ? 0.0 : s[k] > 1.0 ? 1.0 : s[k];
			}
			if (!row)
				row = target_row(y);
			d = row[x];
			al = blend_on ? s[3] : 1.0;
			o[0] = (int)((s[0] * al + ((d >> 16) & 0xFF) / 255.0 * (1.0 - al)) * 255.0 + 0.5);
			o[1] = (int)((s[1] * al + ((d >> 8) & 0xFF) / 255.0 * (1.0 - al)) * 255.0 + 0.5);
			o[2] = (int)((s[2] * al + (d & 0xFF) / 255.0 * (1.0 - al)) * 255.0 + 0.5);
			row[x] = ((u32)o[0] << 16) | ((u32)o[1] << 8) | (u32)o[2];
			fake_pixels++;
		}
	}
}

static void x_draw(u32 type, u32 start, u32 count)
{
	u32 i;

	if (type != GCM_TYPE_TRIANGLES || count % 3 || !count)
		fake_die("rsxDrawVertexArray: not whole triangles", type, count);
	if (!vp_loaded || !fp_loaded || !tex_set || !tex_on || !surf_set)
		fake_die("a draw before the programs, the texture or the surface are set", vp_loaded * 100 + fp_loaded * 10 + tex_set, tex_on);
	if (!attr[GCM_VERTEX_ATTRIB_POS].set || !attr[GCM_VERTEX_ATTRIB_TEX0].set || !attr[GCM_VERTEX_ATTRIB_TEX1].set)
		fake_die("a draw before the three vertex arrays are bound", 0, 0);
	if (blend_on && (blend_sf != GCM_SRC_ALPHA || blend_df != GCM_ONE_MINUS_SRC_ALPHA))
		fake_die("a blend this stand-in does not know", blend_sf, blend_df);
	for (i = start; i + 2 < start + count; i += 3)
	{
		vert a, b, c;

		fetch(i, &a);
		fetch(i + 1, &b);
		fetch(i + 2, &c);
		triangle(&a, &b, &c);
	}
	fake_draws++;
}

/* ---- the queue: what the program calls, and the late run ---- */

void rsxLoadVertexProgram(gcmContextData *context, const rsxVertexProgram *program, const void *ucode)
{
	cmd *c = push(C_VP);

	(void)context;
	c->a[0] = program != NULL;
	c->a[1] = ucode != NULL;
}

static const rsxFragmentProgram *fp_arg;

void rsxLoadFragmentProgramLocation(gcmContextData *context, const rsxFragmentProgram *program, u32 offset, u32 location)
{
	cmd *c = push(C_FP);

	(void)context;
	fp_arg = program;
	c->a[0] = offset;
	c->a[1] = location;
}

void rsxSetSurface(gcmContextData *context, const gcmSurface *sf) { (void)context; push(C_SURFACE)->sf = *sf; }

void rsxSetViewport(gcmContextData *context, u16 x, u16 y, u16 width, u16 height, f32 min, f32 max, const f32 scale[4], const f32 offset[4])
{
	cmd *c = push(C_VIEWPORT);

	(void)context; (void)x; (void)y; (void)width; (void)height; (void)min; (void)max;
	memcpy(c->f, scale, 4 * sizeof(f32));
	memcpy(c->f + 4, offset, 4 * sizeof(f32));
}

void rsxSetScissor(gcmContextData *context, u16 x, u16 y, u16 w, u16 h)
{
	cmd *c = push(C_SCISSOR);

	(void)context;
	c->a[0] = x; c->a[1] = y; c->a[2] = w; c->a[3] = h;
}

void rsxSetClearColor(gcmContextData *context, u32 color) { (void)context; push(C_CLEARCOLOR)->a[0] = color; }
void rsxClearSurface(gcmContextData *context, u32 clear_mask) { (void)context; push(C_CLEAR)->a[0] = clear_mask; }

void rsxSetBlendFunc(gcmContextData *context, u16 sfcolor, u16 dfcolor, u16 sfalpha, u16 dfalpha)
{
	cmd *c = push(C_BLENDFUNC);

	(void)context; (void)sfalpha; (void)dfalpha;
	c->a[0] = sfcolor; c->a[1] = dfcolor;
}

void rsxSetBlendEnable(gcmContextData *context, u32 enable) { (void)context; push(C_BLENDEN)->a[0] = enable; }

void rsxBindVertexArrayAttrib(gcmContextData *context, u8 a, u16 frequency, u32 offset, u8 stride, u8 elems, u8 dtype, u8 location)
{
	cmd *c = push(C_BIND);

	(void)context; (void)frequency;
	c->a[0] = a; c->a[1] = offset; c->a[2] = stride; c->a[3] = elems; c->a[4] = dtype; c->a[5] = location;
}

void rsxLoadTexture(gcmContextData *context, u8 index, const gcmTexture *t)
{
	cmd *c = push(C_TEX);

	(void)context;
	c->a[0] = index;
	c->tx = *t;
}

void rsxTextureControl(gcmContextData *context, u8 index, u32 enable, u16 minlod, u16 maxlod, u8 maxaniso)
{
	(void)context; (void)index; (void)minlod; (void)maxlod; (void)maxaniso;
	push(C_TEXCTL)->a[0] = enable;
}

void rsxTextureFilter(gcmContextData *context, u8 index, u16 bias, u8 min, u8 mag, u8 conv)
{
	cmd *c = push(C_TEXFILTER);

	(void)context; (void)index; (void)bias; (void)conv;
	c->a[0] = min; c->a[1] = mag;
}

void rsxDrawVertexArray(gcmContextData *context, u32 type, u32 start, u32 count)
{
	cmd *c = push(C_DRAW);

	(void)context;
	c->a[0] = type; c->a[1] = start; c->a[2] = count;
}

static void run_queue(int to_flip)
{
	unsigned long steps = 0;

	(void)to_flip;
	while (ctrl_reg.get != ctrl_reg.put)
	{
		u32 at = ctrl_reg.get;
		cmd *c;

		if (at < CB_BEGIN || at >= CB_END || (at & 3))
			fake_die("the RSX's GET left the command buffer", at, ctrl_reg.put);
		c = &slots[(at - CB_BEGIN) / 4];
		if (!c->valid)
			fake_die("the RSX ran into a place where no command was written (GET, PUT)", at, ctrl_reg.put);
		if (++steps > 4 * CB_SLOTS)
			fake_die("the RSX goes round and round the command buffer (GET, PUT)", at, ctrl_reg.put);
		ctrl_reg.get = at + 4;
		idle_polls = 0;
		fake_cmds++;

		switch (c->op)
		{
		case C_SURFACE: x_surface(&c->sf); break;
		case C_VIEWPORT: x_viewport(c->f, c->f + 4); break;
		case C_SCISSOR: x_scissor(c->a[0], c->a[1], c->a[2], c->a[3]); break;
		case C_CLEARCOLOR: x_clearcolor(c->a[0]); break;
		case C_CLEAR: x_clear(c->a[0]); break;
		case C_BLENDFUNC: x_blendfunc(c->a[0], c->a[1]); break;
		case C_BLENDEN: x_blenden(c->a[0]); break;
		case C_BIND: x_bind(c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5]); break;
		case C_TEX: x_tex(c->a[0], &c->tx); break;
		case C_TEXCTL: x_texctl(c->a[0]); break;
		case C_TEXFILTER: x_texfilter(c->a[0], c->a[1]); break;
		case C_VP: x_vp(c->a[0] ? (const rsxVertexProgram *)"" : NULL, c->a[1] ? "" : NULL); break;
		case C_FP: x_fp(fp_arg, c->a[0], c->a[1]); break;
		case C_DRAW: x_draw(c->a[0], c->a[1], c->a[2]); break;
		case C_LABEL: labels[c->a[0]] = c->a[1]; break;
		case C_JUMP:
			ctrl_reg.get = c->a[0];
			fake_jumps++;
			break;
		case C_WAITFLIP:
			break;		/* flips happen at once here */
		case C_DEPTHTEST: depth_test = (int)c->a[0]; break;
		case C_DEPTHWRITE: depth_write = (int)c->a[0]; break;
		case C_DEPTHFUNC:
			if (c->a[0] != GCM_LESS)
				fake_die("a depth test this stand-in does not know", c->a[0], 0);
			break;
		case C_FLIP:
			on_screen = (int)c->a[0];
			flip_status = 0;
			flip_queued = 0;
			break;
		}
	}
}
