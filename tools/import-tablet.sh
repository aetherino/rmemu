#!/bin/sh
# Turn the firmware running on your own Paper Pro (developer mode, over SSH) into a
# linux/arm64 Docker image (rmpp-rootfs:<version>), for versions the firmware index doesn't
# have yet:
#
#   tools/import-tablet.sh [root@10.11.99.1]
#   docker build -t rmemu:<version> --build-arg ROOTFS=rmpp-rootfs:<version> -f docker/Dockerfile .
#   bin/rmemu start <version>
#
# It reads the root partition through a bind mount of / (so not the RAM overlay firmware 6
# puts on /etc), and leaves out every file changed after the firmware was built, such as
# units installed in developer mode, so the image is the stock filesystem.
set -eu
host=${1:-root@10.11.99.1}
version=$(ssh "$host" 'sed -n "s/^IMG_VERSION=\"*\([^\"]*\)\"*/\1/p" /etc/os-release')
[ -n "$version" ] || { echo "can't read the firmware version from $host" >&2; exit 1; }

ssh "$host" sh -s <<'EOF' >&2
mkdir -p /run/rmemu-img && mount --bind / /run/rmemu-img
cd /run/rmemu-img && find . -xdev ! -type d -newer etc/version >/run/rmemu-img.skip
cd / && umount /run/rmemu-img
if [ -s /run/rmemu-img.skip ]; then echo "leaving out files changed since the build:"; sed 's/^/  /' /run/rmemu-img.skip; fi
EOF
ssh "$host" 'mount --bind / /run/rmemu-img && tar -C /run/rmemu-img -X /run/rmemu-img.skip -cf - .; s=$?
             umount /run/rmemu-img; rmdir /run/rmemu-img; rm -f /run/rmemu-img.skip; exit $s' |
    docker import --platform linux/arm64 - "rmpp-rootfs:$version" >/dev/null
echo "rmpp-rootfs:$version"
