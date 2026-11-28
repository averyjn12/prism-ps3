/* v0.60: PSP games straight from a card slot (Bananafuzzbuddy 2026-11-24: "get the psp games to play from the sd card
 * without having to copy it to the hdd"). The parts that need no console, tested on the PC by
 * host/pspsd_test.sh.
 *
 * Why: Cobra opens a PSP UMD image with lv2's own cellFsOpen in the PSP EMULATOR's process (Mamba/Cobra
 * stage2 psp.c sys_psp_read_header), and a /dev_sd path fails there: the launcher starts, the game does not
 * (v0.30). webMAN plays PSP images from NTFS / exFAT USB drives, which lv2 cannot open either, by serving a
 * virtual disc on /dev_bdvd whose bytes come from the drive's raw sectors (include/mount/rawseciso.h,
 * prepISO's .ntfs[PSPISO] files) and pointing Cobra at /dev_bdvd/<the image>. Prism's card reader already
 * knows every run of sectors of every file on the card, so Prism writes the same file for the card. */
#ifndef PRISM_APP_PSPSD_CORE_H
#define PRISM_APP_PSPSD_CORE_H
#include <stdint.h>

#define FI_HDR_SIZE	0xD000				/* the fake volume's header: 26 sectors of 2048 */
#define FI_HDR_SECTORS	(FI_HDR_SIZE / 2048)
#define FI_ARGS_MAX	0x10000				/* webMAN reads at most 64 KB of the .ntfs file */
#define FI_EMU_BD	5				/* webMAN-MOD cobra.h: EMU_OFF, PS3, PS2_DVD, PS2_CD, PSX, BD */
#define FI_FAKE_ISO	0x800				/* rawseciso mode_file 2: section 0 is a file (the header) */

/* the header of a volume holding one file, name (UTF-8), size bytes, at sector FI_HDR_SECTORS: the ISO
 * 9660 and Joliet records webMAN-MOD's create_fake_file_iso_mem writes. out: FI_HDR_SIZE bytes. 0 / -1 */
int fi_header(const char *name, uint64_t size, uint8_t *out);
/* the .ntfs[PSPISO] bytes (big-endian, as the PS3 reads them): device, the header's path on the console,
 * and the image's runs on the device (lba, cnt in the device's own 512-byte sectors). Bytes, or -1 */
int fi_args(uint64_t device, const char *hdr_path, const uint32_t *lba, const uint32_t *cnt, int n, uint8_t *out, int cap);

/* v0.82: a PS3 game ISO on a card, played through webMAN's rawseciso (Bananafuzzbuddy 2026-11-27: "get the exfat to work for ps3
 * games"; src/ps3sd.c). The .ntfs[PS3ISO] layout (webMAN-MOD _Projects_/prepISO/source/main.c rawseciso_args and
 * include/prep.h, lr/re_notes/webman_rawiso_contract_2026-11-27.md): u64 device, u32 emu_mode 1 (EMU_PS3, raw mode:
 * every section is device sectors, no header file), u32 sections, u32 tracks 0, u32 lba[n], u32 cnt[n], big-endian.
 * Bytes (20 + 8n), or -1 */
#define FI_EMU_PS3	1
int ps3_args(uint64_t device, const uint32_t *lba, const uint32_t *cnt, int n, uint8_t *out, int cap);
#endif
