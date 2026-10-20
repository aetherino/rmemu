#!/bin/sh
# Turn a Paper Pro .swu firmware package into a linux/arm64 Docker image
# (rmpp-rootfs:<version>). No root or privileged containers needed: the ext4
# image is read with debugfs inside a throwaway Alpine container.
set -eu
swu=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
version=$(basename "$swu" | sed -E 's/.*image-([0-9.]+)-.*/\1/')
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && cpio -id --quiet < "$swu")
gunzip -c "$work"/*.ext4.verity.gz > "$work/rootfs.ext4"
docker run --rm --platform linux/arm64 -v "$work":/fw:ro alpine:3.20 sh -c \
    'apk add -q e2fsprogs-extra >/dev/null && mkdir /out && debugfs -R "rdump / /out" /fw/rootfs.ext4 >/dev/null 2>&1; tar -C /out --numeric-owner -cf - .' |
    docker import --platform linux/arm64 - "rmpp-rootfs:$version" >/dev/null
echo "rmpp-rootfs:$version"
