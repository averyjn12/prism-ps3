/* See pspsd_core.h. A port of webMAN-MOD prepISO's fake_iso.h (create_fake_file_iso_mem, build_fake_iso),
 * GPL-3.0, written byte-order-explicit so the PC tests produce the console's exact bytes. */
#include <string.h>
#include "pspsd_core.h"
#include "fake_iso_data.h"

#define MAX_NAME_LEN	110	/* prepISO's: a longer name is cut, its extension kept */

static void set733(uint8_t *p, uint32_t n)
{
	p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
	p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
}

static void set723(uint8_t *p, uint32_t n)
{
	p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static uint32_t get731(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* UTF-8 to UTF-16 big-endian (Joliet's byte order; prepISO wrote native u16 on the big-endian PS3).
 * Returns the bytes written; a broken sequence is skipped, as prepISO's UTF8_to_UTF16 does */
static int utf16be(const uint8_t *s, uint8_t *out, int max)
{
	int k = 0;

	while (*s && k + 4 <= max)
	{
		uint32_t c;
		int m, i;

		if (*s < 0x80)
		{
			c = *s++;
			m = 0;
		}
		else if ((*s & 0xF8) == 0xF0)
		{
			c = *s++ & 7;
			m = 3;
		}
		else if ((*s & 0xF0) == 0xE0)
		{
			c = *s++ & 0xF;
			m = 2;
		}
		else if ((*s & 0xE0) == 0xC0)
		{
			c = *s++ & 0x1F;
			m = 1;
		}
		else
		{
			s++;
			continue;
		}
		for (i = 0; i < m && (*s & 0xC0) == 0x80; i++)
			c = (c << 6) | (*s++ & 0x3F);
		if (i < m)
			continue;
		if (c < 0x10000)
		{
			out[k++] = (uint8_t)(c >> 8);
			out[k++] = (uint8_t)c;
		}
		else
		{
			uint32_t hi = 0xD800 | (((c - 0x10000) >> 10) & 0x3FF), lo = 0xDC00 | ((c - 0x10000) & 0x3FF);

			out[k++] = (uint8_t)(hi >> 8);
			out[k++] = (uint8_t)hi;
			out[k++] = (uint8_t)(lo >> 8);
			out[k++] = (uint8_t)lo;
		}
	}
	return k;
}

/* one directory record at p (prepISO's create_directory_record: struct iso_directory_record is 34 bytes
 * with its name[1], so the length is (name_len + 35) & ~1); returns the next record's place */
static uint8_t *dir_record(uint8_t *p, const uint8_t *name, int name_len, uint32_t size, uint8_t flags, uint32_t lba)
{
	int len = (name_len + 35) & ~1;

	memcpy(p + 33, name, name_len);
	p[32] = (uint8_t)name_len;
	p[0] = (uint8_t)len;
	p[1] = 0;
	set733(p + 2, lba);
	set733(p + 10, size);
	p[18] = 0x71; p[19] = 0x0B; p[20] = 0x0A; p[21] = 0x0D; p[22] = 0x38; p[23] = 0x00; p[24] = 0x04;
	p[25] = flags;
	p[26] = 0;
	p[27] = 0;
	set723(p + 28, 1);
	return p + len;
}

static const char *extension(const char *path)
{
	const char *d = strrchr(path, '.'), *s = strrchr(path, '/');

	return d && (!s || d > s) ? d : path + strlen(path);
}

int fi_header(const char *name_in, uint64_t size, uint8_t *out)
{
	char name[MAX_NAME_LEN + 2];
	uint8_t u16[0x400];
	int len_name, len16;
	/* the file's record goes after "." and ".." (34 bytes each) of each root directory: 0xB800 + 0x44 and
	 * 0xC000 + 0x44. prepISO writes the primary one at 0xB840, over the last 4 bytes of "..": Linux then
	 * fails the primary tree (EIO, host/pspsd_test.py), and only Joliet readers ever worked */
	uint8_t *idr = out + 0xB844, *idr2 = out + 0xC044;
	uint32_t last_lba;

	if (!name_in || !name_in[0] || !size || size > 0xFFFFF800ull || strchr(name_in, '/'))
		return -1;	/* a PSP image is one record: under 4 GiB */
	len_name = (int)strlen(name_in);
	memcpy(name, name_in, len_name > MAX_NAME_LEN ? MAX_NAME_LEN : len_name);
	name[len_name > MAX_NAME_LEN ? MAX_NAME_LEN : len_name] = 0;
	if (len_name > MAX_NAME_LEN)
	{
		/* as prepISO: cut the name, keep its extension, never inside a UTF-8 sequence */
		const char *ext = extension(name_in);
		int pos = MAX_NAME_LEN - 1 - (int)strlen(ext);

		if (pos < 1)
			return -1;
		while (pos > 0 && ((uint8_t)name[pos] & 0xC0) == 0x80)
			pos--;
		strcpy(name + pos, ext);
		len_name = (int)strlen(name);
	}
	len16 = utf16be((const uint8_t *)name, u16, (int)sizeof u16);
	memset(out, 0, FI_HDR_SIZE);
	memcpy(out + 0x8000, build_iso_data, sizeof build_iso_data);
	last_lba = get731(out + 0x8000 + 80);		/* the template's own size: 26 sectors */
	if (last_lba != FI_HDR_SECTORS)
		return -1;
	dir_record(idr, (const uint8_t *)name, len_name, (uint32_t)size, 0, last_lba);
	dir_record(idr2, u16, len16, (uint32_t)size, 0, last_lba);
	last_lba += (uint32_t)((size + 0x7FF) / 0x800);
	set733(out + 0x8000 + 80, last_lba);		/* primary volume descriptor */
	set733(out + 0x8800 + 80, last_lba);		/* Joliet's */
	return 0;
}

int fi_args(uint64_t device, const char *hdr_path, const uint32_t *lba, const uint32_t *cnt, int n, uint8_t *out, int cap)
{
	int ns = n + 1, total = 20 + 0x200 + 8 * ns, i;
	uint8_t *sec, *siz;

	if (n < 1 || !hdr_path || strlen(hdr_path) > 0x1FF || total > cap || total > FI_ARGS_MAX)
		return -1;
	memset(out, 0, total);
	be32(out, (uint32_t)(device >> 32));
	be32(out + 4, (uint32_t)device);
	be32(out + 8, FI_EMU_BD | FI_FAKE_ISO);
	be32(out + 12, (uint32_t)ns);
	be32(out + 16, 0);
	strcpy((char *)out + 20, hdr_path);
	sec = out + 20 + 0x200;
	siz = sec + 4 * ns;
	/* section 0: the header file. prepISO writes its start as 0; rawseciso.h:1073 adds 2048 x that start
	 * to the disc's size, so 0 leaves the volume's last 26 sectors past the disc's end. Prism gives it the
	 * header's own sector count, so the disc is the whole volume (its start is not read: rawseciso.h:1185) */
	be32(sec, FI_HDR_SECTORS);
	be32(siz, FI_HDR_SECTORS);
	for (i = 0; i < n; i++)
	{
		if (!cnt[i])
			return -1;
		be32(sec + 4 * (i + 1), lba[i]);
		be32(siz + 4 * (i + 1), cnt[i]);
	}
	return total;
}
