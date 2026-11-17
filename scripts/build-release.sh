#!/bin/sh
# Builds the release files of one architecture into OUT:
#
#   room-linux-ARCH.tar.gz     for install.sh
#   room_VERSION-1_ARCH.deb
#   room-VERSION-1.ARCH.rpm
#
# Usage: scripts/build-release.sh ARCH BINARY OUT
# ARCH is amd64 or arm64. Run it from the repository root. It needs fpm, ar
# and rpmbuild, which the packages stage of the Dockerfile has.
set -eu

[ $# -eq 3 ] || {
    echo "Usage: $0 ARCH BINARY OUT" >&2
    exit 1
}
arch=$1 binary=$2 out=$3

case $arch in
amd64) rpm_arch=x86_64 ;;
arm64) rpm_arch=aarch64 ;;
*)
    echo "Unknown architecture: $arch" >&2
    exit 1
    ;;
esac
version=$(sed -n 's/.*define ROOM_VERSION "\(.*\)"/\1/p' src/room.h)

mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

install -m 755 "$binary" "$stage/room"
install -m 644 systemd/room.service LICENSE "$stage"
tar -czf "$out/room-linux-$arch.tar.gz" --owner=0 --group=0 -C "$stage" room room.service LICENSE

# package TYPE ARCH FILE
# With --after-upgrade, fpm doesn't run before-remove.sh on an upgrade.
package() {
    fpm -s dir -t "$1" --name room --version "$version" --iteration 1 \
        --architecture "$2" --package "$out/$3" \
        --license MIT --url https://github.com/shaiq-dev/room \
        --maintainer "shaiq-dev <shaiq-dev@users.noreply.github.com>" \
        --description "A sandbox for running untrusted code" \
        --category admin --rpm-os linux \
        --after-install packaging/linux/scripts/after-install-and-upgrade.sh \
        --after-upgrade packaging/linux/scripts/after-install-and-upgrade.sh \
        --before-remove packaging/linux/scripts/before-remove.sh \
        "$stage/room=/usr/bin/room" \
        "$stage/room.service=/usr/lib/systemd/system/room.service" \
        "$stage/LICENSE=/usr/share/doc/room/LICENSE"
}

package deb "$arch" "room_${version}-1_$arch.deb"
package rpm "$rpm_arch" "room-$version-1.$rpm_arch.rpm"
