#!/bin/bash
I="$HOME/ps3dev/ps3dev/ppu/include"
S="$HOME/ps3dev/src/PSL1GHT/samples"
grep -n "sysUtilRegisterCallback\|sysUtilCheckCallback\|SYSUTIL_EXIT_GAME\|SYSUTIL_EVENT_SLOT0\|typedef.*sysutilCallback" "$I/sysutil/sysutil.h"
grep -n "sysFsOpendir\|sysFsReaddir\|sysFsClosedir\|sysFSDirent\|sysFsUnlink\|s32 sysFsOpen\b\|sysFsStat\b" "$I/lv2/sysfs.h" | head -12
grep -n "typedef struct _sys_fs_dirent" -A6 "$I/lv2/sysfs.h"
grep -n "netInitialize\|netDeinitialize" "$I/net/net.h"
ls "$S/network/httptest/source"; sed -n 1,60p "$S/network/networktest/source/"*.c 2>/dev/null | head -70
grep -n "BTN_CROSS\|BTN_CIRCLE\|BTN_UP\|BTN_L1\|BTN_R1\|BTN_START" "$I/io/pad.h" | head -10
