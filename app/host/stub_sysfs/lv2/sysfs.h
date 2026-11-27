/* Host stand-in for PSL1GHT's lv2/sysfs.h (v0.81, host/logcopy_test.c): the console's paths are mapped
 * under FAKE_ROOT on the PC. Only what logcopy.c and dirlist.c use. */
#ifndef HOST_FAKE_SYSFS_H
#define HOST_FAKE_SYSFS_H
#include <ppu-types.h>

#define SYS_O_RDONLY	0x000000
#define SYS_O_WRONLY	0x000001
#define SYS_O_RDWR	0x000002
#define SYS_O_CREAT	0x000040
#define SYS_O_TRUNC	0x000200
#define SYS_O_APPEND	0x000400

typedef struct
{
	s32 st_mode;
	s32 st_uid, st_gid;
	s64 at_, mt_, ct_;	/* st_atime & co: glibc's sys/stat.h makes those names macros */
	u64 st_size;
	u64 st_blksize;
} sysFSStat;

typedef struct
{
	u8 d_type;
	u8 d_namlen;
	char d_name[256];
} sysFSDirent;

typedef struct
{
	sysFSStat attribute;
	sysFSDirent entry_name;
} sysFSDirectoryEntry;

s32 sysFsOpen(const char *path, s32 oflags, s32 *fd, const void *arg, u64 argsize);
s32 sysFsRead(s32 fd, void *ptr, u64 len, u64 *nread);
s32 sysFsWrite(s32 fd, const void *ptr, u64 size, u64 *written);
s32 sysFsClose(s32 fd);
s32 sysFsStat(const char *path, sysFSStat *st);
s32 sysFsMkdir(const char *path, s32 mode);
s32 sysFsChmod(const char *path, s32 mode);
s32 sysFsOpendir(const char *path, s32 *fd);
s32 sysFsGetDirectoryEntries(s32 fd, sysFSDirectoryEntry *entries, u32 size, u32 *count);
s32 sysFsClosedir(s32 fd);
#endif
