#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build a SIZE-BOUNDED rubens initramfs.
#
#   tools/build-rubens-initramfs-min.sh <output.cpio.gz> \
#           [alpine-minirootfs.tar.gz] [busybox-static.apk]
#
# Why this exists (measured on this tree, 2026-09-27):
#   tools/build-rubens-rootfs.sh packs the whole Alpine minirootfs and comes out
#   at 5,390,767 bytes (5.14 MiB) with the rubens init.  The stock LK resets the
#   device before Linux starts when the boot ramdisk grows past roughly 4 MB
#   (see tools/build-rubens-vendor-ramdisk.sh: a 5.3 MB ramdisk is a documented
#   hard failure), so the full Alpine tree does not fit.
#
#   Two things make the oversized image:
#     1. Alpine ships OpenSSL for apk -- libcrypto.so.3 (4.4 MiB), libssl.so.3
#        (0.9 MiB) and the ca-certificates bundle (0.2 MiB) are 63% of the
#        image and nothing in this initramfs uses them.
#     2. Alpine's `busybox` package only provides a subset of applets as
#        symlinks; the init script needs awk, cmp, od, find and chvt, which
#        live in gawk/diffutils/coreutils/findutils/kbd -- five packages, their
#        shared libraries and their post-install scripts, to get five binaries.
#
#   A single statically linked busybox replaces both problems: it carries every
#   applet the init script uses, so the musl loader and every shared library can
#   go away as well.
#
#   Result: ~1.2 MiB gzip, comfortably inside the LK budget.
#
# Everything included is required by tools/rubens-rootfs-init.sh:
#   - /init                      the script itself
#   - /bin/busybox               static; sh, mount, umount, find, awk, cmp, od,
#                                grep, sed, dd, chvt, dmesg, switch_root, ...
#   - /dev/console, /dev/null    early console before devtmpfs
#   - empty mount points /proc /sys /dev /run /tmp /mnt

set -eu

srcdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: $0 <output.cpio.gz> [alpine-tarball] [busybox-static.apk]}

ALPINE_VER=${ALPINE_VER:-3.20.3}
BB_VER=${BB_VER:-1.36.1-r31}
base_url=${ALPINE_URL:-https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64}
tarball=${2:-/tmp/alpine-minirootfs-$ALPINE_VER-aarch64.tar.gz}
apk=${3:-/tmp/busybox-static-$BB_VER.apk}
apk_url=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64/busybox-static-$BB_VER.apk

# LK refuses to boot past roughly this ramdisk size.
BUDGET=${LK_RAMDISK_BUDGET:-4194304}

# Local fork of the init script (diagnostics + fixed PID 1 tail); falls back to
# the upstream script when it is not present.
LOCAL_INIT=${LOCAL_INIT:-$srcdir/../../../local/rubens-initramfs-init-fixed.sh}
# $srcdir is <root>/kport/tools, so the port root is two levels up.
[ -f "$LOCAL_INIT" ] || LOCAL_INIT="$srcdir/../../local/rubens-initramfs-init-fixed.sh"

gen_init_cpio=${GEN_INIT_CPIO:-$srcdir/../usr/gen_init_cpio}
if [ ! -x "$gen_init_cpio" ]; then
	printf '%s\n' "gen_init_cpio not found at $gen_init_cpio" \
		"build it first:  make -C <kernel> usr/gen_init_cpio" >&2
	exit 1
fi

[ -f "$tarball" ] || {
	printf '%s\n' "downloading $base_url/alpine-minirootfs-$ALPINE_VER-aarch64.tar.gz"
	curl -fL --retry 3 -o "$tarball" \
		"$base_url/alpine-minirootfs-$ALPINE_VER-aarch64.tar.gz"; }
[ -f "$apk" ] || {
	printf '%s\n' "downloading $apk_url"
	curl -fL --retry 3 -o "$apk" "$apk_url"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# ---- the init script ------------------------------------------------------
# A local fork is preferred when present: upstream ends PID 1 with
#   while :; do wait; sleep 3600; done
# and `wait` is not an applet of the static busybox used here, so the loop
# degrades to an endless sleep and the hot path trips the softlockup detector.
INIT_SRC="$srcdir/rubens-rootfs-init.sh"
if [ -f "$LOCAL_INIT" ]; then
	INIT_SRC="$LOCAL_INIT"
	printf 'init script: %s (local fork)\n' "$INIT_SRC"
else
	printf 'init script: %s (upstream)\n' "$INIT_SRC"
fi
install -m 0755 "$INIT_SRC" "$work/init"

# ---- the shell: one static busybox for everything -------------------------
# An Alpine .apk is a plain gzip'd tar.
tar -xzf "$apk" -C "$work" ./bin/busybox.static 2>/dev/null || \
	tar -xzf "$apk" -C "$work" bin/busybox.static
install -D -m 0755 "$work/bin/busybox.static" "$work/bin/busybox"
rm -f "$work/bin/busybox.static"

# Applets the init script calls (plus the ones a rescue shell needs).
APPLETS="sh ash awk cat chvt cmp cp cut dd dmesg echo find grep head kill
         ln ls mkdir mknod mount mountpoint mv od pivot_root printenv ps pwd
         readlink rm rmdir sed sleep sort stat switch_root sync tail tar tee
         touch tr true false umount uname wc which xargs
         chmod chown date df du expr free hexdump id ifconfig ip loadkmap
         logger lsmod md5sum microcom modprobe more netstat nslookup ping
         poweroff reboot reset rmmod route run-parts seq setconsole sh
         sha256sum strings stty test timeout top vi watch yes"

symlink_applets() {
	local a
	for a in $APPLETS; do
		ln -sf /bin/busybox "$work/bin/$a"
	done
}
symlink_applets

# ---- runtime directories --------------------------------------------------
mkdir -p "$work/dev" "$work/proc" "$work/sys" "$work/run" "$work/tmp" \
         "$work/mnt" "$work/etc"
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$work/etc/passwd"
printf 'root:x:0:\n' > "$work/etc/group"

# ---- pack with a spec (no root needed; device nodes come from the spec) ----
spec=$work/.spec
: > "$spec"
printf 'nod /dev/console 0600 0 0 c 5 1\n' >> "$spec"
printf 'nod /dev/null 0666 0 0 c 1 3\n' >> "$spec"
find "$work" -mindepth 1 ! -name .spec -printf '%y %m %p %l\n' \
| while read -r type mode path link; do
	rel=${path#"$work"}
	case "$type" in
	d) printf 'dir %s %s 0 0\n' "$rel" "$mode" ;;
	f) printf 'file %s %s %s 0 0\n' "$rel" "$path" "$mode" ;;
	# gen_init_cpio format: slink <name> <target> <mode> <uid> <gid>
	l) printf 'slink %s %s %s 0 0\n' "$rel" "$link" "$mode" ;;
	esac
done >> "$spec"

entries=$(wc -l < "$spec")
"$gen_init_cpio" "$spec" | gzip -9 > "$out"
rm -f "$spec"

size=$(stat -c %s "$out")
printf 'built %s (%s bytes = %.2f MiB, %s entries)\n' \
	"$out" "$size" "$(awk -v s="$size" 'BEGIN{print s/1048576}')" "$entries"
if [ "$size" -gt "$BUDGET" ]; then
	printf 'FAIL: %s bytes exceeds the ~%s-byte LK boot-ramdisk budget\n' \
		"$size" "$BUDGET" >&2
	exit 1
fi
printf 'OK: %s bytes of headroom against the %s-byte budget\n' \
	"$((BUDGET - size))" "$BUDGET"
