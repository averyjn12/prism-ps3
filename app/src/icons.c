/* The HOME tiles' pictures (v0.15): each console drawn from plain shapes in the app's own
 * style, in place of the same disc on every tile. Our own drawings of the hardware's outline:
 * no Sony artwork, logo or lettering. s is the picture's width in logical pixels; the picture
 * is centred on (cx, cy). */
#include "ui.h"
#include "look.h"
#include "icons.h"

static col_t A(int r, int g, int b, int a, int al)
{
	return RGBA(r, g, b, a * al / 0x80);
}

/* a filled ellipse, as a fan of 24 triangles */
static void ellipse(float cx, float cy, float rx, float ry, col_t c)
{
	int k;

	for (k = 0; k < 24; k++)
	{
		float a0 = k / 24.0f, a1 = (k + 1) / 24.0f;

		tri(cx, cy, cx + rx * fcos(a0), cy + ry * fsin(a0), cx + rx * fcos(a1), cy + ry * fsin(a1), c, c, c);
	}
}

static void ring(float cx, float cy, float rx, float ry, col_t c)
{
	int k;

	for (k = 0; k < 24; k++)
	{
		float a0 = k / 24.0f, a1 = (k + 1) / 24.0f;

		line(cx + rx * fcos(a0), cy + ry * fsin(a0), cx + rx * fcos(a1), cy + ry * fsin(a1), c);
	}
}

static void shadow(float cx, float cy, float rx, float ry, int al)
{
	ellipse(cx, cy, rx, ry, A(0x00, 0x00, 0x00, 0x30, al));
}

/* the grey box with the round lid on top (1994) */
static void ps1(float cx, float cy, float s, int al, float t, int chosen)
{
	float x0 = cx - s * 0.5f, x1 = cx + s * 0.5f, ty = cy - s * 0.20f, dp = s * 0.34f, fh = s * 0.15f, in = s * 0.07f;
	float fy = ty + dp, ly = ty + dp * 0.50f, lx = x0 + s * 0.37f;
	int k;

	shadow(cx, fy + fh + s * 0.03f, s * 0.54f, s * 0.06f, al);
	/* top face, front face */
	quad(x0 + in, ty, x1 - in, ty, x0, fy, x1, fy, A(0xD9, 0xDC, 0xE6, 0x80, al), A(0xCF, 0xD3, 0xDE, 0x80, al),
		A(0xC4, 0xC8, 0xD4, 0x80, al), A(0xBA, 0xBE, 0xCB, 0x80, al));
	vgrad(x0, fy, x1, fy + fh, A(0x9C, 0xA1, 0xB0, 0x80, al), A(0x7C, 0x81, 0x91, 0x80, al));
	line(x0, fy, x1, fy, A(0xF0, 0xF2, 0xF8, 0x80, al));
	/* the lid: a shallow dish, a hub, and a light that moves round it when chosen */
	ellipse(lx, ly, s * 0.25f, s * 0.115f, A(0xB4, 0xB9, 0xC6, 0x80, al));
	ring(lx, ly, s * 0.25f, s * 0.115f, A(0x8A, 0x8F, 0x9E, 0x80, al));
	ring(lx, ly, s * 0.20f, s * 0.092f, A(0xE6, 0xE9, 0xF1, 0x50, al));
	ellipse(lx, ly, s * 0.035f, s * 0.016f, A(0x8A, 0x8F, 0x9E, 0x80, al));
	if (chosen)
		for (k = 0; k < 3; k++)
		{
			float a = t / 4.0f + k * 0.012f;

			line(lx + s * 0.06f * fcos(a), ly + s * 0.028f * fsin(a), lx + s * 0.19f * fcos(a), ly + s * 0.088f * fsin(a),
				A(0xFF, 0xFF, 0xFF, 0x40 - k * 0x10, al));
		}
	/* three round buttons on the right */
	ellipse(x0 + s * 0.74f, ty + dp * 0.36f, s * 0.045f, s * 0.021f, A(0x9A, 0x9F, 0xAE, 0x80, al));
	ellipse(x0 + s * 0.86f, ty + dp * 0.36f, s * 0.045f, s * 0.021f, A(0x9A, 0x9F, 0xAE, 0x80, al));
	ellipse(x0 + s * 0.80f, ty + dp * 0.72f, s * 0.06f, s * 0.027f, A(0x9A, 0x9F, 0xAE, 0x80, al));
	/* the front: two pad ports, two card slots above them */
	for (k = 0; k < 2; k++)
	{
		float px = x0 + s * (0.10f + 0.16f * k);

		rect(px, fy + fh * 0.18f, px + s * 0.11f, fy + fh * 0.30f, A(0x3A, 0x3E, 0x4C, 0x80, al));
		rect(px + s * 0.01f, fy + fh * 0.50f, px + s * 0.10f, fy + fh * 0.84f, A(0x2A, 0x2E, 0x3A, 0x80, al));
	}
	spectrum_bar(x1 - s * 0.30f, x1 - s * 0.08f, fy + fh * 0.45f, 2.0f, 4, chosen ? al : al / 2);
}

/* the black slab, standing on its end with its ribs (2000) */
static void ps2(float cx, float cy, float s, int al, float t, int chosen)
{
	float w = s * 0.27f, h = s * 0.80f, dx = s * 0.17f, dy = s * 0.07f;
	float x0 = cx - s * 0.16f, x1 = x0 + w, y0 = cy - h * 0.52f, y1 = y0 + h;
	float glow = chosen ? 0.55f + 0.45f * fsin(t / 2.5f) : 0.35f;
	int k;

	shadow(cx + s * 0.02f, y1 + s * 0.035f, s * 0.36f, s * 0.05f, al);
	/* the side we see, ribbed, and the front */
	quad(x1, y0, x1 + dx, y0 - dy, x1, y1, x1 + dx, y1 - dy, A(0x2A, 0x2E, 0x3C, 0x80, al), A(0x1E, 0x21, 0x2C, 0x80, al),
		A(0x16, 0x18, 0x22, 0x80, al), A(0x10, 0x12, 0x1A, 0x80, al));
	for (k = 1; k < 9; k++)
	{
		float f = k / 9.0f;

		line(x1 + dx * 0.12f, y0 + (y1 - y0) * f - dy * 0.12f, x1 + dx * 0.92f, y0 + (y1 - y0) * f - dy * 0.92f,
			A(0x4A, 0x50, 0x64, 0x80, al));
	}
	vgrad(x0, y0, x1, y1, A(0x24, 0x27, 0x34, 0x80, al), A(0x12, 0x14, 0x1C, 0x80, al));
	quad(x0, y0, x1, y0, x0 + dx, y0 - dy, x1 + dx, y0 - dy, A(0x3A, 0x3F, 0x52, 0x80, al), A(0x3A, 0x3F, 0x52, 0x80, al),
		A(0x2E, 0x32, 0x42, 0x80, al), A(0x2E, 0x32, 0x42, 0x80, al));
	line(x0, y0, x0, y1, A(0x5A, 0x61, 0x7A, 0x80, al));
	/* the tray's edge down the front, a groove across it, two ports low down */
	rect(x0 + w * 0.70f, y0 + h * 0.05f, x0 + w * 0.74f, y0 + h * 0.62f, A(0x06, 0x08, 0x0E, 0x80, al));
	rect(x0 + w * 0.08f, y0 + h * 0.66f, x0 + w * 0.92f, y0 + h * 0.67f, A(0x40, 0x45, 0x58, 0x80, al));
	rect(x0 + w * 0.18f, y0 + h * 0.78f, x0 + w * 0.42f, y0 + h * 0.83f, A(0x06, 0x08, 0x0E, 0x80, al));
	rect(x0 + w * 0.58f, y0 + h * 0.78f, x0 + w * 0.82f, y0 + h * 0.83f, A(0x06, 0x08, 0x0E, 0x80, al));
	/* a light at the top of the front */
	rect(x0 + w * 0.20f, y0 + h * 0.70f, x0 + w * 0.34f, y0 + h * 0.715f, A(0x4F, 0xB6, 0xFF, (int)(0x80 * glow), al));
	rect(x0 + w * 0.50f, y0 + h * 0.70f, x0 + w * 0.64f, y0 + h * 0.715f, A(0x4F, 0xB6, 0xFF, (int)(0x80 * glow), al));
	/* the stand under it */
	quad(x0 - s * 0.03f, y1, x1 + s * 0.03f, y1, x0 - s * 0.03f, y1 + s * 0.025f, x1 + s * 0.03f, y1 + s * 0.025f,
		A(0x30, 0x34, 0x44, 0x80, al), A(0x30, 0x34, 0x44, 0x80, al), A(0x1A, 0x1C, 0x26, 0x80, al), A(0x1A, 0x1C, 0x26, 0x80, al));
}

/* the wide, curved, glossy one lying down (2006) */
static void ps3(float cx, float cy, float s, int al, float t, int chosen)
{
	enum { N = 14 };
	float x0 = cx - s * 0.5f, yb = cy + s * 0.16f, eh = s * 0.13f, bump = s * 0.07f, depth = s * 0.20f, inset = s * 0.08f;
	float sweep = chosen ? fsin(t / 6.0f) * 0.5f + 0.5f : -1.0f;
	int k;

	shadow(cx, yb + s * 0.035f, s * 0.54f, s * 0.06f, al);
	for (k = 0; k < N; k++)
	{
		float u0 = (float)k / N, u1 = (float)(k + 1) / N;
		float a0 = x0 + s * u0, a1 = x0 + s * u1;
		/* the front's top edge: a gentle arch */
		float h0 = eh + bump * (1.0f - (2.0f * u0 - 1.0f) * (2.0f * u0 - 1.0f));
		float h1 = eh + bump * (1.0f - (2.0f * u1 - 1.0f) * (2.0f * u1 - 1.0f));
		/* the top, going back and in */
		float b0 = x0 + inset + (s - 2.0f * inset) * u0, b1 = x0 + inset + (s - 2.0f * inset) * u1;
		float g0 = 0.30f + 0.70f * (1.0f - (2.0f * u0 - 1.0f) * (2.0f * u0 - 1.0f)), g1 = 0.30f + 0.70f * (1.0f - (2.0f * u1 - 1.0f) * (2.0f * u1 - 1.0f));
		int hl0 = 0x24 + (int)(0x2E * g0), hl1 = 0x24 + (int)(0x2E * g1);
		float sw0 = sweep >= 0.0f ? 1.0f - 6.0f * (u0 - sweep < 0 ? sweep - u0 : u0 - sweep) : 0.0f;
		float sw1 = sweep >= 0.0f ? 1.0f - 6.0f * (u1 - sweep < 0 ? sweep - u1 : u1 - sweep) : 0.0f;

		if (sw0 < 0.0f)
			sw0 = 0.0f;
		if (sw1 < 0.0f)
			sw1 = 0.0f;
		hl0 += (int)(0x30 * sw0);
		hl1 += (int)(0x30 * sw1);
		quad(b0, yb - h0 - depth, b1, yb - h1 - depth, a0, yb - h0, a1, yb - h1, A(0x14, 0x16, 0x20, 0x80, al), A(0x14, 0x16, 0x20, 0x80, al),
			A(hl0, hl0 + 2, hl0 + 10, 0x80, al), A(hl1, hl1 + 2, hl1 + 10, 0x80, al));
		quad(a0, yb - h0, a1, yb - h1, a0, yb, a1, yb, A(0x22, 0x25, 0x31, 0x80, al), A(0x22, 0x25, 0x31, 0x80, al),
			A(0x0C, 0x0D, 0x14, 0x80, al), A(0x0C, 0x0D, 0x14, 0x80, al));
		/* the disc slot, following the arch */
		if (k >= 2 && k < N - 3)
			line(a0, yb - h0 * 0.62f, a1, yb - h1 * 0.62f, A(0x7A, 0x82, 0x9C, 0x80, al));
		line(a0, yb - h0, a1, yb - h1, A(0x8A, 0x92, 0xAC, 0x70, al));
	}
	/* a bright end cap on the right, two touch spots */
	quad(x0 + s - s * 0.02f, yb - eh - s * 0.005f, x0 + s, yb - eh, x0 + s - s * 0.02f, yb, x0 + s, yb,
		A(0xC8, 0xCE, 0xDC, 0x60, al), A(0xC8, 0xCE, 0xDC, 0x90 > 0x80 ? 0x80 : 0x90, al), A(0x80, 0x86, 0x96, 0x60, al), A(0x80, 0x86, 0x96, 0x80, al));
	ellipse(x0 + s * 0.84f, yb - eh * 0.38f, s * 0.012f, s * 0.008f, A(0x7F, 0xE3, 0xFF, chosen ? 0x80 : 0x40, al));
	ellipse(x0 + s * 0.89f, yb - eh * 0.38f, s * 0.012f, s * 0.008f, A(0x7F, 0xE3, 0xFF, chosen ? 0x80 : 0x40, al));
}

/* the wide handheld (2004): screen in the middle, pad left, four buttons right */
static void psp(float cx, float cy, float s, int al, float t, int chosen)
{
	float w = s * 0.50f, h = s * 0.21f, r = h, x0 = cx - w, x1 = cx + w, y0 = cy - h, y1 = cy + h;
	float sx0 = cx - s * 0.25f, sx1 = cx + s * 0.25f, sy0 = cy - h * 0.72f, sy1 = cy + h * 0.72f;
	float glint = chosen ? fsin(t / 5.0f) * 0.5f + 0.5f : 0.3f;
	int k;

	shadow(cx, y1 + s * 0.06f, s * 0.50f, s * 0.05f, al);
	/* the body: a bar with round ends */
	vgrad(x0 + r * 0.6f, y0, x1 - r * 0.6f, y1, A(0x2C, 0x30, 0x40, 0x80, al), A(0x12, 0x14, 0x1C, 0x80, al));
	ellipse(x0 + r * 0.6f, cy, r * 0.6f, h, A(0x22, 0x25, 0x32, 0x80, al));
	ellipse(x1 - r * 0.6f, cy, r * 0.6f, h, A(0x22, 0x25, 0x32, 0x80, al));
	line(x0 + r * 0.6f, y0, x1 - r * 0.6f, y0, A(0x6A, 0x72, 0x8C, 0x80, al));
	/* the screen, with a slow light across it */
	rect(sx0 - 2.0f, sy0 - 2.0f, sx1 + 2.0f, sy1 + 2.0f, A(0x06, 0x08, 0x0E, 0x80, al));
	vgrad(sx0, sy0, sx1, sy1, A(0x1A, 0x2A, 0x6A, 0x80, al), A(0x2A, 0x15, 0x52, 0x80, al));
	quad(sx0 + (sx1 - sx0) * glint, sy0, sx0 + (sx1 - sx0) * glint + s * 0.05f, sy0, sx0 + (sx1 - sx0) * glint - s * 0.04f, sy1,
		sx0 + (sx1 - sx0) * glint + s * 0.01f, sy1, A(0xFF, 0xFF, 0xFF, 0x14, al), A(0xFF, 0xFF, 0xFF, 0x00, al),
		A(0xFF, 0xFF, 0xFF, 0x14, al), A(0xFF, 0xFF, 0xFF, 0x00, al));
	spectrum_bar(sx0 + s * 0.04f, sx1 - s * 0.04f, sy1 - s * 0.03f, 2.0f, 4, chosen ? al : al / 2);
	/* the pad: a plus */
	rect(x0 + s * 0.075f, cy - s * 0.012f, x0 + s * 0.165f, cy + s * 0.012f, A(0x8A, 0x92, 0xAC, 0x80, al));
	rect(x0 + s * 0.108f, cy - s * 0.045f, x0 + s * 0.132f, cy + s * 0.045f, A(0x8A, 0x92, 0xAC, 0x80, al));
	/* four buttons in a diamond */
	for (k = 0; k < 4; k++)
	{
		static const float dx[4] = { 0.0f, 0.036f, 0.0f, -0.036f }, dy[4] = { -0.036f, 0.0f, 0.036f, 0.0f };

		ellipse(x1 - s * 0.12f + s * dx[k], cy + s * dy[k], s * 0.014f, s * 0.014f, A(0x8A, 0x92, 0xAC, 0x80, al));
	}
}

/* SETTINGS: a cog */
static void gear(float cx, float cy, float s, int al, float t, int chosen)
{
	float r0 = s * 0.20f, r1 = s * 0.27f, turn = chosen ? t / 24.0f : 0.0f;
	col_t c = A(0xB8, 0xC0, 0xE0, 0x80, al), cd = A(0x5A, 0x64, 0x90, 0x80, al);
	int k;

	shadow(cx, cy + s * 0.33f, s * 0.26f, s * 0.04f, al);
	for (k = 0; k < 10; k++)
	{
		float a = turn + k / 10.0f, w = 0.028f;

		quad(cx + r0 * fcos(a - w), cy + r0 * fsin(a - w), cx + r0 * fcos(a + w), cy + r0 * fsin(a + w),
			cx + r1 * fcos(a - w * 0.7f), cy + r1 * fsin(a - w * 0.7f), cx + r1 * fcos(a + w * 0.7f), cy + r1 * fsin(a + w * 0.7f), c, c, cd, cd);
	}
	ellipse(cx, cy, r0 + 1.0f, r0 + 1.0f, c);
	ellipse(cx, cy, s * 0.085f, s * 0.085f, A(0x0D, 0x14, 0x33, 0x80, al));
	ring(cx, cy, r0 * 0.80f, r0 * 0.80f, cd);
	spectrum_bar(cx - s * 0.12f, cx + s * 0.12f, cy + s * 0.25f + 6.0f, 2.0f, 4, chosen ? al : al / 2);
}

void console_icon(int which, float cx, float cy, float s, int al, float t, int chosen)
{
	if (which == ICON_PS1)
		ps1(cx, cy, s, al, t, chosen);
	else if (which == ICON_PS2)
		ps2(cx, cy, s, al, t, chosen);
	else if (which == ICON_PS3)
		ps3(cx, cy, s, al, t, chosen);
	else if (which == ICON_PSP)
		psp(cx, cy, s, al, t, chosen);
	else
		gear(cx, cy, s, al, t, chosen);
}
