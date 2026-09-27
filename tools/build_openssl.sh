#!/usr/bin/env bash
# Build the pinned OpenSSL in an isolated prefix, without replacing system TLS.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${1:-$root/.local/openssl-1.1.1w}"
mkdir -p "$prefix" "$root/build-deps"
prefix="$(cd "$prefix" && pwd)"
cd "$root/build-deps"
archive=openssl-1.1.1w.tar.gz
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 2 "https://www.openssl.org/source/old/1.1.1/$archive" -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8  $archive" | sha256sum -c -
tar -xzf "$archive"
cd openssl-1.1.1w
./config --prefix="$prefix" --openssldir="$prefix/ssl" no-shared
make -j"${JOBS:-2}"
make install_sw
"$prefix/bin/openssl" version
