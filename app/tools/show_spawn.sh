#!/bin/bash
I="$HOME/ps3dev/ps3dev/ppu/include"
grep -n "ExitSpawn\|SPAWN_STACK\|PRIMARY_STACK" "$I/sys/process.h" "$I/lv2/process.h" 2>/dev/null | head -20
