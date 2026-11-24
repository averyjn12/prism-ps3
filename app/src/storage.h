/* lv2 storage syscalls (no PSL1GHT header has them). Device IDs from psdevwiki "DeviceID". */
#ifndef PRISM_APP_STORAGE_H
#define PRISM_APP_STORAGE_H
#include <stdint.h>

#define ST_DEV_MS	0x103000000000010ULL
#define ST_DEV_SD	0x103000100000010ULL
#define ST_DEV_CF	0x103000200000010ULL
#define ST_DEV_CARDCTRL	0x101000000000010ULL
#define ST_DEV_USB000	0x10300000000000AULL

/* layout as webMAN-MOD's device_info_t */
typedef struct
{
	uint8_t name[7];
	uint8_t unknown01;
	uint32_t unknown02;
	uint32_t zero01;
	uint32_t unknown03;
	uint32_t unknown04;
	uint8_t zero02[16];
	uint64_t total_sectors;
	uint32_t sector_size;
	uint32_t unknown05;
	uint8_t writable;
	uint8_t unknown06[3];
	uint32_t unknown07;
} __attribute__((packed)) st_devinfo;

int32_t st_info(uint64_t dev, st_devinfo *info);
int32_t st_open(uint64_t dev, uint32_t *fd);
int32_t st_close(uint32_t fd);
int32_t st_read(uint32_t fd, uint32_t lba, uint32_t n, void *buf, uint32_t *got);

int fsport_attach(uint64_t dev);
void fsport_detach(void);
int fsport_raw(uint32_t lba, uint32_t n, void *buf);
extern uint32_t fsport_reads, fsport_errs;

#endif
