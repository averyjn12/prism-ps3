#!/bin/bash
I="$HOME/ps3dev/ps3dev/ppu/include"
grep -rn "closesocket\|socketclose\|int close\b\|netClose\|#define close" "$I/sys/socket.h" "$I/net/net.h" "$I/net/socket.h" 2>/dev/null | head
grep -n "SO_RCVTIMEO\|SO_SNDTIMEO" "$I/sys/socket.h" | head -3
ls "$I/net"
grep -rn "socket(\|connect(\|closesocket\|close(" "$HOME/ps3dev/src/PSL1GHT/samples/network/httptest/source/http.c" | head
grep -n "SYSMODULE_NET\b\|SYSMODULE_NETCTL" "$I/sysmodule/sysmodule.h" | head -3
