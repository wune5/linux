#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Package the rubens boot.img for the mainline port.
#
#   tools/build-rubens-bootimg.sh <Image> <initramfs.cpio.gz> <out/boot.img>
#
# Geometry is taken from the stock image read back on this device (2026-09-27):
#
#   unpack_bootimg --boot_img boot_a.img --format mkbootimg
#     --header_version 4 --os_version 12.0.0 --os_patch_level 2024-09 \
#     --kernel <kernel> --ramdisk <ramdisk> --cmdline ''
#
#   stock layout: kernel 19,610,141 B + ramdisk 1,875,758 B + a 4096-byte GKI
#   boot signature, all 4096-aligned after a one-page header.
#
# Two deliberate differences from the stock image:
#
#   1. No boot signature.  mkbootimg only emits one when a GKI signing key is
#      passed, and the signature covers the stock ramdisk, which we replace.
#      The bootloader is unlocked (ro.boot.verifiedbootstate=orange) and the
#      KernelSU-repacked boot.img -- a modified ramdisk with the header fields
#      and signature left untouched, i.e. equally unverifiable -- boots on this
#      exact device, so LK is not checking it.
#
#   2. No dtb in boot.img.  CONFIG_XIAOMI_RUBENS=y makes the kernel embed the
#      rubens DTB itself and install it as initial_boot_params, overriding the
#      Android DT that LK hands over (commit 34fa0624fd).  The stock boot.img
#      has dtb_size 0 as well, so the layout is unchanged.
#
# The kernel is NOT compressed here: the stock image carries a raw arm64 Image
# and LK decompresses it.  Do not pass Image.gz.

set -eu

srcdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
image=${1:?usage: $0 <Image> <initramfs.cpio.gz> <out.img>}
ramdisk=${2:?usage: $0 <Image> <initramfs.cpio.gz> <out.img>}
out=${3:?usage: $0 <Image> <initramfs.cpio.gz> <out.img>}

for f in "$image" "$ramdisk"; do
	[ -f "$f" ] || { printf 'missing input: %s\n' "$f" >&2; exit 1; }
done
command -v mkbootimg >/dev/null 2>&1 || {
	printf '%s\n' "mkbootimg not found (install mkbootimg, or put the AOSP" \
		"mkbootimg.py on PATH as 'mkbootimg')" >&2; exit 1; }

# ---- guard: the arm64 header image_size decides whether LK boots at all ----
# "keep the kernel image within the LK image budget" (commit 6b238ad86b):
# 0x3640000 boots, 0x3820000 reset-loops.  Read the value LK reads.
python3 - "$image" <<'PY'
import struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    hdr = f.read(64)
if hdr[:4] not in (b'\x4d\x5a\x00\x00',) and hdr[56:60] != b'ARM\x64':
    # the magic is at offset 56 in the arm64 Image header
    pass
magic = hdr[56:60]
if magic != b'ARM\x64':
    print(f"warning: {p} does not look like an arm64 Image (magic {magic!r})", file=sys.stderr)
image_size = struct.unpack_from('<Q', hdr, 16)[0]
limit = 0x3640000
boots = 0x3640000
fails = 0x3820000
print(f"arm64 Image header: image_size = 0x{image_size:x} ({image_size/1048576:.1f} MiB)")
print(f"  LK budget: 0x{boots:x} boots / 0x{fails:x} reset-loops (limit {limit/1048576:.1f} MiB)")
if image_size > limit:
    sys.exit("FAIL: image_size is over the LK budget; disable CONFIG_KALLSYMS_ALL "
             "or move built-in code to modules before flashing")
print("  OK: inside the LK budget")
PY

# ---- guard: the boot ramdisk has its own, much tighter budget --------------
rdsize=$(stat -c %s "$ramdisk")
if [ "$rdsize" -gt 4194304 ]; then
	printf 'FAIL: boot ramdisk is %s bytes, over the ~4 MiB LK budget\n' "$rdsize" >&2
	exit 1
fi

# ---- pack exactly the way the working xaga build does -----------------------
#
# MT6895-Mainline's own build project (Laodelaisi/xaga-mt6895-mainline-build,
# build-mainline.sh) packs with this set and says so twice, in capitals:
#
#   "boot 头部参数, 与 postmarketOS wiki 一致"
#   "必须与 postmarketOS wiki 中 Pack boot image 中的一致，别乱改"
#
#   BASE=0x3fff8000  KERNEL_OFFSET=0x8000  PAGESIZE=4096
#   RAMDISK_OFFSET=0x26f08000  TAGS_OFFSET=0x07c88000  DTB_OFFSET=0x07c88000
#   HEADER_VERSION=4  OS_VERSION=16.0.0  OS_PATCH_LEVEL=2026-08
#
# and it gzips the kernel first:
#   gzip -n -9 -c arch/arm64/boot/Image > Image.gz
#   mkbootimg --kernel Image.gz --ramdisk initramfs.cpio.lz4 ...
#
# The kernel load address that matters is base + kernel_offset = 0x40000000.
# The previous attempts packed with mkbootimg's defaults (base 0x10000000) and
# with a raw, uncompressed Image, and the kernel came up far enough to bring the
# USB gadget online and then panicked.  A wrong load address produces exactly
# that shape of failure, so this now follows the known-good recipe.
#
# The boot signature padding below is kept: the stock image declares a
# 4096-byte signature slot and the KernelSU-repacked image that demonstrably
# boots on this device kept it too, so it costs nothing to match.
gzipped="$out.gzkernel"
gzip -n -9 -c "$image" > "$gzipped"

mkbootimg \
	--header_version 4 \
	--os_version 16.0.0 \
	--os_patch_level 2026-08 \
	--pagesize 4096 \
	--base 0x3fff8000 \
	--kernel_offset 0x8000 \
	--ramdisk_offset 0x26f08000 \
	--tags_offset 0x07c88000 \
	--dtb_offset 0x07c88000 \
	--kernel "$gzipped" \
	--ramdisk "$ramdisk" \
	--output "$out"
rm -f "$gzipped"

# ---- pad the boot signature slot so the LK-visible layout is byte-identical
# to the stock image.  mkbootimg only emits the 4096-byte boot signature when a
# GKI signing key is supplied, so it writes boot_signature_size = 0.
#
# The stock image has 4096, and the KernelSU-repacked image on this device kept
# 4096 through its repack.  An image whose declared layout differs from every
# image this bootloader is known to have accepted is a needless variable during
# bring-up: the first boot attempt produced no display and no serial output,
# which is also what a blunt "LK refuses to load the kernel" looks like.
#
# Zero padding reproduces the layout, not the signature -- the bootloader is
# unlocked and does not verify it.  PAD_SIGNATURE=0 disables this.
if [ "${PAD_SIGNATURE:-1}" = "1" ]; then
	pad=4096
	# Boot image header v4 field offsets, from mkbootimg.py:
	#   BOOT_IMAGE_HEADER_V3_SIZE = 1580  (everything before the signature
	#                                      size field)
	#   BOOT_IMAGE_HEADER_V4_SIZE = 1584  (plus the 4-byte signature size)
	#   BOOT_IMAGE_HEADER_V3_PAGESIZE = 4096  (the header is padded to a page)
	# so boot_signature_size lives at byte 1580, and the data area starts at
	# 4096.
	python3 - "$out" "$pad" <<'PY'
import struct, sys, os
path, pad = sys.argv[1], int(sys.argv[2])
SIGSIZE_OFF = 1580
PAGE = 4096
size = os.path.getsize(path)
with open(path, 'r+b') as f:
    f.seek(SIGSIZE_OFF)
    cur = struct.unpack('<I', f.read(4))[0]
    if cur == pad:
        note = f"boot_signature_size already {pad}, layout already matches stock"
    elif cur == 0:
        body = (size + PAGE - 1) // PAGE * PAGE
        f.seek(0, 2)
        if size < body:
            f.write(b'\0' * (body - size))
        f.write(b'\0' * pad)
        f.seek(SIGSIZE_OFF)
        f.write(struct.pack('<I', pad))
        note = (f"padded boot signature slot to {pad} bytes "
                f"(image {size} -> {os.path.getsize(path)})")
    else:
        note = f"unexpected boot_signature_size {cur}, left alone"
print(f"  {note}")
PY
fi

printf 'built %s (%s bytes, kernel %s, ramdisk %s)\n' \
	"$out" "$(stat -c %s "$out")" "$(stat -c %s "$image")" "$rdsize"

# ---- read it back so a broken header cannot reach fastboot ----------------
if command -v unpack_bootimg >/dev/null 2>&1; then
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	unpack_bootimg --boot_img "$out" --out "$tmp" 2>&1 | sed 's/^/  /'
fi
