#!/bin/sh
set -eu

umask 022
src=/src/wiggle-monitor.c
out=/out/wiggle-monitor

test -f "$src"
test -d /out

echo "source:"
sha256sum "$src"
echo "toolchain:"
gcc --version | sed -n '1p'
ld --version | sed -n '1p'
strip --version | sed -n '1p'
pkg-config --version | sed 's/^/pkg-config /'
echo "dependencies:"
dpkg-query -W -f='${Package}=${Version}\n' \
  gcc gcc-12 cpp-12 libgcc-12-dev libgcc-s1 libc6 libc6-dev linux-libc-dev \
  libevdev2 libevdev-dev pkg-config pkgconf pkgconf-bin binutils \
  binutils-common binutils-x86-64-linux-gnu | LC_ALL=C sort

gcc \
  -O2 \
  -Wall -Wextra -Werror -Wdate-time -Wformat -Wformat-security -pedantic \
  -std=c11 \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  -fPIE -fno-record-gcc-switches \
  -ffile-prefix-map=/src=. -fdebug-prefix-map=/src=. \
  -Wl,--as-needed -Wl,--build-id=none -Wl,-z,relro,-z,now -pie \
  -o /tmp/wiggle-monitor.unstripped \
  "$src" \
  $(pkg-config --cflags --libs libevdev) \
  -lm

strip --strip-unneeded --enable-deterministic-archives \
  /tmp/wiggle-monitor.unstripped
cp /tmp/wiggle-monitor.unstripped "$out"
chmod 0755 "$out"
