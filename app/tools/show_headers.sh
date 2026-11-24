#!/bin/bash
I="$HOME/ps3dev/ps3dev/ppu/include"
ls "$I" | tr '\n' ' '; echo
ls "$I/sys" | tr '\n' ' '; echo
ls "$I/lv2" 2>/dev/null | tr '\n' ' '; echo
grep -n "define lv2syscall7\|define return_to_user_prog\|define lv2syscall4\b" "$I/ppu-lv2.h" | head
grep -rn "sysGetSystemTime\|sysProcessExit\|sysUsleep" "$I/sys/systime.h" "$I/sys/process.h" "$I/sys/thread.h" 2>/dev/null | head
grep -rln "storage" "$I" | head
