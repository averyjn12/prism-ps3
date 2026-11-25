#!/bin/bash
# NVIDIA's Cg Toolkit 3.1 (April 2012), which PSL1GHT's cgcomp loads at run time to compile the two RSX
# programs in app/src/shaders. Only libCg.so is used; it goes to ~/cg, nothing is installed system-wide.
# The toolkit is NVIDIA's (its licence is in the archive: usr/local/Cg/docs/license.txt); it is fetched,
# never kept in this repository.
set -e
mkdir -p "$HOME/cg" && cd "$HOME/cg"
curl -sL -o cg.tgz https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz
echo "e8ff01e6cc38d1b3fd56a083f5860737dbd2f319a39037528fb1a74a89ae9878  cg.tgz" | sha256sum -c -
tar xzf cg.tgz --wildcards "*usr/lib64/libCg.so" "*license.txt"
ls -l usr/lib64/libCg.so
