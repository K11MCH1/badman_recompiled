#!/bin/sh
# Build the minimal LGPL FFmpeg shared libraries the Badman host loads on
# Android arm64: H.264 decoder/parser and ATRAC3plus, plus swscale/swresample.
# Same upstream commit as the pinned Windows build (n8.1.3-9, LGPL-2.1+).
#
# Usage: build_ffmpeg_android.sh <unpacked FFmpeg source> <install prefix> <NDK root>
# Source: https://github.com/FFmpeg/FFmpeg/archive/29e619e767cde9045a75c29bc9a8278ae7b3a98b.tar.gz
# (tarball SHA-256 8e4699bcb6a311772e8f513cc72ce27328891b82a598b82e7cff3dfe580875a0).
# On Windows run from Git Bash: build in-tree, pass Windows-style (C:/...)
# prefix/NDK paths, and use the NDK's make. The libraries stay outside the
# repository and are distributed separately with their license and source.
set -e
SRC=$1 PREFIX=$2 NDK=$3
case "$(uname -s)" in MINGW*|MSYS*) HOST=windows-x86_64; EXE=.exe ;; Linux) HOST=linux-x86_64; EXE= ;; *) HOST=darwin-x86_64; EXE= ;; esac
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
MAKE=make; [ -x "$NDK/prebuilt/$HOST/bin/make$EXE" ] && MAKE=$NDK/prebuilt/$HOST/bin/make$EXE
cd "$SRC"
mkdir -p tmp-build
export TMPDIR="$PWD/tmp-build" TEMP="$PWD/tmp-build" TMP="$PWD/tmp-build"
./configure --prefix="$PREFIX" --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
    --cc="$TC/clang$EXE" --cxx="$TC/clang++$EXE" --ld="$TC/clang$EXE" --ar="$TC/llvm-ar$EXE" \
    --nm="$TC/llvm-nm$EXE" --ranlib="$TC/llvm-ranlib$EXE" --strip="$TC/llvm-strip$EXE" \
    --host-cc="$TC/clang$EXE" --host-cflags="--target=x86_64-linux-android29" --host-ldflags="--target=x86_64-linux-android29" \
    --extra-cflags="--target=aarch64-linux-android29" \
    --extra-ldflags="--target=aarch64-linux-android29 -Wl,-z,max-page-size=16384" \
    --enable-shared --disable-static --enable-pic --disable-programs --disable-doc --disable-network \
    --disable-avdevice --disable-avfilter --disable-avformat --disable-everything --disable-autodetect --disable-iconv \
    --enable-decoder=h264,atrac3p --enable-parser=h264 --enable-swscale --enable-swresample --disable-debug
"$MAKE" -j8
"$MAKE" install
