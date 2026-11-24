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

/* the boot file named in a SYSTEM.CNF ("BOOT2 = cdrom0:\SLUS_209.46;1", PS1 "cdrom:\...") */
static void cnf_id(char *out, char *b, int n)
{
	char *p;
	int i;

	b[n - 1] = 0;
	for (p = b; *p; p++)
	{
		if (!strncmp(p, "cdrom0:", 7) || !strncmp(p, "cdrom:", 6))
		{
			p += (p[5] == '0') ? 7 : 6;
			while (*p == '\\' || *p == '/')
				p++;
			for (i = 0; i < 11 && p[i] > ' ' && p[i] != ';'; i++)
				out[i] = p[i];
			out[i] = 0;
			return;
		}
	}
}

/* ISO9660 walk shared by both: rd(lba, buf) reads one 2048-byte user-data sector */
typedef int (*rd_fn)(void *ctx, u32 lba, u8 *buf);

static int iso_id(void *ctx, rd_fn rd, char *out, int *is_cd)
{
	u8 b[2048];
	u32 i;

	out[0] = 0;
	if (rd(ctx, 16, b) || b[0] != 1 || memcmp(b + 1, "CD001", 5))
		return -1;
	if (is_cd)
		*is_cd = (b[0xAA] == 0 && b[0xAB] == 0 && b[0xAC] == 8 && b[0xAD] == 0);	/* Cobra's rule, as the stage2 */
	if (rd(ctx, le32(b + 158), b))
		return -2;
	for (i = 0; i + 44 < 2048 && b[i]; i += b[i])
	{
		if (b[i + 32] >= 10 && !memcmp(b + i + 33, "SYSTEM.CNF", 10))
			break;
	}
	if (i + 44 >= 2048 || !b[i])
		return -3;
	if (rd(ctx, le32(b + i + 2), b))
		return -4;
	cnf_id(out, (char *)b, 512);
	return 0;
}

static int rd_card(void *ctx, u32 lba, u8 *buf)
{
	(void)ctx;
	return lr_file_read((u64)lba * 2048, buf, 2048);
}

typedef struct { FILE *f; int sec, data; } img_t;

static int rd_img(void *ctx, u32 lba, u8 *buf)
{
	img_t *m = (img_t *)ctx;

	if (fseek(m->f, (long)lba * m->sec + m->data, SEEK_SET) || fread(buf, 1, 2048, m->f) != 2048)
		return -1;
	return 0;
}

/* a PS1 image: 2048-byte ISO, or raw 2352-byte sectors (Mode 2 user data at +24, Mode 1 at +16) */
static void ps1_id(const char *path, char *out)
{
	static const int fmt[3][2] = { { 2048, 0 }, { 2352, 24 }, { 2352, 16 } };
	img_t m;
	int k;

	out[0] = 0;
	m.f = fopen(path, "rb");
	if (!m.f)
		return;
	for (k = 0; k < 3 && !out[0]; k++)
	{
		m.sec = fmt[k][0];
		m.data = fmt[k][1];
		iso_id(&m, rd_img, out, NULL);
	}
	fclose(m.f);
}

static void scan_ps2(void)
{
	int n, k;

	n_games[P_PS2] = 0;
	card_state = fsport_attach(ST_DEV_SD);
	if (card_state)
		return;
	card_state = lr_fs_mount();
	if (card_state == 0)
	{
		n = lr_fs_list();
		if (n < 0)
			card_state = n;
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
			if (lr_fs_cat_open((u32)k, &size, 1) == 0)
			{
				if (iso_id(NULL, rd_card, g->id, &cd) == -1)
					g->kind = 2;		/* no ISO9660 volume: a raw 2352-byte CD image */
				else
					g->kind = (u8)cd;
			}
			g->size = size;
			n_games[P_PS2]++;
		}
	}
	fsport_detach();
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

static void scan_hdd_ps2(void)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;

	if (sysFsOpendir("/dev_hdd0/PS2ISO", &fd) != 0)
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
		snprintf(g->path, sizeof(g->path), "/dev_hdd0/PS2ISO/%s", e.d_name);
		if (sysFsStat(g->path, &st) != 0 || (st.st_mode & 0170000) == 0040000 || has_marker(g->path))
			continue;
		snprintf(g->name, sizeof(g->name), "%s", e.d_name);
		g->size = st.st_size;
		g->where = 2;
		m.f = fopen(g->path, "rb");
		m.sec = 2048;
		m.data = 0;
		if (m.f)
		{
			if (iso_id(&m, rd_img, g->id, &cd) == -1)
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
	scan_hdd_ps2();			/* v0.16 */
	scan_disc_and_shortcuts();	/* v0.15 */
}

const char *where_name(int where)
{
	static const char *const n[] = { "SD", "SD", "HDD", "USB 0", "USB 1", "USB 2", "USB 3", "USB 4", "USB 5", "USB 6", "USB 7", "DISC", "MS", "CF" };

	return where >= 0 && where < (int)(sizeof n / sizeof n[0]) ? n[where] : "?";
}
