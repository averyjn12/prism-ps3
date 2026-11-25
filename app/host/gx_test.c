/* v0.67: the parts of src/gfx.c the screens never push hard, run on the PC over host/fake_rsx.c:
 * the texture slots (more textures than slots, a slot never reused while a queued frame may read it,
 * other pixels under a kept address), the vertex buffer's end, an empty scissor.
 *   bash host/gx_test.sh          -> "gx_test: N checks, 0 failed"
 * Each texture is one flat colour made from its number, so the picture read back says which texture a
 * square was really drawn with. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define NTEX	400
#define PER_ROW	20
#define CELL	32

static unsigned char *tex[NTEX];
static int checks, failed;

static void check(int ok, const char *what, long a, long b)
{
	checks++;
	if (!ok)
	{
		failed++;
		printf("FAIL: %s (%ld, %ld)\n", what, a, b);
	}
}

static void colour_of(int k, int gen, int *r, int *g, int *b)
{
	*r = 40 + (k * 37 + gen * 91) % 200;
	*g = 40 + (k * 59 + gen * 13) % 200;
	*b = 40 + (k * 83 + gen * 47) % 200;
}

static void fill(int k, int gen)
{
	int r, g, b, i;

	colour_of(k, gen, &r, &g, &b);
	for (i = 0; i < 128 * 128; i++)
	{
		tex[k][4 * i] = (unsigned char)r;
		tex[k][4 * i + 1] = (unsigned char)g;
		tex[k][4 * i + 2] = (unsigned char)b;
		tex[k][4 * i + 3] = 0x80;
	}
}

/* square number slot on the screen, with texture k; 1 = drawn */
static int square(int slot, int k)
{
	float x0 = (float)(slot % PER_ROW * CELL), y0 = (float)(slot / PER_ROW * CELL), x1 = x0 + CELL, y1 = y0 + CELL;
	gxv_t *p = gx_prims(tex[k], 128, 128, 6);

	if (!p)
		return 0;
	gx_put(p + 0, x0, y0, 0, 0, 1, 1, 1, 1); gx_put(p + 1, x1, y0, 128, 0, 1, 1, 1, 1); gx_put(p + 2, x0, y1, 0, 128, 1, 1, 1, 1);
	gx_put(p + 3, x1, y0, 128, 0, 1, 1, 1, 1); gx_put(p + 4, x1, y1, 128, 128, 1, 1, 1, 1); gx_put(p + 5, x0, y1, 0, 128, 1, 1, 1, 1);
	return 1;
}

static unsigned char *shot;

static void read_frame(void)
{
	FILE *f;
	int w, h, mx;

	gfx_present();
	check(gx_dump("/tmp/gx_test.ppm") == 0, "the frame could not be read back", 0, 0);
	f = fopen("/tmp/gx_test.ppm", "rb");
	if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != CW || h != CH)
	{
		check(0, "the frame read back is not 1280 x 720", 0, 0);
		exit(1);
	}
	fgetc(f);
	if (!shot)
		shot = malloc(CW * CH * 3);
	if (fread(shot, 3, CW * CH, f) != CW * CH)
		check(0, "short frame", 0, 0);
	fclose(f);
}

static void expect(int slot, int k, int gen, const char *what)
{
	int x = slot % PER_ROW * CELL + CELL / 2, y = slot / PER_ROW * CELL + CELL / 2, r, g, b;
	const unsigned char *p = shot + (y * CW + x) * 3;

	colour_of(k, gen, &r, &g, &b);
	check(abs(p[0] - r) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - b) <= 1, what, slot, k);
}

int main(void)
{
	int k, f, drawn;

	for (k = 0; k < NTEX; k++)
	{
		tex[k] = malloc(128 * 128 * 4);
		fill(k, 0);
	}
	check(gfx_init() == 0 && gfx_gpu, "gfx_init did not turn the RSX path on", gfx_gpu_why, 0);
	if (!gfx_gpu)
		return 1;

	/* 1. 40 new textures a frame for 30 frames, 400 in all against 160 slots: every square must show its
	 * own texture, none may be dropped (40 x 3 frames = 120 slots are held back at most) */
	for (f = 0; f < 30; f++)
	{
		gx_clear(0);
		for (k = 0; k < 40; k++)
			check(square(k, (f * 40 + k) % NTEX), "a square was dropped with slots to spare", f, k);
		read_frame();
		for (k = 0; k < 40; k++)
			expect(k, (f * 40 + k) % NTEX, 0, "a square shows another texture's colour");
		check(gx_stat_uploads == 40 || f >= 10, "textures sent in a frame of 40 new ones", gx_stat_uploads, f);
	}
	/* 2. the same 40 again and again: sent once, then kept */
	for (f = 0; f < 4; f++)
	{
		gx_clear(0);
		for (k = 0; k < 40; k++)
			square(k, k);
		read_frame();
		for (k = 0; k < 40; k++)
			expect(k, k, 0, "a kept texture shows the wrong colour");
		check(f == 0 || gx_stat_uploads == 0, "a kept texture was sent again", gx_stat_uploads, f);
	}
	/* 3. other pixels under the same address (the art file reloaded): the new ones must show */
	for (k = 0; k < 40; k++)
		fill(k, 1);
	gx_clear(0);
	for (k = 0; k < 40; k++)
		square(k, k);
	read_frame();
	for (k = 0; k < 40; k++)
		expect(k, k, 1, "changed pixels under a kept address did not show");
	check(gx_stat_uploads == 40, "changed textures sent again", gx_stat_uploads, 40);
	/* 4. the same after gx_tex_forget() */
	for (k = 0; k < 40; k++)
		fill(k, 2);
	gx_tex_forget();
	gx_clear(0);
	for (k = 0; k < 40; k++)
		square(k, k);
	read_frame();
	for (k = 0; k < 40; k++)
		expect(k, k, 2, "pixels did not show after gx_tex_forget");
	/* 5. more new textures in three frames than there are slots: the extra squares are dropped and counted,
	 * the ones drawn are still right, nothing queued is overwritten */
	for (f = 0; f < 3; f++)
	{
		gx_clear(0);
		drawn = 0;
		for (k = 0; k < 100; k++)
			drawn += square(k, 100 + f * 100 + k);
		read_frame();
		for (k = 0; k < drawn; k++)
			expect(k, 100 + f * 100 + k, 0, "a square drawn while the slots ran out shows the wrong colour");
		check(gx_stat_dropped == (unsigned)(100 - drawn) * 6, "dropped vertices are counted", gx_stat_dropped, drawn);
		printf("   frame %d of the overload: %d of 100 drawn\n", f, drawn);
	}
	/* after an overload every slot is held back for the frames still queued: three frames on they are free */
	for (f = 0; f < 3; f++)
	{
		gx_clear(0);
		read_frame();
	}
	/* 6. the vertex buffer's end: a request past it is refused, the frame before it stands */
	gx_clear(0);
	check(square(0, 5), "no slot three frames after the overload", 0, 0);
	drawn = 0;
	for (k = 0; k < 12000; k++)
	{
		gxv_t *p = gx_prims(NULL, 0, 0, 6);
		int i;

		if (!p)
			continue;
		for (i = 0; i < 6; i++)		/* six vertices at one point: nothing is covered */
			gx_put(p + i, 700.0f, 700.0f, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 1);
		drawn++;
	}
	check(drawn == (65536 - 6) / 6, "shapes that fitted the vertex buffer", drawn, (65536 - 6) / 6);
	read_frame();
	expect(0, 5, 2, "the square drawn before the buffer filled is gone");	/* textures 0..39 hold their third colours since step 4 */
	check(gx_stat_dropped == (unsigned)(12000 - drawn) * 6, "vertices past the buffer's end are counted", gx_stat_dropped, drawn);
	/* 8. the RSX runs a frame's draws LATE: the frame presented is only looked at after the next one has been
	 * built (its textures sent, its vertices written). Squares move a place each frame and take 40 new
	 * textures, so a slot or a vertex buffer reused a frame too early shows as a wrong square. */
	for (f = 0; f < 12; f++)
	{
		gx_clear(0);
		if (f >= 6)
			gx_tex_forget();	/* the kept textures are dropped: their slots must still wait for the queued frame */
		for (k = 0; k < 40; k++)
			check(square(k + f, 200 + (f * 40 + k) % 200), "a late-frame square was dropped", f, k);
		if (f > 0)
		{
			check(gx_dump("/tmp/gx_test.ppm") == 0, "the frame before could not be read back", 0, 0);
			{
				FILE *fp = fopen("/tmp/gx_test.ppm", "rb");
				int w, h, mx;

				if (fp && fscanf(fp, "P6 %d %d %d", &w, &h, &mx) == 3 && fgetc(fp) != EOF && fread(shot, 3, CW * CH, fp) == CW * CH)
					for (k = 0; k < 40; k++)
						expect(k + f - 1, 200 + ((f - 1) * 40 + k) % 200, 0, "a square of the frame before, drawn late, is wrong");
				else
					check(0, "the late frame did not read back", f, 0);
				if (fp)
					fclose(fp);
			}
		}
		gfx_present();
	}
	/* 9. a texture narrower than its slot (a PS1 save icon is 16 x 48): each row has its own colour */
	{
		static unsigned char small[16 * 48 * 4];
		gxv_t *p;
		int j, i;

		for (j = 0; j < 48; j++)
			for (i = 0; i < 16; i++)
			{
				unsigned char *t = small + (j * 16 + i) * 4;

				t[0] = (unsigned char)(j * 5);
				t[1] = (unsigned char)(i * 15);
				t[2] = 77;
				t[3] = 0x80;
			}
		gx_clear(0);
		p = gx_prims(small, 16, 48, 6);
		check(p != NULL, "the small texture was refused", 0, 0);
		if (p)
		{
			gx_put(p + 0, 0, 0, 0, 0, 1, 1, 1, 1); gx_put(p + 1, 160, 0, 16, 0, 1, 1, 1, 1); gx_put(p + 2, 0, 480, 0, 48, 1, 1, 1, 1);
			gx_put(p + 3, 160, 0, 16, 0, 1, 1, 1, 1); gx_put(p + 4, 160, 480, 16, 48, 1, 1, 1, 1); gx_put(p + 5, 0, 480, 0, 48, 1, 1, 1, 1);
		}
		read_frame();
		for (j = 2; j < 48; j += 9)
			for (i = 1; i < 16; i += 5)
			{
				const unsigned char *q = shot + ((j * 10 + 5) * CW + i * 10 + 5) * 3;	/* the middle of texel (i, j) */

				check(abs(q[0] - j * 5) <= 1 && abs(q[1] - i * 15) <= 1 && abs(q[2] - 77) <= 1, "a texel of the small texture", i, j);
			}
	}
	/* 7. an empty scissor draws nothing and is undone by the next one */
	gx_clear(0);
	gx_scissor(100, 100, 100, 200);
	check(gx_prims(NULL, 0, 0, 6) == NULL, "a shape was accepted inside an empty scissor", 0, 0);
	gx_scissor(0, 0, CW, CH);
	check(square(3, 7), "no shape after the scissor was opened again", 0, 0);
	gx_scissor(0, 0, CELL * 2, CH);		/* slot 3 lies outside this one */
	square(1, 8);
	square(4, 9);
	read_frame();
	expect(3, 7, 2, "the square after the empty scissor");
	expect(1, 8, 2, "the square inside the scissor");
	{
		const unsigned char *p = shot + ((CELL / 2) * CW + 4 * CELL + CELL / 2) * 3;

		check(p[0] == 0 && p[1] == 0 && p[2] == 0, "a square outside the scissor was drawn", p[0], p[1]);
	}
	/* 10. a half-clear white square over black is mid grey (the colour's fourth float is read), and a clear
	 * wipes what was drawn before it in the same frame */
	{
		gxv_t *p;
		const unsigned char *q;

		gx_clear(0);
		gx_scissor(0, 0, CW, CH);
		square(6, 3);
		gx_clear(0);
		p = gx_prims(NULL, 0, 0, 6);
		check(p != NULL, "no room for a plain square", 0, 0);
		if (p)
		{
			float x0 = 10 * CELL, x1 = 11 * CELL, y0 = 0, y1 = CELL;

			gx_put(p + 0, x0, y0, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f); gx_put(p + 1, x1, y0, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f);
			gx_put(p + 2, x0, y1, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f); gx_put(p + 3, x1, y0, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f);
			gx_put(p + 4, x1, y1, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f); gx_put(p + 5, x0, y1, GX_WHITE_U, GX_WHITE_V, 1, 1, 1, 0.5f);
		}
		read_frame();
		q = shot + ((CELL / 2) * CW + 10 * CELL + CELL / 2) * 3;
		check(abs(q[0] - 128) <= 1 && abs(q[1] - 128) <= 1 && abs(q[2] - 128) <= 1, "a half-clear white square over black", q[0], q[1]);
		q = shot + ((CELL / 2) * CW + 6 * CELL + CELL / 2) * 3;
		check(q[0] == 0 && q[1] == 0 && q[2] == 0, "a square drawn before a clear is still there", q[0], q[1]);
	}
	/* 11. v0.72, the depth buffer (the 3D save icons): between gx_depth(1) and gx_depth(0) the nearer of two
	 * overlapping squares wins whatever the order; each gx_depth(1) starts empty; after gx_depth(0) the
	 * last one drawn wins again */
	{
		gxv_t *p;
		const unsigned char *q;
		int pass, i;

		gx_clear(0);
		for (pass = 0; pass < 2; pass++)
		{
			/* slot 12: near (texture 3) then far (texture 4). slot 13: far (4) then near (3). */
			gx_depth(1);
			for (i = 0; i < 4; i++)
			{
				int slot = 12 + i / 2, near = (i % 2 == 0) == (slot == 12), k = near ? 3 : 4, v;
				float x0 = (float)(slot * CELL), x1 = x0 + CELL, d = near ? 0.2f : 0.8f;

				if (pass == 1 && near)
					continue;	/* second time round: only the far squares, into an emptied buffer */
				p = gx_prims(tex[k], 128, 128, 6);
				check(p != NULL, "no room for a depth square", slot, k);
				if (!p)
					continue;
				gx_put(p + 0, x0, 0, 0, 0, 1, 1, 1, 1); gx_put(p + 1, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 2, x0, CELL, 0, 128, 1, 1, 1, 1);
				gx_put(p + 3, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 4, x1, CELL, 128, 128, 1, 1, 1, 1); gx_put(p + 5, x0, CELL, 0, 128, 1, 1, 1, 1);
				for (v = 0; v < 6; v++)
					gx_z(p + v, d);
			}
			if (pass == 0)
			{
				/* slot 17: near then FAR as the very last thing before gx_depth(0): the far one is still waiting in
				 * its batch then, and must be drawn while the buffer is on (gx_depth has to send it off first) */
				int v, w;
				float x0 = (float)(17 * CELL), x1 = x0 + CELL;

				for (w = 0; w < 2; w++)
				{
					p = gx_prims(tex[w ? 4 : 3], 128, 128, 6);
					if (!p)
						continue;
					gx_put(p + 0, x0, 0, 0, 0, 1, 1, 1, 1); gx_put(p + 1, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 2, x0, CELL, 0, 128, 1, 1, 1, 1);
					gx_put(p + 3, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 4, x1, CELL, 128, 128, 1, 1, 1, 1); gx_put(p + 5, x0, CELL, 0, 128, 1, 1, 1, 1);
					for (v = 0; v < 6; v++)
						gx_z(p + v, w ? 0.8f : 0.2f);
				}
			}
			if (pass == 1)
			{
				/* slot 14: a NEAR square while the buffer is on; the square drawn there after gx_depth(0) is
				 * farther and must still cover it */
				int v;
				float x0 = (float)(14 * CELL), x1 = x0 + CELL;

				p = gx_prims(tex[3], 128, 128, 6);
				if (p)
				{
					gx_put(p + 0, x0, 0, 0, 0, 1, 1, 1, 1); gx_put(p + 1, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 2, x0, CELL, 0, 128, 1, 1, 1, 1);
					gx_put(p + 3, x1, 0, 128, 0, 1, 1, 1, 1); gx_put(p + 4, x1, CELL, 128, 128, 1, 1, 1, 1); gx_put(p + 5, x0, CELL, 0, 128, 1, 1, 1, 1);
					for (v = 0; v < 6; v++)
						gx_z(p + v, 0.1f);
				}
			}
			gx_depth(0);
			if (pass == 0)
			{
				read_frame();
				expect(12, 3, 2, "depth: near drawn first, far second: the near one must show");
				expect(13, 3, 2, "depth: far drawn first, near second: the near one must show");
				expect(17, 3, 2, "depth: the last square before gx_depth(0) was drawn after the buffer went off");
				gx_clear(0);
			}
		}
		square(14, 4);		/* no depth test now: it covers the near square left there */
		{
			/* slot 16: a plain red square, then the depth buffer on and a plain green one FARTHER than red's
			 * resting depth. Same texture, so nothing but gx_depth itself ends the red square's batch: red must
			 * be drawn before the buffer is on, and green then covers it. */
			int pass2, v;
			float x0 = (float)(16 * CELL), x1 = x0 + CELL;

			for (pass2 = 0; pass2 < 2; pass2++)
			{
				if (pass2 == 1)
					gx_depth(1);
				p = gx_prims(NULL, 0, 0, 6);
				if (!p)
					continue;
				gx_put(p + 0, x0, 0, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1); gx_put(p + 1, x1, 0, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1);
				gx_put(p + 2, x0, CELL, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1); gx_put(p + 3, x1, 0, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1);
				gx_put(p + 4, x1, CELL, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1); gx_put(p + 5, x0, CELL, GX_WHITE_U, GX_WHITE_V, !pass2, pass2, 0, 1);
				if (pass2 == 1)
					for (v = 0; v < 6; v++)
						gx_z(p + v, 0.9f);
			}
			gx_depth(0);
		}
		read_frame();
		q = shot + ((CELL / 2) * CW + 16 * CELL + CELL / 2) * 3;
		check(q[0] == 0 && q[1] == 255 && q[2] == 0, "a square drawn before gx_depth(1) was still drawn under the depth buffer", q[0], q[1]);
		expect(12, 4, 2, "depth: a far square after gx_depth(1) again must show (the buffer starts empty)");
		expect(14, 4, 2, "after gx_depth(0) the square drawn last must show");
		q = shot + ((CELL / 2) * CW + 15 * CELL + CELL / 2) * 3;
		check(q[0] == 0 && q[1] == 0 && q[2] == 0, "the depth squares spilled into the next place", q[0], q[1]);
	}
	printf("gx_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
