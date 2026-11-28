/* Find the games.
 *   PS2: the stage2's reader over the raw card: /PS2ISO and /PS2PRISM of every FAT32 / exFAT
 *        partition, split games joined (lr_fs.c). Title ID and CD/DVD by the stage2's rules.
 *   PS1: /dev_sd/PSXISO (the card's FAT32 partition, the one GameOS mounts), .bin / .iso / .img.
 *   PS3: /dev_sd/PS3ISO, .iso and split .iso.0 (webMAN joins the parts itself).
 * Read-only. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lv2/sysfs.h>
#include "fsport.h"
#include "storage.h"
#include "games.h"
#include "manage.h"
#include "launch.h"	/* v0.72: pps_is_link */
#include "discid.h"	/* v0.72 */

int lr_fs_cat_where(u32 i, int *exfat, u32 *part);	/* lr_fs.c, LR_APP (v0.73) */
#include "cardcheck.h"	/* v0.65 */
#include "exfat_card.h"	/* v0.82: the card's exFAT PS3ISO folder */

game_t games[N_PLAT][MAX_GAMES];
int n_games[N_PLAT];
int card_state = LR_E_NOMEDIUM;

static int ends_ci(const char *s, const char *e)
{
	size_t n = strlen(s), m = strlen(e), i;

	if (m > n)
		return 0;
	for (i = 0; i < m; i++)
	{
		char a = s[n - m + i], b = e[i];

		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* v0.72: the serial reader is discid.c (tested on the PC); these read an image's bytes for it */
static int rd_card(void *ctx, unsigned long long off, unsigned char *buf, unsigned len)
{
	(void)ctx;
	return lr_file_read(off, buf, len);
}

static int rd_stdio(void *ctx, unsigned long long off, unsigned char *buf, unsigned len)
{
	FILE *f = (FILE *)ctx;

	return fseeko(f, (off_t)off, SEEK_SET) || fread(buf, 1, len, f) != len ? -1 : 0;
}

typedef struct { FILE *f; int sec, data; } img_t;

static int rd_img(void *ctx, u32 lba, u8 *buf)
{
	img_t *m = (img_t *)ctx;

	if (fseek(m->f, (long)lba * m->sec + m->data, SEEK_SET) || fread(buf, 1, 2048, m->f) != 2048)
		return -1;
	return 0;
}

/* a PS1 image: 2048-byte ISO, or raw 2352-byte sectors (discid.c tries each) */
static void ps1_id(const char *path, char *out)
{
	FILE *f = fopen(path, "rb");

	out[0] = 0;
	if (!f)
		return;
	disc_id(f, rd_stdio, out, NULL, NULL);
	fclose(f);
}

/* v0.63: the card can sit in any of the three slots (Bananafuzzbuddy 11-24: a microSD in a Memory Stick adaptor showed no PS2
 * games: this read only the SD slot). Tried in the card reader's own LUN order (GL819: 0 Memory Stick, 1 SD, 2 CF;
 * lr/README.md) because the PS2 helper takes the FIRST ready LUN: with cards in two slots, both pick the same one. */
u64 card_dev = ST_DEV_SD;

static void scan_ps2(void)
{
	static const u64 devs[3] = { ST_DEV_MS, ST_DEV_SD, ST_DEV_CF };
	FILE *lg = fopen("/dev_hdd0/tmp/prism_card.log", "w");	/* v0.64: how each slot answered, with the reader's own lines */
	int n, k, d, chosen = -1, first_err = 0;

	n_games[P_PS2] = 0;
	card_state = LR_E_NOMEDIUM;
	card_bad_slot = -1;
	fsport_log = lg;
	/* v0.65: first every slot with a card in it: does it give back the card's real bytes (cardcheck.h) */
	for (d = 0; d < 3; d++)
	{
		int r = fsport_attach(devs[d]);

		slot_cc[d] = -1;
		slot_cc_why[d][0] = 0;
		if (lg)
			fprintf(lg, "slot %s: open %d\n", slot_name(d), r);
		if (r)
		{
			if (card_state == LR_E_NOMEDIUM)
				card_state = r;
			continue;
		}
		cardcheck_attached(d, lg);
		if (slot_bad(d) && card_bad_slot < 0)
			card_bad_slot = d;
		fsport_detach();
	}
	/* v0.66: then EVERY sound slot whose card holds a FAT32 / exFAT volume Prism reads, all their PS2 games (Bananafuzzbuddy 11-24:
	 * "if I have all three cards inserted at once I need it to read all three"). Each game keeps its slot; at the start
	 * the helper is told it when another card would answer first (card_lun_for). The first card read stays card_dev:
	 * copies to "the card's exFAT part" go there. */
	card_read_mask = 0;
	for (d = 0; d < 3; d++)
	{
		int r, before = n_games[P_PS2];

		if (slot_cc[d] < 0 || slot_bad(d))
			continue;
		r = fsport_attach(devs[d]);
		if (r == 0)
			r = lr_fs_mount();
		if (lg)
			fprintf(lg, "slot %s: partitions %d\n", slot_name(d), r);
		if (r)
		{
			if (!first_err)
				first_err = r;	/* a card there, but no volume Prism reads */
			fsport_detach();
			continue;
		}
		if (chosen < 0)
		{
			chosen = d;
			card_dev = devs[d];
		}
		card_read_mask |= 1 << d;
		n = lr_fs_list();
		for (k = 0; k < n && n_games[P_PS2] < MAX_GAMES; k++)
		{
			game_t *g = &games[P_PS2][n_games[P_PS2]];
			const char *nm = lr_fs_cat_name((u32)k);
			u64 size = 0;
			int cd = 0;

			memset(g, 0, sizeof(*g));
			snprintf(g->name, sizeof(g->name), "%s", nm);
			snprintf(g->path, sizeof(g->path), "%s/%s", LR_GAME_DIR, nm);
			g->where = 1;
			g->slot = (u8)d;
			{
				int ex = 0;
				u32 part = 0;

				if (lr_fs_cat_where((u32)k, &ex, &part) == 0)
					g->exfat = (u8)ex;
			}
			if (lr_fs_cat_open((u32)k, &size, 1) == 0)
			{
				int raw = 0;

				/* kind 2 = a raw 2352-byte CD image. v0.72: its serial is read too (it was taken for raw
				 * BECAUSE no volume was found at 2048 bytes a sector, and so had no serial: Crazy Taxi) */
				if (disc_id(NULL, rd_card, g->id, &cd, &raw) == -1 || raw)
					g->kind = 2;
				else
					g->kind = (u8)cd;
			}
			g->size = size;
			n_games[P_PS2]++;
		}
		if (lg)
			fprintf(lg, "slot %s: %d PS2 game(s)%s%s\n", slot_name(d), n_games[P_PS2] - before, n < 0 ? " (list failed)" : "",
				card_lun_for(d) >= 0 ? ", the PS2 helper is told this slot's LUN" : "");
		fsport_detach();
	}
	if (chosen >= 0)
		card_state = 0;
	else if (card_bad_slot >= 0)
		card_state = LR_E_BADSLOT;
	else if (first_err)
		card_state = first_err;
	fsport_log = NULL;
	if (lg)
	{
		fprintf(lg, "cards read:%s%s%s%s\n", card_read_mask & 1 ? " Memory Stick" : "", card_read_mask & 2 ? " SD" : "",
			card_read_mask & 4 ? " CompactFlash" : "", card_read_mask ? "" : " none");
		fclose(lg);
	}
}

/* v0.82: the storage device of USB port n, as rawseciso numbers them (webMAN-MOD _Projects_/rawseciso/storage.h) */
static u64 usb_dev(int n)
{
	return n < 6 ? 0x10300000000000AULL + (u64)n : 0x10300000000001FULL + (u64)(n - 6);
}

/* v0.82: the raw storage device a game on an exFAT volume is read from: a card slot's (where 0 / 12 / 13), or a USB
 * port's (where 3 .. 10). 0 = none. ps3sd.c and manage.c's exFAT delete use it, so both always mean the same drive. */
u64 game_raw_dev(const void *gv)
{
	const game_t *g = (const game_t *)gv;

	if (g->where >= 3 && g->where <= 10)
		return usb_dev(g->where - 3);
	if (g->where == 0)
		return ST_DEV_SD;
	if (g->where == WHERE_MS)
		return ST_DEV_MS;
	if (g->where == WHERE_CF)
		return ST_DEV_CF;
	return 0;
}

/* v0.82: PS3 ISOs on the exFAT volume of every card Prism read and of every USB drive with one (GameOS mounts neither:
 * a card's FAT32 part and a FAT32 USB drive are listed by scan_dir). Played from there by ps3sd.c: g->exfat = 1,
 * g->path = "<the device's GameOS name>/PS3ISO/<name>.iso", a split set by the name of its whole (its ".iso.0" part
 * names it; the other parts are not games). Bananafuzzbuddy 11-27: "just do Ps1 PSP and PS3 exfat usb drives". */
typedef struct
{
	const char *dir;
	u8 where, slot;
} rawx_t;

static int ps3x_cb(void *arg, const char *name, uint64_t size, int is_dir)
{
	const rawx_t *x = (const rawx_t *)arg;
	char nm[104];
	size_t n;
	game_t *g;

	if (is_dir || !size || name[0] == '.' || n_games[P_PS3] >= MAX_GAMES)
		return 0;
	snprintf(nm, sizeof nm, "%s", name);
	n = strlen(nm);
	if (ends_ci(nm, ".iso.0"))
		nm[n - 2] = 0;
	else if (!ends_ci(nm, ".iso"))
		return 0;
	g = &games[P_PS3][n_games[P_PS3]++];
	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof(g->name), "%s", nm);
	snprintf(g->path, sizeof(g->path), "%s/PS3ISO/%s", x->dir, nm);
	g->size = size;
	g->where = x->where;
	g->slot = x->slot;
	g->exfat = 1;
	g->kind = 0;
	return 0;
}

/* v0.82: PSP ISOs on the same exFAT volumes (PSPISO), played from there by pspsd.c (a plain .iso only: the fake disc
 * webMAN serves holds one image file) */
static int pspx_cb(void *arg, const char *name, uint64_t size, int is_dir)
{
	const rawx_t *x = (const rawx_t *)arg;
	game_t *g;

	if (is_dir || !size || name[0] == '.' || !ends_ci(name, ".iso") || n_games[P_PSP] >= MAX_GAMES)
		return 0;
	g = &games[P_PSP][n_games[P_PSP]++];
	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof(g->name), "%s", name);
	snprintf(g->path, sizeof(g->path), "%s/PSPISO/%s", x->dir, name);
	g->size = size;
	g->where = x->where;
	g->slot = x->slot;
	g->exfat = 1;
	g->kind = 0;
	return 0;
}

/* v0.82 / 1.00: PS1 images on the same exFAT volumes (PSXISO: .bin, .iso, .img; a .cue beside one gives its tracks),
 * played from there by ps3sd.c ps1sd_prepare */
static int psxx_cb(void *arg, const char *name, uint64_t size, int is_dir)
{
	const rawx_t *x = (const rawx_t *)arg;
	game_t *g;

	if (is_dir || !size || name[0] == '.' || n_games[P_PS1] >= MAX_GAMES
	    || !(ends_ci(name, ".bin") || ends_ci(name, ".iso") || ends_ci(name, ".img")))
		return 0;
	g = &games[P_PS1][n_games[P_PS1]++];
	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof(g->name), "%s", name);
	snprintf(g->path, sizeof(g->path), "%s/PSXISO/%s", x->dir, name);
	g->size = size;
	g->where = x->where;
	g->slot = x->slot;
	g->exfat = 1;
	g->kind = 1;		/* a CD, as scan_dir marks PS1 images */
	return 0;
}

static void scan_ps3_cards(void)
{
	static const u64 devs[3] = { ST_DEV_MS, ST_DEV_SD, ST_DEV_CF };
	static const char *const sdir[3] = { "/dev_ms", "/dev_sd", "/dev_cf" };
	static const u8 swhere[3] = { WHERE_MS, 0, WHERE_CF };
	static const char *const udir[8] = { "/dev_usb000", "/dev_usb001", "/dev_usb002", "/dev_usb003",
	                                     "/dev_usb004", "/dev_usb005", "/dev_usb006", "/dev_usb007" };
	u64 keep = card_dev;
	exw_dev dv;
	char why[160];
	rawx_t x;
	int d;

	for (d = 0; d < 3 + 8; d++)
	{
		if (d < 3)
		{
			if (!(card_read_mask & (1 << d)))
				continue;
			card_dev = devs[d];	/* exc_open opens card_dev */
			x.dir = sdir[d];
			x.where = swhere[d];
			x.slot = (u8)d;
		}
		else
		{
			card_dev = usb_dev(d - 3);	/* a drive with no exFAT volume (or none at all) stops at exc_open */
			x.dir = udir[d - 3];
			x.where = (u8)(3 + d - 3);
			x.slot = 0;
		}
		if (exc_open(&dv, why, sizeof why) == 0)
		{
			exw_list(&dv, "PS3ISO", ps3x_cb, &x, why, sizeof why);
			exw_list(&dv, "PSPISO", pspx_cb, &x, why, sizeof why);	/* v0.82 */
			exw_list(&dv, "PSXISO", psxx_cb, &x, why, sizeof why);	/* v0.82 / 1.00 */
			exc_close();
		}
	}
	card_dev = keep;
}

const char *game_where_name(const game_t *g)
{
	static const char *const s[3] = { "MS", "SD", "CF" };

	if (g->where == 1)
		return g->slot < 3 ? s[g->slot] : "card";
	return where_name(g->where);
}

static void psp_info(game_t *g);	/* below */

static void scan_dir(int plat, const char *dir, int where)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;

	if (sysFsOpendir(dir, &fd) != 0)
		return;
	while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0 && n_games[plat] < MAX_GAMES)
	{
		game_t *g = &games[plat][n_games[plat]];
		sysFSStat st;
		char name[256];
		int ok;

		if (e.d_name[0] == '.')
			continue;
		snprintf(name, sizeof(name), "%s", e.d_name);
		if (plat == P_PS1)
			ok = ends_ci(name, ".bin") || ends_ci(name, ".iso") || ends_ci(name, ".img");
		else if (plat == P_PSP)
			ok = ends_ci(name, ".iso") || ends_ci(name, ".cso");
		else
			ok = ends_ci(name, ".iso") || ends_ci(name, ".iso.0");
		if (!ok)
			continue;
		if (plat == P_PSP)
		{
			/* v0.72: an empty file that only stands for a card game in PPSSPP's folder is not a game (launch.h) */
			char full[400];

			snprintf(full, sizeof full, "%s/%s", dir, name);
			if (pps_is_link(full))
				continue;
		}
		memset(g, 0, sizeof(*g));
		snprintf(g->path, sizeof(g->path), "%s/%s", dir, name);
		if (ends_ci(name, ".0"))
			name[strlen(name) - 2] = 0;
		snprintf(g->name, sizeof(g->name), "%s", name);
		g->size = sysFsStat(g->path, &st) == 0 ? st.st_size : 0;
		g->kind = plat == P_PS1 ? 1 : 0;
		g->where = (u8)where;
		if (plat == P_PS1)
			ps1_id(g->path, g->id);
		if (plat == P_PSP)
		{
			g->kind = 0;
			psp_info(g);
		}
		n_games[plat]++;
	}
	sysFsClosedir(fd);
}

/* v0.16: a PSP image: /PSP_GAME/PARAM.SFO's TITLE and DISC_ID (plain .iso only; a .cso keeps its
 * file name) */
static int iso_find(img_t *m, u32 dir_lba, u32 dir_len, const char *name, u32 *lba, u32 *len)
{
	u8 b[2048];
	u32 s, i, n = (u32)strlen(name);

	for (s = 0; s < (dir_len + 2047) / 2048 && s < 16; s++)
	{
		if (rd_img(m, dir_lba + s, b))
			return -1;
		for (i = 0; i + 33 < 2048 && b[i]; i += b[i])
		{
			u32 nl = b[i + 32];

			if (nl >= n && !memcmp(b + i + 33, name, n) && (nl == n || b[i + 33 + n] == ';'))
			{
				*lba = le32(b + i + 2);
				*len = le32(b + i + 10);
				return 0;
			}
		}
	}
	return -2;
}

int sfo_get(const u8 *d, int n, const char *key, char *out, int out_n);

static void psp_info(game_t *g)
{
	static u8 d[8192];
	u8 b[2048];
	img_t m;
	u32 lba, len, k;

	if (!ends_ci(g->path, ".iso"))
		return;
	m.f = fopen(g->path, "rb");
	m.sec = 2048;
	m.data = 0;
	if (!m.f)
		return;
	if (rd_img(&m, 16, b) == 0 && b[0] == 1 && !memcmp(b + 1, "CD001", 5)
		&& iso_find(&m, le32(b + 158), le32(b + 166), "PSP_GAME", &lba, &len) == 0
		&& iso_find(&m, lba, len, "PARAM.SFO", &lba, &len) == 0 && len <= sizeof d)
	{
		for (k = 0; k * 2048 < len; k++)
			if (rd_img(&m, lba + k, d + k * 2048))
				break;
		if (k * 2048 >= len)
		{
			char t[100];

			sfo_get(d, (int)len, "DISC_ID", g->id, sizeof g->id);
			if (sfo_get(d, (int)len, "TITLE", t, sizeof t) == 0 && t[0])
			{
				snprintf(g->name, sizeof g->name, "%s", t);
				g->kind = 3;	/* a title, not a file name: describe() keeps it whole */
			}
		}
	}
	fclose(m.f);
}

/* v0.58: a PS3 or PSP game's own ICON0.PNG, for the Game covers page (covers.c): from a folder
 * (<path>/PS3_GAME/ICON0.PNG; a disc in the drive is one too) or from inside a plain .iso (PS3_GAME/ or
 * PSP_GAME/, the same look-up psp_info makes). buf NULL: only whether it is there (0). Else the bytes
 * read, or < 0: -1 not there, -2 bigger than cap, -3 not a folder or a plain .iso, -4 a read failed. */
int game_icon0(const char *path, int plat, u8 *buf, u32 cap)
{
	const char *top = plat == P_PSP ? "PSP_GAME" : "PS3_GAME";
	sysFSStat st;
	u8 b[2048];
	img_t m;
	u32 lba, len, k;
	int r = -1;

	if (sysFsStat(path, &st) == 0 && (st.st_mode & 0170000) == 0040000)
	{
		char p[320];
		FILE *f;
		size_t got;

		snprintf(p, sizeof p, "%s/%s/ICON0.PNG", path, top);
		f = fopen(p, "rb");
		if (!f)
			return -1;
		if (!buf)
		{
			fclose(f);
			return 0;
		}
		got = fread(buf, 1, cap, f);
		fclose(f);
		return got == 0 ? -4 : got >= cap ? -2 : (int)got;
	}
	if (!ends_ci(path, ".iso"))
		return -3;
	m.f = fopen(path, "rb");
	m.sec = 2048;
	m.data = 0;
	if (!m.f)
		return -1;
	if (rd_img(&m, 16, b) == 0 && b[0] == 1 && !memcmp(b + 1, "CD001", 5)
		&& iso_find(&m, le32(b + 158), le32(b + 166), top, &lba, &len) == 0
		&& iso_find(&m, lba, len, "ICON0.PNG", &lba, &len) == 0 && len > 0)
	{
		if (!buf)
			r = 0;
		else if (len >= cap)
			r = -2;
		else
		{
			for (k = 0; k * 2048 < len; k++)
			{
				u32 n = len - k * 2048 < 2048 ? len - k * 2048 : 2048;

				if (rd_img(&m, lba + k, b))
					break;
				memcpy(buf + k * 2048, b, n);
			}
			r = k * 2048 >= len ? (int)len : -4;
		}
	}
	fclose(m.f);
	return r;
}

/* one key of a PARAM.SFO (UTF-8 strings only) */
int sfo_get(const u8 *d, int n, const char *key, char *out, int out_n)
{
	u32 kt, dt, cnt, i;

	out[0] = 0;
	if (n < 20 || memcmp(d, "\0PSF", 4))
		return -1;
	kt = le32(d + 8);
	dt = le32(d + 12);
	cnt = le32(d + 16);
	for (i = 0; i < cnt && 20 + 16 * i + 16 <= (u32)n; i++)
	{
		const u8 *e = d + 20 + 16 * i;
		u32 ko = e[0] | (e[1] << 8), fmt = e[2] | (e[3] << 8), len = le32(e + 4), off = le32(e + 12);

		if (kt + ko >= (u32)n || strncmp((const char *)d + kt + ko, key, n - kt - ko))
			continue;
		if (fmt == 0x0404 || dt + off + len > (u32)n)
			return -2;
		if ((int)len >= out_n)
			len = out_n - 1;
		memcpy(out, d + dt + off, len);
		out[len] = 0;
		return 0;
	}
	return -3;
}

/* PS3 games as folders (the disc's files, PS3_GAME/... ; webMAN mounts these as it does ISOs),
 * named by the TITLE in their PARAM.SFO. Added to whatever scan_dir found. */
static void scan_folders(const char *dir, int where)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;

	if (sysFsOpendir(dir, &fd) != 0)
		return;
	while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0 && n_games[P_PS3] < MAX_GAMES)
	{
		game_t *g = &games[P_PS3][n_games[P_PS3]];
		char sfo[300], title[100];
		static u8 d[8192];
		FILE *f;
		int n;

		if (e.d_name[0] == '.')
			continue;
		snprintf(sfo, sizeof(sfo), "%s/%s/PS3_GAME/PARAM.SFO", dir, e.d_name);
		f = fopen(sfo, "rb");
		if (!f)
			continue;
		n = (int)fread(d, 1, sizeof(d), f);
		fclose(f);
		memset(g, 0, sizeof(*g));
		snprintf(g->path, sizeof(g->path), "%s/%s", dir, e.d_name);
		if (sfo_get(d, n, "TITLE", title, sizeof(title)) || !title[0])
			snprintf(title, sizeof(title), "%s", e.d_name);
		snprintf(g->name, sizeof(g->name), "%s", title);
		sfo_get(d, n, "TITLE_ID", g->id, sizeof(g->id));
		g->kind = 3;			/* a folder: no size worth walking the tree for */
		g->where = (u8)where;
		n_games[P_PS3]++;
	}
	sysFsClosedir(fd);
}

/* v0.16: PS2 images on the hard disk (/dev_hdd0/PS2ISO, where webMAN keeps them; it mounts PS2
 * images from the hard disk only). Started like any PS2 title (mount, then the PS2 hand-over),
 * no SD card and no setup involved. Left out: Prism's own launcher image and every shortcut title
 * (both carry the PRISMLR1 marker at 0x7800: they stand for games on the card, listed already). */
static int has_marker(const char *path)
{
	char b[8];
	FILE *f = fopen(path, "rb");
	int r = 0;

	if (!f)
		return 0;
	if (fseek(f, 0x7800, SEEK_SET) == 0 && fread(b, 1, 8, f) == 8)
		r = !memcmp(b, "PRISMLR1", 8);
	fclose(f);
	return r;
}

/* v0.78 (Bananafuzzbuddy, 11-25: "I can't transfer anything to the usb sticks"): the same for <usb drive>/PS2ISO, where a
 * copy made by rip.c lands. Kept there, not played there (main.c start() says so): listed so that it can be
 * copied back or deleted. where = games.h's device number (2 the hard disk, 3..10 the USB drives). */
static void scan_fs_ps2(const char *dir, int where)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;

	if (sysFsOpendir(dir, &fd) != 0)
		return;
	while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0 && n_games[P_PS2] < MAX_GAMES)
	{
		game_t *g = &games[P_PS2][n_games[P_PS2]];
		img_t m;
		sysFSStat st;
		int cd = 0;

		if (e.d_name[0] == '.' || !(ends_ci(e.d_name, ".iso") || ends_ci(e.d_name, ".bin")))
			continue;
		memset(g, 0, sizeof(*g));
		snprintf(g->path, sizeof(g->path), "%s/%s", dir, e.d_name);
		if (sysFsStat(g->path, &st) != 0 || (st.st_mode & 0170000) == 0040000 || has_marker(g->path))
			continue;
		snprintf(g->name, sizeof(g->name), "%s", e.d_name);
		g->size = st.st_size;
		g->where = (u8)where;
		m.f = fopen(g->path, "rb");
		if (m.f)
		{
			int raw = 0;

			if (disc_id(m.f, rd_stdio, g->id, &cd, &raw) == -1 || raw)
				g->kind = 2;
			else
				g->kind = (u8)cd;
			fclose(m.f);
		}
		n_games[P_PS2]++;
	}
	sysFsClosedir(fd);
}

/* v0.13: PS1 and PS3 games on every device GameOS mounts (webMAN mounts from any of them);
 * PS2 stays on the card, read raw by the stage2. g->where names the device (games.h WHERE_*). */
static const char *const dev_path[] = { "/dev_sd", NULL, "/dev_hdd0", "/dev_usb000", "/dev_usb001", "/dev_usb002",
	"/dev_usb003", "/dev_usb004", "/dev_usb005", "/dev_usb006", "/dev_usb007", NULL /* the drive */,
	"/dev_ms", "/dev_cf" /* v0.48: the copy screen offered these two, the lists never looked there */ };

void games_scan(void)
{
	unsigned i;

	n_games[P_PS1] = n_games[P_PS3] = n_games[P_PSP] = 0;
	for (i = 0; i < sizeof dev_path / sizeof dev_path[0]; i++)
	{
		char p[64];

		if (!dev_path[i])
			continue;
		snprintf(p, sizeof p, "%s/PSXISO", dev_path[i]);
		scan_dir(P_PS1, p, (int)i);
		snprintf(p, sizeof p, "%s/PS3ISO", dev_path[i]);
		scan_dir(P_PS3, p, (int)i);
		snprintf(p, sizeof p, "%s/GAMES", dev_path[i]);
		scan_folders(p, (int)i);
		snprintf(p, sizeof p, "%s/PSPISO", dev_path[i]);
		scan_dir(P_PSP, p, (int)i);
	}
	scan_ps2();
	scan_ps3_cards();			/* v0.82: after scan_ps2, which says which cards were read */
	scan_fs_ps2("/dev_hdd0/PS2ISO", 2);	/* v0.16 */
	for (i = 3; i <= 10; i++)
	{
		char p[64];

		snprintf(p, sizeof p, "%s/PS2ISO", dev_path[i]);
		scan_fs_ps2(p, (int)i);		/* v0.78 */
	}
	scan_disc_and_shortcuts();	/* v0.15 */
}

const char *where_name(int where)
{
	static const char *const n[] = { "SD", "SD", "HDD", "USB 0", "USB 1", "USB 2", "USB 3", "USB 4", "USB 5", "USB 6", "USB 7", "DISC", "MS", "CF" };

	if (where == 1)	/* v0.63: the card read raw: in whichever slot scan_ps2 found it */
		return card_dev == ST_DEV_MS ? "MS" : card_dev == ST_DEV_CF ? "CF" : "SD";
	return where >= 0 && where < (int)(sizeof n / sizeof n[0]) ? n[where] : "?";
}
