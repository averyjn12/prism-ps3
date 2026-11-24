/* Software rasteriser for the launcher's primitives (see ui.h). Logical (PS2) coordinates are
 * mapped to the canvas by X = OX + x * S, Y = y * S. Pixels are covered when their centres are
 * inside; colours blend over the canvas with the GS's alpha (0x80 = opaque). */
#include <string.h>
#include <math.h>
#include "ui.h"
#include "gfx.h"
#include "font_ps3.h"

#define S	(720.0f / 448.0f)
#define OX	((1280.0f - 640.0f * S) / 2.0f)
#define XC(x)	(OX + (x) * S)
#define YC(y)	((y) * S)

static int clx0 = 0, clx1 = CW, cly0 = 0, cly1 = CH;

typedef struct { float r, g, b, a; } fcol;	/* r, g, b 0..255, a 0..256 weight */

static fcol fc(col_t c)
{
	fcol f;
	u32 a = c & 0xFF;

	f.r = (float)(c >> 24);
	f.g = (float)((c >> 16) & 0xFF);
	f.b = (float)((c >> 8) & 0xFF);
	f.a = a >= 0x80 ? 256.0f : (float)(a * 2);
	return f;
}

static inline void put(u32 *p, int r, int g, int b, int w)
{
	u32 d;
	int dr, dg, db;

	if (w <= 0)
		return;
	if (w >= 256)
	{
		*p = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
		return;
	}
	d = *p;
	dr = (int)((d >> 16) & 0xFF);
	dg = (int)((d >> 8) & 0xFF);
	db = (int)(d & 0xFF);
	dr += ((r - dr) * w) >> 8;
	dg += ((g - dg) * w) >> 8;
	db += ((b - db) * w) >> 8;
	*p = ((u32)dr << 16) | ((u32)dg << 8) | (u32)db;
}

static int ci(float v)
{
	return (int)ceilf(v - 0.5f);
}

void ui_clear(col_t c)
{
	u32 v = (c >> 8) & 0xFFFFFF, *p = fb, *e = fb + CW * CH;

	while (p < e)
		*p++ = v;
}

/* canvas-space rectangle with a colour at each corner (top left, top right, bottom left, right) */
static void crect(float X0, float Y0, float X1, float Y1, fcol tl, fcol tr, fcol bl, fcol br, int flat)
{
	int x0 = ci(X0), x1 = ci(X1), y0 = ci(Y0), y1 = ci(Y1), x, y;
	float dx = X1 - X0, dy = Y1 - Y0;

	if (x0 < clx0) x0 = clx0;
	if (y0 < cly0) y0 = cly0;
	if (x1 > clx1) x1 = clx1;
	if (y1 > cly1) y1 = cly1;
	if (x0 >= x1 || y0 >= y1 || dx <= 0.0f || dy <= 0.0f)
		return;
	for (y = y0; y < y1; y++)
	{
		u32 *p = fb + y * CW;
		float t = ((float)y + 0.5f - Y0) / dy;
		fcol L, R;

		if (flat)
		{
			int r = (int)tl.r, g = (int)tl.g, b = (int)tl.b, w = (int)tl.a;

			for (x = x0; x < x1; x++)
				put(p + x, r, g, b, w);
			continue;
		}
		L.r = tl.r + (bl.r - tl.r) * t; L.g = tl.g + (bl.g - tl.g) * t; L.b = tl.b + (bl.b - tl.b) * t; L.a = tl.a + (bl.a - tl.a) * t;
		R.r = tr.r + (br.r - tr.r) * t; R.g = tr.g + (br.g - tr.g) * t; R.b = tr.b + (br.b - tr.b) * t; R.a = tr.a + (br.a - tr.a) * t;
		{
			/* 16.16 fixed point along the row: four conversions per row, not per pixel (v0.10) */
			float s0 = ((float)x0 + 0.5f - X0) / dx, k = 65536.0f / dx;
			int r = (int)((L.r + (R.r - L.r) * s0) * 65536.0f), dr = (int)((R.r - L.r) * k);
			int g = (int)((L.g + (R.g - L.g) * s0) * 65536.0f), dg = (int)((R.g - L.g) * k);
			int b = (int)((L.b + (R.b - L.b) * s0) * 65536.0f), db = (int)((R.b - L.b) * k);
			int a = (int)((L.a + (R.a - L.a) * s0) * 65536.0f), da = (int)((R.a - L.a) * k);

			for (x = x0; x < x1; x++, r += dr, g += dg, b += db, a += da)
			{
				int rr = r >> 16, gg = g >> 16, bb = b >> 16, aa = a >> 16;

				put(p + x, rr < 0 ? 0 : rr > 255 ? 255 : rr, gg < 0 ? 0 : gg > 255 ? 255 : gg, bb < 0 ? 0 : bb > 255 ? 255 : bb, aa);
			}
		}
	}
}

void rect(float x0, float y0, float x1, float y1, col_t c)
{
	fcol f = fc(c);

	crect(XC(x0), YC(y0), XC(x1), YC(y1), f, f, f, f, 1);
}

void hgrad(float x0, float y0, float x1, float y1, col_t ca, col_t cb)
{
	fcol a = fc(ca), b = fc(cb);

	crect(XC(x0), YC(y0), XC(x1), YC(y1), a, b, a, b, 0);
}

void vgrad(float x0, float y0, float x1, float y1, col_t ca, col_t cb)
{
	fcol a = fc(ca), b = fc(cb);

	crect(XC(x0), YC(y0), XC(x1), YC(y1), a, a, b, b, 0);
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* the exact cover test of ctri for pixel x of a row (row constants c0, c1, a0, a1) */
static inline int inside_px(float c0, float c1, float a0, float a1, float x1v, float x2v, int x)
{
	float px = (float)x + 0.5f + 1.0f / 512.0f;
	float w0 = c0 + a0 * (px - x1v), w1 = c1 + a1 * (px - x2v);

	return !(w0 < 0.0f || w1 < 0.0f || 1.0f - w0 - w1 < 0.0f);
}

/* canvas-space triangle; tex may be NULL (then uv is ignored) */
static void ctri(const float *X, const float *Y, const fcol *c, const tex_t *tex, const float *U, const float *V)
{
	float area = edge(X[0], Y[0], X[1], Y[1], X[2], Y[2]), mnx, mxx, mny, mxy, inv, a0, a1;
	int x0, x1, y0, y1, x, y, flat;

	if (area > -1e-4f && area < 1e-4f)
		return;
	mnx = fminf(X[0], fminf(X[1], X[2]));
	mxx = fmaxf(X[0], fmaxf(X[1], X[2]));
	mny = fminf(Y[0], fminf(Y[1], Y[2]));
	mxy = fmaxf(Y[0], fmaxf(Y[1], Y[2]));
	x0 = ci(mnx); x1 = ci(mxx) + 1; y0 = ci(mny); y1 = ci(mxy) + 1;
	if (x0 < clx0) x0 = clx0;
	if (y0 < cly0) y0 = cly0;
	if (x1 > clx1) x1 = clx1;
	if (y1 > cly1) y1 = cly1;
	inv = 1.0f / area;
	a0 = -(Y[2] - Y[1]) * inv;		/* w0 = (edge 1->2) / area, linear in px along a row */
	a1 = -(Y[0] - Y[2]) * inv;		/* w1 = (edge 2->0) / area */
	flat = !tex && c[0].r == c[1].r && c[0].r == c[2].r && c[0].g == c[1].g && c[0].g == c[2].g &&
		c[0].b == c[1].b && c[0].b == c[2].b && c[0].a == c[1].a && c[0].a == c[2].a;

	/* 2026-11-22 v0.10: each row is walked only between the triangle's edges (the span is found
	 * from the three weights, widened by a pixel, and every pixel still passes the exact test
	 * below), with no division per pixel. v0.9 tested every pixel of the bounding box with two
	 * divisions each: 68% of a PS2-list frame, most of it outside thin lines and the big prisms. */
	for (y = y0; y < y1; y++)
	{
		u32 *p = fb + y * CW;
		/* sample points a hair off the pixel centres: a shared edge through a centre (a disc's
		 * diameter) rounded it out of BOTH triangles; off-centre it belongs to exactly one */
		float py = (float)y + 0.5f + 1.0f / 1024.0f;
		float c0 = (X[2] - X[1]) * (py - Y[1]) * inv, c1 = (X[0] - X[2]) * (py - Y[2]) * inv;
		/* the three weights as m * px + q, for the span */
		float m[3] = { a0, a1, -a0 - a1 };
		float q[3] = { c0 - a0 * X[1], c1 - a1 * X[2], 1.0f - (c0 - a0 * X[1]) - (c1 - a1 * X[2]) };
		float lo = -1.0e9f, hi = 1.0e9f;
		int k, xs, xe;

		for (k = 0; k < 3; k++)
		{
			if (m[k] > 1e-7f)
				lo = fmaxf(lo, -q[k] / m[k]);
			else if (m[k] < -1e-7f)
				hi = fminf(hi, -q[k] / m[k]);
		}
		if (lo > hi + 2.0f)
			continue;
		xs = lo < (float)x0 ? x0 : (int)floorf(lo - 0.5f) - 1;
		xe = hi > (float)x1 ? x1 : (int)ceilf(hi - 0.5f) + 2;
		if (xs < x0) xs = x0;
		if (xe > x1) xe = x1;

		/* v0.22: the covered pixels of a row are ONE run (a triangle is convex), so the exact
		 * inside test is made only at the run's two ends, walking in from the widened span; the
		 * pixels between are filled without it. v0.10-v0.21 tested every pixel, and the three
		 * big backdrop prisms (~170,000 blended pixels a frame) were 25 of a menu frame's 55 ms. */
#define TRI_IN(xx) inside_px(c0, c1, a0, a1, X[1], X[2], (xx))
		while (xs < xe && !TRI_IN(xs))
			xs++;
		while (xe > xs && !TRI_IN(xe - 1))
			xe--;
#undef TRI_IN
		if (xs >= xe)
			continue;

		if (flat)
		{
			int fr = (int)c[0].r, fg = (int)c[0].g, fbb = (int)c[0].b, fa = (int)c[0].a;

			for (x = xs; x < xe; x++)
				put(p + x, fr, fg, fbb, fa);
			continue;
		}

		if (!tex)
		{
			/* colour along the row in 16.16 fixed point, stepped per pixel (v0.10): the colour is
			 * linear in px, so four conversions per row replace four per pixel */
			float px0 = (float)xs + 0.5f + 1.0f / 512.0f;
			float w0 = c0 + a0 * (px0 - X[1]), w1 = c1 + a1 * (px0 - X[2]), w2 = 1.0f - w0 - w1, a2 = -a0 - a1;
			int r = (int)((w0 * c[0].r + w1 * c[1].r + w2 * c[2].r) * 65536.0f), dr = (int)((a0 * c[0].r + a1 * c[1].r + a2 * c[2].r) * 65536.0f);
			int g = (int)((w0 * c[0].g + w1 * c[1].g + w2 * c[2].g) * 65536.0f), dg = (int)((a0 * c[0].g + a1 * c[1].g + a2 * c[2].g) * 65536.0f);
			int b = (int)((w0 * c[0].b + w1 * c[1].b + w2 * c[2].b) * 65536.0f), db = (int)((a0 * c[0].b + a1 * c[1].b + a2 * c[2].b) * 65536.0f);
			int a = (int)((w0 * c[0].a + w1 * c[1].a + w2 * c[2].a) * 65536.0f), da = (int)((a0 * c[0].a + a1 * c[1].a + a2 * c[2].a) * 65536.0f);

			for (x = xs; x < xe; x++, r += dr, g += dg, b += db, a += da)
			{
				int rr, gg, bb, aa;

				rr = r >> 16; gg = g >> 16; bb = b >> 16; aa = a >> 16;
				put(p + x, rr < 0 ? 0 : rr > 255 ? 255 : rr, gg < 0 ? 0 : gg > 255 ? 255 : gg, bb < 0 ? 0 : bb > 255 ? 255 : bb, aa < 0 ? 0 : aa);
			}
			continue;
		}

		/* v0.27: textured rows stepped in 16.16 fixed point like the untextured ones (colour, u, v
		 * are linear in px), with the texel modulation in integers; the span is already trimmed to
		 * the covered run above. v0.10-v0.26 recomputed every weight, colour and u/v in floats per
		 * pixel: the Memory Card screen's turning 3D save icons drew at 8 fps (105 ms a frame on
		 * the console, owner: "kind of laggy", 2026-11-23). */
		{
			float px0 = (float)xs + 0.5f + 1.0f / 512.0f;
			float w0 = c0 + a0 * (px0 - X[1]), w1 = c1 + a1 * (px0 - X[2]), w2 = 1.0f - w0 - w1, a2 = -a0 - a1;
			int r = (int)((w0 * c[0].r + w1 * c[1].r + w2 * c[2].r) * 65536.0f), dr = (int)((a0 * c[0].r + a1 * c[1].r + a2 * c[2].r) * 65536.0f);
			int g = (int)((w0 * c[0].g + w1 * c[1].g + w2 * c[2].g) * 65536.0f), dg = (int)((a0 * c[0].g + a1 * c[1].g + a2 * c[2].g) * 65536.0f);
			int b = (int)((w0 * c[0].b + w1 * c[1].b + w2 * c[2].b) * 65536.0f), db = (int)((a0 * c[0].b + a1 * c[1].b + a2 * c[2].b) * 65536.0f);
			int a = (int)((w0 * c[0].a + w1 * c[1].a + w2 * c[2].a) * 65536.0f), da = (int)((a0 * c[0].a + a1 * c[1].a + a2 * c[2].a) * 65536.0f);
			int u = (int)((w0 * U[0] + w1 * U[1] + w2 * U[2]) * 65536.0f), du = (int)((a0 * U[0] + a1 * U[1] + a2 * U[2]) * 65536.0f);
			int v = (int)((w0 * V[0] + w1 * V[1] + w2 * V[2]) * 65536.0f), dv = (int)((a0 * V[0] + a1 * V[1] + a2 * V[2]) * 65536.0f);
			int tw = tex->w, th = tex->h;

			for (x = xs; x < xe; x++, r += dr, g += dg, b += db, a += da, u += du, v += dv)
			{
				int tu = u >> 16, tv = v >> 16, rr = r >> 16, gg = g >> 16, bb = b >> 16, aa = a >> 16;
				const u8 *t;

				if (tu < 0) tu = 0;
				if (tv < 0) tv = 0;
				if (tu >= tw) tu = tw - 1;
				if (tv >= th) tv = th - 1;
				t = tex->px + (tv * tw + tu) * 4;
				rr = rr < 0 ? 0 : (rr * t[0]) >> 7;
				gg = gg < 0 ? 0 : (gg * t[1]) >> 7;
				bb = bb < 0 ? 0 : (bb * t[2]) >> 7;
				if (t[3] < 0x80)
					aa = (aa * t[3]) >> 7;
				put(p + x, rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb, aa);
			}
		}
	}
}

void canvas_point(float x, float y, int *cx, int *cy)
{
	*cx = ci(XC(x));
	*cy = ci(YC(y));
}

void blit_keyed(const u32 *src, int w, int h, u32 key, float lx, float ly, int ox, int oy, int a)
{
	int X0, Y0, x, y, wt = a >= 0x80 ? 256 : a * 2;

	canvas_point(lx, ly, &X0, &Y0);
	X0 += ox;
	Y0 += oy;
	for (y = 0; y < h; y++)
	{
		int Y = Y0 + y;
		const u32 *s = src + y * w;
		u32 *p;

		if (Y < cly0 || Y >= cly1)
			continue;
		p = fb + Y * CW;
		for (x = 0; x < w; x++)
		{
			int X = X0 + x;
			u32 c = s[x];

			if (c == key || X < clx0 || X >= clx1)
				continue;
			put(p + X, (int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF), wt);
		}
	}
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, col_t c0, col_t c1, col_t c2)
{
	float X[3] = { XC(x0), XC(x1), XC(x2) }, Y[3] = { YC(y0), YC(y1), YC(y2) };
	fcol c[3] = { fc(c0), fc(c1), fc(c2) };

	ctri(X, Y, c, NULL, NULL, NULL);
}

void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, col_t c0, col_t c1, col_t c2, col_t c3)
{
	if (x0 == x2 && x1 == x3 && y0 == y1 && y2 == y3 && x1 > x0 && y2 > y0)
	{
		crect(XC(x0), YC(y0), XC(x1), YC(y2), fc(c0), fc(c1), fc(c2), fc(c3), 0);
		return;
	}
	tri(x0, y0, x1, y1, x2, y2, c0, c1, c2);
	tri(x1, y1, x3, y3, x2, y2, c1, c3, c2);
}

static void line_w(float x0, float y0, float x1, float y1, col_t c, float w)
{
	float X0 = XC(x0), Y0 = YC(y0), X1 = XC(x1), Y1 = YC(y1), dx = X1 - X0, dy = Y1 - Y0, n = sqrtf(dx * dx + dy * dy), nx, ny;
	float X[3], Y[3];
	fcol f = fc(c), cc[3] = { f, f, f };

	if (n < 1e-3f)
		return;
	nx = -dy / n * w * 0.5f;
	ny = dx / n * w * 0.5f;
	X[0] = X0 + nx; Y[0] = Y0 + ny; X[1] = X1 + nx; Y[1] = Y1 + ny; X[2] = X0 - nx; Y[2] = Y0 - ny;
	ctri(X, Y, cc, NULL, NULL, NULL);
	X[0] = X1 + nx; Y[0] = Y1 + ny; X[1] = X1 - nx; Y[1] = Y1 - ny; X[2] = X0 - nx; Y[2] = Y0 - ny;
	ctri(X, Y, cc, NULL, NULL, NULL);
}

void line(float x0, float y0, float x1, float y1, col_t c)
{
	line_w(x0, y0, x1, y1, c, 2.0f * S);
}

void stroke(float x0, float y0, float x1, float y1, col_t c)
{
	line_w(x0 + 0.5f, y0 + 0.5f, x1 + 0.5f, y1 + 0.5f, c, 2.0f * S);
}

void frame_rect(float x0, float y0, float x1, float y1, float t, col_t c)
{
	rect(x0, y0, x1, y0 + t, c);
	rect(x0, y1 - t, x1, y1, c);
	rect(x0, y0 + t, x0 + t, y1 - t, c);
	rect(x1 - t, y0 + t, x1, y1 - t, c);
}

/* bilinear sample, texel centres at i + 0.5 */
static void sample(const tex_t *t, float u, float v, float *o)
{
	int i0, j0, i1, j1, k;
	float fu, fv;
	const u8 *a, *b, *c, *d;

	u -= 0.5f;
	v -= 0.5f;
	if (u < 0.0f) u = 0.0f;
	if (v < 0.0f) v = 0.0f;
	i0 = (int)u; j0 = (int)v;
	fu = u - i0; fv = v - j0;
	if (i0 >= t->w - 1) { i0 = t->w - 1; fu = 0.0f; }
	if (j0 >= t->h - 1) { j0 = t->h - 1; fv = 0.0f; }
	i1 = i0 + 1 < t->w ? i0 + 1 : i0;
	j1 = j0 + 1 < t->h ? j0 + 1 : j0;
	a = t->px + (j0 * t->w + i0) * 4; b = t->px + (j0 * t->w + i1) * 4;
	c = t->px + (j1 * t->w + i0) * 4; d = t->px + (j1 * t->w + i1) * 4;
	for (k = 0; k < 4; k++)
		o[k] = (a[k] * (1 - fu) + b[k] * fu) * (1 - fv) + (c[k] * (1 - fu) + d[k] * fu) * fv;
}

/* one texel coordinate (texel centres at i + 0.5) -> left index, right index, 8-bit fraction */
static void taps(float u, int n, int *i0, int *i1, int *f)
{
	int i;

	u -= 0.5f;
	if (u < 0.0f)
		u = 0.0f;
	i = (int)u;
	*f = (int)((u - (float)i) * 256.0f);
	if (i >= n - 1)
	{
		i = n - 1;
		*f = 0;
	}
	*i0 = i;
	*i1 = i + 1 < n ? i + 1 : i;
}

/* 2026-11-22 v0.10: integer bilinear, the column taps worked out once per sprite. v0.9 called
 * sample() per pixel in floats and converted r, g, b, a to int per pixel: on the PPU every
 * float -> int conversion stalls (load-hit-store), and the covers were half of a PS2-list frame. */
void sprite_tex(const tex_t *t, float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1, col_t c)
{
	static int cu0[CW], cu1[CW], cfu[CW];
	float X0 = XC(x0), Y0 = YC(y0), X1 = XC(x1), Y1 = YC(y1), du, dv;
	int ix0 = ci(X0), ix1 = ci(X1), iy0 = ci(Y0), iy1 = ci(Y1), x, y;
	int mr = (int)(c >> 24), mg = (int)((c >> 16) & 0xFF), mb = (int)((c >> 8) & 0xFF);
	int ma = (c & 0xFF) >= 0x80 ? 256 : (int)(c & 0xFF) * 2;

	if (!t || !t->px || X1 <= X0 || Y1 <= Y0 || ma <= 0)
		return;
	if (ix0 < clx0) ix0 = clx0;
	if (iy0 < cly0) iy0 = cly0;
	if (ix1 > clx1) ix1 = clx1;
	if (iy1 > cly1) iy1 = cly1;
	du = (u1 - u0) / (X1 - X0);
	dv = (v1 - v0) / (Y1 - Y0);
	for (x = ix0; x < ix1; x++)
	{
		int i0, i1, f;

		taps(u0 + ((float)x + 0.5f - X0) * du, t->w, &i0, &i1, &f);
		cu0[x] = i0 * 4;
		cu1[x] = i1 * 4;
		cfu[x] = f;
	}
	for (y = iy0; y < iy1; y++)
	{
		u32 *p = fb + y * CW;
		int j0, j1, fv, gv;
		const u8 *r0, *r1;

		taps(v0 + ((float)y + 0.5f - Y0) * dv, t->h, &j0, &j1, &fv);
		gv = 256 - fv;
		r0 = t->px + j0 * t->w * 4;
		r1 = t->px + j1 * t->w * 4;
		for (x = ix0; x < ix1; x++)
		{
			const u8 *a = r0 + cu0[x], *b = r0 + cu1[x], *e = r1 + cu0[x], *d = r1 + cu1[x];
			int fu = cfu[x], gu = 256 - fu, s0, s1, s2, s3, r, g, bl, w;

			s0 = ((a[0] * gu + b[0] * fu) * gv + (e[0] * gu + d[0] * fu) * fv) >> 16;
			s1 = ((a[1] * gu + b[1] * fu) * gv + (e[1] * gu + d[1] * fu) * fv) >> 16;
			s2 = ((a[2] * gu + b[2] * fu) * gv + (e[2] * gu + d[2] * fu) * fv) >> 16;
			s3 = ((a[3] * gu + b[3] * fu) * gv + (e[3] * gu + d[3] * fu) * fv) >> 16;
			r = (s0 * mr) >> 7;
			g = (s1 * mg) >> 7;
			bl = (s2 * mb) >> 7;
			w = s3 >= 128 ? ma : (ma * s3) >> 7;
			put(p + x, r > 255 ? 255 : r, g > 255 ? 255 : g, bl > 255 ? 255 : bl, w);
		}
	}
}

void tri_tex(const tex_t *t, const float *xy, const float *uv, const col_t *c)
{
	float X[3], Y[3], U[3], V[3];
	fcol f[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		X[k] = XC(xy[2 * k]);
		Y[k] = YC(xy[2 * k + 1]);
		U[k] = uv[2 * k];
		V[k] = uv[2 * k + 1];
		f[k] = fc(c[k]);
	}
	ctri(X, Y, f, t, U, V);
}

void scissor(float x0, float x1, float y0, float y1)
{
	clx0 = ci(XC(x0)); clx1 = ci(XC(x1)); cly0 = ci(YC(y0)); cly1 = ci(YC(y1));
	if (clx0 < 0) clx0 = 0;
	if (cly0 < 0) cly0 = 0;
	if (clx1 > CW) clx1 = CW;
	if (cly1 > CH) cly1 = CH;
}

void scissor_reset(void)
{
	clx0 = 0; clx1 = CW; cly0 = 0; cly1 = CH;
}

/* ---- text ---- */

static const glyph_t *glyph(int f, unsigned c)
{
	if (c < FONT_FIRST || c > FONT_LAST)
		c = '?';
	return &font_glyphs[f][c - FONT_FIRST];
}

int font_h(int f)
{
	return (int)(font_height[f] / S + 0.5f);
}

int text_w(int f, const char *s, int track)
{
	float w = 0.0f;

	for (; *s; s++)
		w += glyph(f, (unsigned char)*s)->adv / S + track;
	return (int)(w + 0.5f);
}

int text_ex(int f, int x, int y, float scale, int track, col_t c, const char *s)
{
	float pen = XC((float)x), top = YC((float)y);
	fcol m = fc(c);
	/* integers per pixel (v0.10): the glyph step in 16.16 and the colour once per string */
	int mr = (int)m.r, mg = (int)m.g, mb = (int)m.b, ma = (int)m.a;
	int step = (int)(65536.0f / scale);

	for (; *s; s++)
	{
		const glyph_t *g = glyph(f, (unsigned char)*s);

		if (g->w)
		{
			int gw = (int)(g->w * scale + 0.5f), gh = (int)(g->h * scale + 0.5f), i, j;
			int bx = (int)(pen + g->ox * scale + 0.5f), by = (int)(top + g->oy * scale + 0.5f);

			for (j = 0; j < gh; j++)
			{
				int py = by + j, sv = (j * step) >> 16, su16 = 0;
				const unsigned char *row;
				u32 *dst;

				if (py < cly0 || py >= cly1 || sv >= g->h)
					continue;
				row = font_cov + (g->v + sv) * FONT_TEX_W + g->u;
				dst = fb + py * CW;
				for (i = 0; i < gw; i++, su16 += step)
				{
					int px = bx + i, su = su16 >> 16;
					unsigned cov;

					if (px < clx0 || px >= clx1 || su >= g->w)
						continue;
					cov = row[su];
					if (cov)
						put(dst + px, mr, mg, mb, (ma * (int)cov * 257 + 32768) >> 16);
				}
			}
		}
		pen += (g->adv + track * S) * scale;
	}
	return (int)((pen - OX) / S + 0.5f);
}

int text(int f, int x, int y, col_t c, const char *s)
{
	return text_ex(f, x, y, 1.0f, 0, c, s);
}

void text_centre(int f, int cx, int y, int track, col_t c, const char *s)
{
	text_ex(f, cx - (text_w(f, s, track) - track) / 2, y, 1.0f, track, c, s);
}

void fit(char *out, unsigned n, int f, const char *s, int w)
{
	unsigned len;

	strncpy(out, s, n - 1);
	out[n - 1] = 0;
	len = strlen(out);
	while (len > 4 && text_w(f, out, 0) > w)
	{
		len--;
		strcpy(out + len - 3, "...");
	}
}
