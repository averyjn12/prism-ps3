/* SETTINGS, "Game covers" (v0.57, Bananafuzzbuddy 2026-11-24: "have prism search the internet for it and give
 * them the option to download from somewhere").
 *
 * PS1 and PS2 discs carry no cover art; the key is the serial each game's SYSTEM.CNF names
 * (games.c). The places to look are lines in USRDIR/cover_sources.txt (covers_core.c reads them):
 * public cover sets on GitHub, and a cover pack installed on this PS3. "Look for covers" asks each
 * one which of the games without a cover it has (HEAD requests; only the serial is sent); then the
 * player picks one, or "every cover found", and Prism downloads, decodes (the PS3's own JPEG / PNG
 * decoders), scales to 128 x 128 and rebuilds /dev_hdd0/tmp/prism_art.dat with them, keeping every
 * cover the file already held. Prism itself ships no covers.
 * Each step goes to /dev_hdd0/tmp/prism_covers.log. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include <sysmodule/sysmodule.h>
#include <jpgdec/jpgdec.h>
#include <pngdec/pngdec.h>
#include "ui.h"
#include "look.h"
#include "games.h"
#include "screens.h"
#include "titles.h"
#include "covers.h"
#include "covers_core.h"
#include "net.h"
#include "version.h"

#define USR		"/dev_hdd0/game/PRSM00001/USRDIR"
#define SRC_FILE	USR "/cover_sources.txt"
#define ART_FILE	"/dev_hdd0/tmp/prism_art.dat"
#define ART_NEW		"/dev_hdd0/tmp/prism_art.dat.new"
#define LOG_FILE	"/dev_hdd0/tmp/prism_covers.log"
#define IMG_MAX		(6u << 20)
#define TB_HZ		79800000ULL

/* v0.58: a PS2 image smaller than this is not a retail disc: homebrew that borrowed a serial (Bananafuzzbuddy
 * 11-24: an OPL image says SLES_555.99 and came back as "Galileo Mystery"). Not looked up. */
#define PS2_RETAIL_MIN	(16ull << 20)

typedef struct
{
	char id[12];
	char title[100];
	u8 plat;		/* CV_PS1 .. CV_PSP */
	u8 have;		/* has a cover now, or the player removed it */
	u16 found;		/* bit s: source s has one */
	int gplat;		/* v0.58: P_PS1 .. P_PSP, for game_icon0 */
	char path[208];		/* v0.58: where the game is, for its own icon */
} cg_t;

static cg_t cg[4 * MAX_GAMES];
static int n_cg, n_small;
static cv_src src[CV_SRC_MAX];
static int n_src;
static u8 shown[CV_SRC_MAX];		/* a web source, or a local one whose folder is here */
static int found_n[CV_SRC_MAX], any_n, searched;
static int have_n[5], all_n[5];
static int sel, get_src;
static FILE *logf;
static unsigned (*pad_fn)(void);
static unsigned pad_last;

static u64 tb(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;

	if (!logf)
		return;
	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fputc('\n', logf);
	fflush(logf);
}

static unsigned pressed(void)
{
	unsigned b = pad_fn ? pad_fn() : 0, p = b & ~pad_last;

	pad_last = b;
	return p;
}

int app_events(void);	/* main.c */

static int stop_asked(void)
{
	return (pressed() & PAD_CIRCLE) || app_events();
}

/* a source is offered unless it needs a folder that is not here (a cover pack not installed).
 * v0.59: cv_src_folder decides which sources need one: 0.58 asked it of game: sources too */
static int offered(const cv_src *s)
{
	char d[208];
	sysFSStat st;

	return !cv_src_folder(s, d, sizeof d) || sysFsStat(d, &st) == 0;
}

/* v0.59: a game: source (local 2): the picture beside the game (game:BESIDE, webMAN's naming) or its own
 * ICON0.PNG. buf NULL: only whether it is there. 200 + *len, or 404 */
static int game_pic(const cv_src *s, const cg_t *c, u8 *buf, u32 cap, u32 *len)
{
	int k, r;

	*len = 0;
	if (!strcmp(s->path, "BESIDE"))
	{
		char p[260];

		for (k = 0; cv_beside(c->path, k, p, sizeof p) == 0; k++)
		{
			FILE *f = fopen(p, "rb");
			size_t got;

			if (!f)
				continue;
			if (!buf)
			{
				fclose(f);
				return 200;
			}
			got = fread(buf, 1, cap, f);
			fclose(f);
			if (got == 0 || got >= cap)
				return -5;
			*len = (u32)got;
			return 200;
		}
		return 404;
	}
	r = game_icon0(c->path, c->gplat, buf, cap);
	if (r < 0)
		return 404;
	*len = buf ? (u32)r : 0;
	return 200;
}

static void count(void)
{
	int i;

	memset(have_n, 0, sizeof have_n);
	memset(all_n, 0, sizeof all_n);
	for (i = 0; i < n_cg; i++)
	{
		all_n[cg[i].plat]++;
		have_n[cg[i].plat] += cg[i].have;
	}
}

void covers_open(void)
{
	static char text[4096];
	FILE *f;
	int p, i, k;

	n_src = 0;
	f = fopen(SRC_FILE, "rb");
	if (f)
	{
		size_t n = fread(text, 1, sizeof text - 1, f);

		text[n] = 0;
		fclose(f);
		n_src = cv_sources_parse(text, src, CV_SRC_MAX);
	}
	for (i = 0; i < n_src; i++)
		shown[i] = (u8)offered(&src[i]);
	n_cg = 0;
	n_small = 0;
	for (p = 0; p < 4; p++)
	{
		/* v0.58: PS3 and PSP games too */
		static const int gp[4] = { P_PS1, P_PS2, P_PS3, P_PSP }, cp[4] = { CV_PS1, CV_PS2, CV_PS3, CV_PSP };
		int plat = gp[p];

		for (i = 0; i < n_games[plat] && n_cg < (int)(sizeof cg / sizeof cg[0]); i++)
		{
			const game_t *g = &games[plat][i];

			if (!cv_is_id(g->id))
				continue;
			if (plat == P_PS2 && g->kind < 4 && g->size && g->size < PS2_RETAIL_MIN)
			{
				n_small++;
				continue;
			}
			for (k = 0; k < n_cg && strcmp(cg[k].id, g->id); k++)
				;
			if (k < n_cg)
			{
				cg[k].have |= g->cover != NULL;
				continue;
			}
			memset(&cg[n_cg], 0, sizeof cg[n_cg]);
			strcpy(cg[n_cg].id, g->id);
			snprintf(cg[n_cg].title, sizeof cg[n_cg].title, "%s", g->shown);
			snprintf(cg[n_cg].path, sizeof cg[n_cg].path, "%s", g->path);
			cg[n_cg].plat = (u8)cp[p];
			cg[n_cg].gplat = plat;
			cg[n_cg].have = g->cover != NULL || art_refused(g->id);
			n_cg++;
		}
	}
	count();
	searched = 0;
	any_n = 0;
	memset(found_n, 0, sizeof found_n);
	sel = 0;
}

/* ---- the page ---- */

#define ROW_FIND	0
static int rows(int *src_of)
{
	int n = 0, s;

	src_of[n++] = -2;		/* Look for covers */
	for (s = 0; s < n_src; s++)
		if (shown[s])
			src_of[n++] = s;
	src_of[n++] = -1;		/* every cover found */
	return n;
}

void covers_draw(void)
{
	int src_of[CV_SRC_MAX + 2], n = rows(src_of), i, y;
	char ln[220], r[40];
	const char *host = NULL;

	top_bar("SETTINGS");
	top_right("PRISM ", DIM, APP_VERSION, FAINT);
	text_ex(F_SMALL, LEFT, 82, 1.0f, 3, FAINT, "GAME COVERS");
	if (!n_cg)
		text(F_BODY, LEFT, 100, DIM, "No games with a serial were found.");
	else
	{
		/* v0.58: each platform that has games; "have" counts covers the player removed too */
		static const char *const pn[5] = { "", "PS1", "PS2", "PS3", "PSP" };
		static const int order[4] = { 2, 1, 3, 4 };
		int k, at = 0;
		char t[220];

		ln[0] = 0;
		for (k = 0; k < 4; k++)
			if (all_n[order[k]])
				at += snprintf(ln + at, sizeof ln - at, "%s%s %d of %d", at ? "  " DOT "  " : "", pn[order[k]], have_n[order[k]], all_n[order[k]]);
		snprintf(ln + at, sizeof ln - at, " have a cover");
		fit(t, sizeof t, F_BODY, ln, RIGHT - LEFT);
		text(F_BODY, LEFT, 100, INK, t);
	}
	if (sel >= n)
		sel = n - 1;
	for (i = 0, y = 132; i < n; i++, y += n > 7 ? 21 : 24)
	{
		int s = src_of[i], on = i == sel, usable = s == -2 ? n_cg > 0 : searched && (s >= 0 ? found_n[s] : any_n) > 0;
		const char *name = s == -2 ? "Look for covers" : s == -1 ? "Get every cover found" : src[s].name;

		if (on)
		{
			float hb = n > 7 ? 17.0f : 20.0f;

			rect(LEFT, (float)y - 3.0f, RIGHT, (float)y + hb, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
			frame_rect(LEFT, (float)y - 3.0f, RIGHT, (float)y + hb, 1.0f, GLOW);
		}
		if (s >= 0)
			snprintf(ln, sizeof ln, "Get covers from %s", name);
		else
			snprintf(ln, sizeof ln, "%s", name);
		{
			char t[200];

			fit(t, sizeof t, F_BODY, ln, 420);
			text(F_BODY, LEFT + 12, y, usable ? (on ? INK : DIM) : FAINT, t);
		}
		r[0] = 0;
		if (s == -2 && searched)
			snprintf(r, sizeof r, "%d without one", all_n[1] + all_n[2] - have_n[1] - have_n[2]);
		else if (s >= 0)
			snprintf(r, sizeof r, searched ? "%d found" : src[s].local == 2 ? "in the game" : src[s].local ? "on this PS3" : "online", found_n[s]);
		else if (s == -1 && searched)
			snprintf(r, sizeof r, "%d", any_n);
		if (r[0])
			text(F_SMALL, RIGHT - 10 - text_w(F_SMALL, r, 0), y + 3, s >= 0 && searched && found_n[s] ? GLOW : DIM, r);
		if (s >= 0 && !src[s].local && !host)
			host = src[s].host;
	}
	y += 6;
	if (host)
	{
		snprintf(ln, sizeof ln, "Looking sends only each game's serial (like SLUS-21243) to %s.", host);
		text(F_SMALL, LEFT, y, DIM, ln);
		y += 17;
	}
	text(F_SMALL, LEFT, y, DIM, "Prism ships no covers. A wrong one: Square on the game, \"Remove its cover\".");
	if (n_small)
	{
		snprintf(ln, sizeof ln, "%d PS2 image%s under 16 MB not looked up: homebrew can borrow a real game's serial.", n_small, n_small > 1 ? "s" : "");
		text(F_SMALL, LEFT, y + 17, FAINT, ln);
	}
	else
		text(F_SMALL, LEFT, y + 17, FAINT, "Covers are kept in /dev_hdd0/tmp/prism_art.dat. Log: prism_covers.log.");
	if (note_up())
		text_centre(F_SMALL, 320, 376, 0, AMBER, note_text());
	legend_line();
	i = legend_icon(LEFT, 0, "Choose");
	legend_icon(i, 3, "Back");
}

int covers_pad(unsigned p)
{
	int src_of[CV_SRC_MAX + 2], n = rows(src_of), s;

	if (p & PAD_CIRCLE)
		return -1;
	if (p & PAD_UP)
		sel = sel ? sel - 1 : n - 1;
	if (p & PAD_DOWN)
		sel = sel < n - 1 ? sel + 1 : 0;
	if (!(p & PAD_CROSS))
		return DO_NOTHING;
	s = src_of[sel < n ? sel : 0];
	if (s == -2)
	{
		if (!n_cg)
		{
			ui_say("No games with a serial were found.");
			return DO_NOTHING;
		}
		return DO_COV_FIND;
	}
	if (!searched)
	{
		ui_say("Choose \"Look for covers\" first.");
		return DO_NOTHING;
	}
	if ((s >= 0 ? found_n[s] : any_n) == 0)
	{
		ui_say(s >= 0 ? "That source has no covers for your games without one." : "No covers were found.");
		return DO_NOTHING;
	}
	get_src = s;
	return DO_COV_GET;
}

/* ---- looking ---- */

static int web_ready(char *msg, int n)
{
	char why[200];
	int s;
	u64 t = 0;

	if (!net_online(why, sizeof why))
	{
		snprintf(msg, n, "%s", why);
		say("offline: %s", why);
		return 0;
	}
	for (s = 0; s < n_src && !t; s++)
		if (shown[s] && !src[s].local)
			t = net_time(src[s].host);
	https_time(t);
	say("time from the web server's Date header: %llu%s", (unsigned long long)t, t ? "" : " (none: certificates judged without a date)");
	return 1;
}

static int game_wanted(int i, int s)
{
	return !cg[i].have && (src[s].plats & (1 << cg[i].plat));
}

void covers_find(unsigned (*pad)(void), char *msg, int n)
{
	int s, i, missing = 0, web = 0, done = 0, todo = 0, stopped = 0;
	char path[300], what[160];
	u64 t0 = tb();

	pad_fn = pad;
	pad_last = pad ? pad() : 0;
	msg[0] = 0;
	logf = fopen(LOG_FILE, "a");
	say("=== look for covers, app v%s: %d games with a serial; %d PS2 image(s) under 16 MB left out", APP_VERSION, n_cg, n_small);
	for (i = 0; i < n_cg; i++)
	{
		missing += !cg[i].have;
		cg[i].found = 0;
	}
	memset(found_n, 0, sizeof found_n);
	any_n = 0;
	searched = 0;
	if (!missing)
	{
		snprintf(msg, n, "Every game already has a cover.");
		goto out;
	}
	for (s = 0; s < n_src; s++)
		if (shown[s] && !src[s].local)
			web = 1;
	if (web)
	{
		progress(NULL, "LOOKING FOR COVERS", "Connecting", 0.02f, "The internet, and the time from a web server");
		if (!web_ready(msg, n))
		{
			web = 0;
			for (s = 0; s < n_src && !(shown[s] && src[s].local); s++)
				;
			if (s == n_src)
				goto out;	/* nothing on this PS3 to look in either */
		}
	}
	for (s = 0; s < n_src; s++)
		if (shown[s] && (src[s].local || web))
			for (i = 0; i < n_cg; i++)
				todo += game_wanted(i, s);
	for (s = 0; s < n_src && !stopped; s++)
	{
		int ok = 0, fails = 0;

		if (!shown[s] || (!src[s].local && !web))
			continue;
		for (i = 0; i < n_cg && !stopped; i++)
		{
			char dash[12];
			int r;

			if (!game_wanted(i, s))
				continue;
			done++;
			cv_dash(cg[i].id, dash);
			snprintf(what, sizeof what, "%s  " DOT "  %d of %d  " DOT "  Circle stops", dash, done, todo);
			progress(NULL, "LOOKING FOR COVERS", src[s].name, (float)done / (float)todo, what);
			if (stop_asked())
			{
				stopped = 1;
				break;
			}
			if (src[s].local == 2)
			{
				u32 len;

				r = game_pic(&src[s], &cg[i], NULL, 0, &len);	/* v0.58/0.59: its own picture */
			}
			else if (cv_expand(src[s].path, cg[i].id, path, sizeof path))
				continue;
			else if (src[s].local)
			{
				sysFSStat st;

				r = sysFsStat(path, &st) == 0 ? 200 : 404;
			}
			else
			{
				u32 len;

				r = https_req(src[s].host, path, 1, NULL, 0, &len);
			}
			if (r == 200)
			{
				cg[i].found |= (u16)(1 << s);
				found_n[s]++;
				ok++;
			}
			else if (r != 404)
			{
				say("  %s %s: %d %s", src[s].name, cg[i].id, r, r < 0 ? https_err(r) : "");
				if (++fails >= 3 && !ok)
				{
					say("  %s: given up after 3 failures", src[s].name);
					break;
				}
			}
		}
		say("%s: %d found, %d failed", src[s].name, found_n[s], fails);
	}
	https_end();
	for (i = 0; i < n_cg; i++)
		any_n += cg[i].found != 0;
	searched = 1;
	if (stopped)
		snprintf(msg, n, "Stopped. So far: covers for %d of the %d games without one.", any_n, missing);
	else if (msg[0] && !web)
		;	/* offline: msg says so; the cover pack was still looked in */
	else
		snprintf(msg, n, "Covers found for %d of the %d games without one (%u s). Pick where to get them from.", any_n, missing,
			(unsigned)((tb() - t0) / TB_HZ));
out:
	say("result: %s", msg);
	if (logf)
		fclose(logf);
	logf = NULL;
}

/* ---- getting ---- */

static int read_local(const char *path, u8 *buf, u32 cap, u32 *len)
{
	FILE *f = fopen(path, "rb");
	size_t k;

	*len = 0;
	if (!f)
		return 404;
	k = fread(buf, 1, cap, f);
	fclose(f);
	*len = (u32)k;
	return k == cap ? -5 : 200;	/* a file as big as the buffer is taken for too big */
}

/* JPEG or PNG bytes to one cover; NULL if neither decoder takes it. v0.59: by its shape (cv_cover): a wide
 * picture is an icon and keeps its shape, a box fills the square */
static u8 *decode(const u8 *b, u32 len)
{
	u8 *px = NULL;
	void *bmp = NULL;
	u32 w = 0, h = 0, pitch = 0;
	s32 r = -1;

	if (len > 3 && b[0] == 0xFF && b[1] == 0xD8)
	{
		jpgData d;

		memset(&d, 0, sizeof d);
		r = jpgLoadFromBuffer(b, len, &d);
		bmp = d.bmp_out, w = d.width, h = d.height, pitch = d.pitch;
	}
	else if (len > 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8))
	{
		pngData d;

		memset(&d, 0, sizeof d);
		r = pngLoadFromBuffer(b, len, &d);
		bmp = d.bmp_out, w = d.width, h = d.height, pitch = d.pitch;
	}
	if (r == 0 && bmp && w && h && w <= 8192 && h <= 8192)
	{
		px = (u8 *)malloc(CV_PIX);
		if (px && cv_cover((const u8 *)bmp, (int)w, (int)h, (int)pitch, px))
		{
			free(px);
			px = NULL;
		}
	}
	else
		say("  decode: rc %x, %ux%u", (unsigned)r, w, h);
	free(bmp);
	return px;
}

/* ASCII only: the PS2 launcher's font has nothing else */
static void ascii_title(const char *id, char *out, int n)
{
	char t[200];
	int i;

	out[0] = 0;
	if (title_lookup(id, t, sizeof t))
		return;
	for (i = 0; t[i]; i++)
		if ((unsigned char)t[i] < 32 || (unsigned char)t[i] > 126)
			return;
	snprintf(out, n, "%s", t);
}

/* the art file again: its old records and covers, newpx[i] for cg[i] added (newpx may be NULL), and
 * v0.58: the record of mark_id given mark (1: its cover removed and never looked up again; 0: allowed
 * to be looked up again) */
static int rebuild(u8 **newpx, const char *mark_id, int mark_plat, int mark, const cv_entry *add, char *why, int wn)
{
	static cv_entry e[CV_REC_MAX + 4 * MAX_GAMES];
	long len;
	const u8 *old = art_bytes(&len);
	int ne = old ? cv_art_parse(old, len, e, CV_REC_MAX) : 0, i, k, r;
	FILE *f;
	char *vb;

	if (ne < 0)
		ne = 0;
	say("rebuild: %d records in the old art file", ne);
	for (i = 0; newpx && i < n_cg; i++)
	{
		for (k = 0; k < ne && strcmp(e[k].id, cg[i].id); k++)
			;
		if (k == ne)
		{
			if (!newpx[i] || ne >= (int)(sizeof e / sizeof e[0]))
				continue;
			memset(&e[ne], 0, sizeof e[ne]);
			strcpy(e[ne].id, cg[i].id);
			if (cg[i].plat <= CV_PS2)
				ascii_title(cg[i].id, e[ne].title, sizeof e[ne].title);	/* PS3 / PSP names come from PARAM.SFO */
			ne++;
		}
		e[k].plat = cg[i].plat;
		if (newpx[i])
		{
			e[k].px = newpx[i];
			e[k].refused = 0;
		}
	}
	if (mark_id)
	{
		for (k = 0; k < ne && strcmp(e[k].id, mark_id); k++)
			;
		if (k == ne && mark && ne < (int)(sizeof e / sizeof e[0]))
		{
			memset(&e[ne], 0, sizeof e[ne]);
			snprintf(e[ne].id, sizeof e[ne].id, "%s", mark_id);
			e[ne].plat = (u8)mark_plat;
			ne++;
		}
		if (k < ne)
		{
			e[k].refused = (u8)mark;
			if (mark)
			{
				e[k].px = NULL;
				e[k].title[0] = 0;	/* the borrowed serial's title goes too: the file name shows again */
			}
		}
		say("rebuild: %s %s", mark_id, mark ? "cover removed, never looked up again" : "may be looked up again");
	}
	if (add)
	{
		/* v0.59: one cover Prism brings itself (Tiny Tees'): added, or put on a record that has none */
		for (k = 0; k < ne && strcmp(e[k].id, add->id); k++)
			;
		if (k == ne && ne < (int)(sizeof e / sizeof e[0]))
		{
			e[ne] = *add;
			ne++;
		}
		else if (k < ne && !e[k].px && !e[k].refused)
		{
			e[k].px = add->px;
			e[k].plat = add->plat;
			if (!e[k].title[0])
				snprintf(e[k].title, sizeof e[k].title, "%s", add->title);
		}
		say("rebuild: %s, Prism's own cover", add->id);
	}
	f = fopen(ART_NEW, "wb");
	if (!f)
	{
		snprintf(why, wn, "could not write %s", ART_NEW);
		return -1;
	}
	vb = (char *)malloc(1 << 20);
	if (vb)
		setvbuf(f, vb, _IOFBF, 1 << 20);	/* newlib's 1 KB default would cut every cover into 64 writes */
	r = cv_art_write(e, ne, f);
	if (fclose(f) != 0 && r >= 0)
		r = -9;
	free(vb);
	say("rebuild: %d records, %d covers written (%d)", ne, r, r);
	if (r < 0)
	{
		sysFsUnlink(ART_NEW);
		snprintf(why, wn, "writing the art file failed (%d)", r);
		return -1;
	}
	sysFsUnlink(ART_FILE);
	if (sysLv2FsRename(ART_NEW, ART_FILE) != 0)
	{
		snprintf(why, wn, "could not rename %s", ART_NEW);
		return -1;
	}
	sysFsChmod(ART_FILE, 0666);
	return r;
}

int covers_get(unsigned (*pad)(void), char *msg, int n)
{
	static u8 *newpx[2 * MAX_GAMES];
	int i, s, todo = 0, done = 0, got = 0, bad = 0, changed = 0, stopped = 0, web = 0;
	u8 *img = NULL;
	char path[300], what[160], why[160];
	u64 t0 = tb();

	pad_fn = pad;
	pad_last = pad ? pad() : 0;
	logf = fopen(LOG_FILE, "a");
	say("=== get covers, app v%s, from %s", APP_VERSION, get_src >= 0 ? src[get_src].name : "every source, first found");
	memset(newpx, 0, sizeof newpx);
	for (i = 0; i < n_cg; i++)
		if (!cg[i].have && (get_src >= 0 ? cg[i].found & (1 << get_src) : cg[i].found))
		{
			todo++;
			for (s = 0; s < n_src; s++)
				if ((get_src < 0 || s == get_src) && (cg[i].found & (1 << s)) && !src[s].local)
					web = 1;
		}
	if (!todo)
	{
		snprintf(msg, n, "Nothing to get.");
		goto out;
	}
	if (web)
	{
		progress(NULL, "GETTING COVERS", "Connecting", 0.02f, "The internet, and the time from a web server");
		if (!web_ready(msg, n))
			goto out;
	}
	img = (u8 *)malloc(IMG_MAX);
	if (!img)
	{
		snprintf(msg, n, "Out of memory.");
		goto out;
	}
	sysModuleLoad(SYSMODULE_JPGDEC);
	sysModuleLoad(SYSMODULE_PNGDEC);
	for (i = 0; i < n_cg && !stopped; i++)
	{
		u32 len = 0;
		int r = 404;

		if (cg[i].have || !(get_src >= 0 ? cg[i].found & (1 << get_src) : cg[i].found))
			continue;
		done++;
		for (s = 0; s < n_src && r != 200; s++)
		{
			char dash[12];

			if ((get_src >= 0 && s != get_src) || !(cg[i].found & (1 << s)))
				continue;
			cv_dash(cg[i].id, dash);
			snprintf(what, sizeof what, "%s  " DOT "  %d of %d  " DOT "  Circle stops", dash, done, todo);
			progress(NULL, "GETTING COVERS", cg[i].title, (float)done / (float)todo, what);
			if (stop_asked())
			{
				stopped = 1;
				break;
			}
			if (src[s].local == 2)
				r = game_pic(&src[s], &cg[i], img, IMG_MAX, &len);	/* v0.58/0.59: its own picture */
			else if (cv_expand(src[s].path, cg[i].id, path, sizeof path))
				continue;
			else
				r = src[s].local ? read_local(path, img, IMG_MAX, &len) : https_req(src[s].host, path, 0, img, IMG_MAX, &len);
			if (r == 200 && !(newpx[i] = decode(img, len)))
				r = -100;	/* not a picture the decoders take: the next source, if "every" */
			if (r != 200)
				say("  %s from %s: %d %s", cg[i].id, src[s].name, r, r < 0 && r > -100 ? https_err(r) : r == -100 ? "could not be read" : "");
		}
		if (newpx[i])
			got++;
		else if (!stopped)
			bad++;
	}
	https_end();
	sysModuleUnload(SYSMODULE_PNGDEC);
	sysModuleUnload(SYSMODULE_JPGDEC);
	free(img);
	img = NULL;
	if (got)
	{
		progress(NULL, "GETTING COVERS", "Saving", 0.98f, ART_FILE);
		if (rebuild(newpx, NULL, 0, 0, NULL, why, sizeof why) < 0)
		{
			snprintf(msg, n, "%d covers downloaded, but %s: nothing changed.", got, why);
			goto out;
		}
		art_load();	/* main.c rescans before anything is drawn: the cover pointers move */
		changed = 1;
		for (i = 0; i < n_cg; i++)
			if (newpx[i])
			{
				cg[i].have = 1;
				cg[i].found = 0;
			}
		count();
		searched = 0;	/* the counts are stale now: look again for the rest */
	}
	snprintf(msg, n, "%s%d new covers (%u s).%s%s", stopped ? "Stopped. " : "", got, (unsigned)((tb() - t0) / TB_HZ),
		bad ? " Some could not be fetched or read: see prism_covers.log." : "", got ? " They show in Prism and in the PS2 launcher." : "");
out:
	for (i = 0; i < (int)(sizeof newpx / sizeof newpx[0]); i++)
		free(newpx[i]);
	memset(newpx, 0, sizeof newpx);
	say("result: %s", msg);
	if (logf)
		fclose(logf);
	logf = NULL;
	return changed;
}

/* v0.58: a game's Square menu, "Remove its cover" (refuse 1) / "Let Game covers find its cover"
 * (refuse 0). 1 = the art file changed: main.c rescans before anything is drawn. */
int covers_mark(const void *gv, int plat_index, int refuse, char *msg, int n)
{
	static const int cp[4] = { CV_PS1, CV_PS2, CV_PS3, CV_PSP };
	const game_t *g = (const game_t *)gv;
	char why[160];
	int r;

	if (!cv_is_id(g->id) || plat_index < 0 || plat_index > 3)
	{
		snprintf(msg, n, "This game has no serial: Prism has no cover for it.");
		return 0;
	}
	logf = fopen(LOG_FILE, "a");
	say("=== %s %s (%s), app v%s", refuse ? "remove the cover of" : "allow a cover again for", g->id, g->path, APP_VERSION);
	r = rebuild(NULL, g->id, cp[plat_index], refuse, NULL, why, sizeof why);
	if (r >= 0)
		art_load();
	if (r < 0)
		snprintf(msg, n, "Nothing changed: %s.", why);
	else if (refuse)
		snprintf(msg, n, "Cover removed. Game covers will not look this game up again.");
	else
		snprintf(msg, n, "Game covers will look this game up again (SETTINGS, Game covers).");
	say("result: %s", msg);
	if (logf)
		fclose(logf);
	logf = NULL;
	return r >= 0;
}

/* v0.59: a cover Prism brings itself (Tiny Tees', USRDIR/setup/tinytees_cover.rgba: 128 x 128 R G B A),
 * put in the art file for id unless that game already has a cover or the player removed it.
 * 1 = the art file changed (and is loaded again). */
int covers_builtin(const char *id, int plat_cv, const char *title, const char *rgba_path)
{
	static u8 px[CV_PIX];
	cv_entry add;
	char why[160];
	FILE *f;
	int r;

	if (!cv_is_id(id) || art_find(id, NULL) || art_refused(id))
		return 0;
	f = fopen(rgba_path, "rb");
	if (!f)
		return 0;
	r = fread(px, 1, CV_PIX, f) == CV_PIX;
	fclose(f);
	if (!r)
		return 0;
	memset(&add, 0, sizeof add);
	snprintf(add.id, sizeof add.id, "%s", id);
	snprintf(add.title, sizeof add.title, "%s", title);
	add.plat = (u8)plat_cv;
	add.px = px;
	logf = fopen(LOG_FILE, "a");
	say("=== Prism's own cover for %s (%s), app v%s", id, title, APP_VERSION);
	r = rebuild(NULL, NULL, 0, 0, &add, why, sizeof why);
	if (r >= 0)
		art_load();
	say("result: %s", r >= 0 ? "added" : why);
	if (logf)
		fclose(logf);
	logf = NULL;
	return r >= 0;
}
