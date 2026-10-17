#!/usr/bin/env bash
# Unmodified upstream test dependency, built only in the ignored spike build dir.
set -euo pipefail
cd "$(dirname "$0")/.."
version=1.2.6
checksum=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
prefix="$PWD/build/musl-prefix"
if [[ ! -x "$prefix/bin/musl-gcc" ]]; then
  mkdir -p build/musl-src
  archive="$PWD/build/musl-$version.tar.gz"
  if [[ ! -f "$archive" ]]; then curl -fsSL --max-time 60 "https://musl.libc.org/releases/musl-$version.tar.gz" -o "$archive"; fi
  printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c - >&2
  tar -xzf "$archive" -C build/musl-src --strip-components=1
  (
    cd build/musl-src
    ./configure --prefix="$prefix" --syslibdir="$prefix/lib" > ../musl-config.log
    make -j4 > ../musl-make.log
    make install > ../musl-install.log
  )
fi
printf '%s\n' "$prefix/bin/musl-gcc"
