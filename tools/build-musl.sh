#!/bin/bash
# tools/build-musl.sh — clone and build musl-blueyos for i386
# Used by 'make musl' in the yap build system.
#
# Usage: ./tools/build-musl.sh [--prefix /path/to/sysroot]
set -e

PREFIX="build/musl"
REPO="https://github.com/nzmacgeek/musl-blueyos.git"
SRC_DIR="build/musl-src"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        *)        echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo ""
echo "  [MUSL] Building musl-blueyos for i386"
echo "         Source:  $SRC_DIR"
echo "         Prefix:  $PREFIX"
echo ""

mkdir -p "$(dirname "$SRC_DIR")"

if [ ! -d "$SRC_DIR" ]; then
    echo "  [MUSL] Cloning $REPO ..."
    git clone --depth=1 "$REPO" "$SRC_DIR"
else
    echo "  [MUSL] Using existing source in $SRC_DIR"
fi

cd "$SRC_DIR"
mkdir -p "$PREFIX"

echo "  [MUSL] Configuring for i386 ..."
./configure \
    --prefix="$(realpath "$PREFIX")" \
    --target=i386-linux-musl \
    CFLAGS="-m32" \
    LDFLAGS="-m32"

echo "  [MUSL] Building ..."
make -j"$(nproc)"

echo "  [MUSL] Installing into $PREFIX ..."
make install

echo ""
echo "  [MUSL] musl-blueyos installed at $PREFIX"
echo ""
