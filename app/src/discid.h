#ifndef PRISM_APP_DISCID_H
#define PRISM_APP_DISCID_H
/* v0.72: the serial of a PS1 / PS2 disc image ("SLUS_005.94", always capital), read through rd: len bytes
 * at byte off of the image, 0 = read. Tried as a 2048-byte ISO, then as raw 2352-byte CD sectors.
 * 0 = the image has a volume and a SYSTEM.CNF (out may still be empty when that names no boot file);
 * -1 = no ISO9660 volume in any layout; -2..-4 = a volume, but its root or SYSTEM.CNF could not be read.
 * out holds 12 bytes. *is_cd: the volume calls itself a CD (Cobra's rule). *raw: 2352-byte sectors. */
typedef int (*disc_rd)(void *ctx, unsigned long long off, unsigned char *buf, unsigned len);
int disc_id(void *ctx, disc_rd rd, char *out, int *is_cd, int *raw);
#endif
