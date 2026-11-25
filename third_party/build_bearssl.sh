#!/bin/bash
# BearSSL 0.6 (commit 8ef7680, bearssl.org, MIT) for the PS3 PPU, plus the host 'brssl' tool.
#   wsl -d PSBBN -- bash "/path/to/PS3 software/lr/third_party/build_bearssl.sh"
# Outputs: third_party/bearssl_ppu/libbearssl.a (PPU), third_party/bearssl_ppu/ta_mozilla.c (trust
# anchors from the host's Mozilla CA bundle, via brssl ta), third_party/bearssl_host/brssl.
# The PS3 has no /dev/urandom, getentropy, RDRAND or POWER8 crypto: all off. The app seeds the
# engine from sysGetRandomNumber() and gives the X.509 engine the time itself.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/BearSSL"
cd "$SRC"
git rev-parse HEAD | grep -q '^8ef7680081c61b486622f2d983c0d3d21e83caad$' || { echo "BearSSL is not at v0.6 (8ef7680)"; exit 1; }

# ---- host build (gcc): only for the brssl tool
H="$HERE/bearssl_host"
mkdir -p "$H"
# BUILD must be relative: make cannot cope with the space in "PS3 software"
make -s BUILD=build_host CONF=Unix > /dev/null 2>&1 || make BUILD=build_host CONF=Unix
cp build_host/brssl "$H/brssl"

# ---- PPU build
export PS3DEV="$HOME/ps3dev/ps3dev"
export PATH="$PS3DEV/ppu/bin:/usr/bin:/bin"
P="$HERE/bearssl_ppu"
rm -rf "$P/obj"; mkdir -p "$P/obj"
DEFS=(-DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0
      -DBR_RDRAND=0 -DBR_POWER8=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_64=1)
n=0
while IFS= read -r f; do
    o="$P/obj/$(echo "${f#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    ppu-gcc -O2 -mcpu=cell -ffunction-sections -fdata-sections "${DEFS[@]}" -I inc -I src -c "$f" -o "$o"
    n=$((n + 1))
done < <(find src -name '*.c' | sort)
rm -f "$P/libbearssl.a"
ppu-ar rcs "$P/libbearssl.a" "$P"/obj/*.o
echo "PPU objects: $n"
ls -l "$P/libbearssl.a"

# ---- trust anchors from the host's Mozilla bundle
CA=/etc/ssl/certs/ca-certificates.crt
"$H/brssl" ta "$CA" > "$P/ta_mozilla.c" 2> "$P/ta_mozilla.log" || true
echo "trust anchors: $(grep -c 'BR_X509_TA_CA' "$P/ta_mozilla.c") from $CA ($(grep -c 'BEGIN CERTIFICATE' "$CA") certs)"
grep -c '' "$P/ta_mozilla.log" | sed 's/^/brssl ta warnings: /'
