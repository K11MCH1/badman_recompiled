#!/bin/sh
# Build the external pspdecrypt (GPLv3, John-K, commit e1c1dd6a) for Android
# arm64 as a separate program. It is never linked into PSPRecomp; ship it with
# its own license and corresponding source. OpenSSL's SHA-256 is replaced by
# the independent openssl/sha.h overlay next to this script.
#
# Usage: build_pspdecrypt_android.sh <unpacked pspdecrypt source> <output executable> <NDK root>
# Source: https://github.com/John-K/pspdecrypt/archive/e1c1dd6adccc84e0b95c0036d5343237fb06d7d5.tar.gz
# (tarball SHA-256 45cf01506bb665f157ea94e5c79056f3548a7917b09167f0f66b3a911c9b656e;
#  LICENSE.TXT SHA-256 8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903).
set -e
SRC=$1 OUT=$2 NDK=$3
OVERLAY=$(cd "$(dirname "$0")/pspdecrypt-overlay" && pwd)
case "$(uname -s)" in MINGW*|MSYS*) HOST=windows-x86_64; EXE=.exe ;; Linux) HOST=linux-x86_64; EXE= ;; *) HOST=darwin-x86_64; EXE= ;; esac
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
TARGET=--target=aarch64-linux-android29
OBJ=$(mktemp -d)
for f in "$SRC"/*.c "$SRC"/libkirk/*.c; do
    "$TC/clang$EXE" $TARGET -O2 -fPIE -w -I"$OVERLAY" -I"$SRC" -I"$SRC/libkirk" -c "$f" -o "$OBJ/$(basename "$f" .c)-c.o"
done
for f in "$SRC"/*.cpp; do
    "$TC/clang++$EXE" $TARGET -O2 -fPIE -w -std=c++17 -I"$OVERLAY" -I"$SRC" -I"$SRC/libkirk" -c "$f" -o "$OBJ/$(basename "$f" .cpp)-cpp.o"
done
"$TC/clang++$EXE" $TARGET -pie -static-libstdc++ -Wl,-z,max-page-size=16384 -o "$OUT" "$OBJ"/*.o -lz
