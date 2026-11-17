#!/bin/sh
# Installs room from a GitHub release. Under systemd it also installs
# room.service and starts it.
#
#   curl -fsSL https://raw.githubusercontent.com/shaiq-dev/room/main/install.sh | sudo sh
#
# Environment:
#   ROOM_VERSION       the release, such as 0.1.0. The default is the latest
#   DESTINATION        the directory for the binary. The default is /usr/local/bin
#   ROOM_DOWNLOAD_URL  the URL of the release files, for tests
set -eu

die() {
    echo "install.sh: $*" >&2
    exit 1
}

repo=https://github.com/shaiq-dev/room
if [ "${ROOM_VERSION:-latest}" = latest ]; then
    url=$repo/releases/latest/download
else
    url=$repo/releases/download/v$ROOM_VERSION
fi
url=${ROOM_DOWNLOAD_URL:-$url}
dest=${DESTINATION:-/usr/local/bin}

[ "$(uname -s)" = Linux ] || die "room only runs on Linux"
case $(uname -m) in
x86_64 | amd64) arch=amd64 ;;
aarch64 | arm64) arch=arm64 ;;
*) die "room has no build for $(uname -m), only for x86_64 and aarch64" ;;
esac
[ "$(id -u)" -eq 0 ] || die "run this as root"

if command -v curl > /dev/null; then
    get() { curl -fsSL "$1"; }
elif command -v wget > /dev/null; then
    get() { wget -qO- "$1"; }
else
    die "needs curl or wget"
fi
command -v sha256sum > /dev/null || die "needs sha256sum"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

file=room-linux-$arch.tar.gz
echo "Downloading $url/$file"
get "$url/$file" > "$tmp/$file" || die "cannot download $url/$file"
want=$(get "$url/SHA256SUMS" | awk -v f="$file" '$2 == f { print $1 }')
[ -n "$want" ] || die "cannot find the checksum of $file in $url/SHA256SUMS"
got=$(sha256sum "$tmp/$file" | cut -d ' ' -f 1)
[ "$got" = "$want" ] || die "$file has the wrong checksum. Try again"

tar -xzf "$tmp/$file" -C "$tmp"
mkdir -p "$dest"
install -m 755 "$tmp/room" "$dest/room"
echo "Installed $("$dest/room" --version) to $dest/room"

# Under systemd room only uses the cgroup of room.service.
if [ -d /run/systemd/system ]; then
    install -m 644 "$tmp/room.service" /etc/systemd/system/room.service
    systemctl daemon-reload
    systemctl enable --now room.service
    echo "Enabled and started room.service"
fi
