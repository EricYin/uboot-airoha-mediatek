#!/usr/bin/env bash
# airoha_pack_boot.sh - Assemble the Airoha BootROM boot image
# (optional BL1 + internal FIP, fixed 512 KiB).
#
# Layout (matches OpenWrt's AN7563 bl2-bl31-uboot.bin and the en7523 variant
# with BL1):
#   [0x00000] BL1 (truncated to 0x800 bytes), or a 2 KiB zero prefix when
#               no BL1 is supplied
#   [0x00800] internal FIP (BL2 + LZMA BL31 + LZMA U-Boot, fiptool output)
#   [0x80000] end of image (512 KiB, zero-padded)
#
# The FIP itself is produced by tools/build_airoha/Makefile via
# `fiptool create --align 1024`; this script only performs the prefix + FIP
# assembly.  It is the single pack tool for both legacy flavours: with BL1
# (CONFIG_AIROHA_LEGACY_BL1=y) it writes bl1-bl2-bl31-uboot.bin, without it the
# same FIP produces bl2-bl31-uboot.bin.  The former 0x4000 env tail region is
# kept zero-filled in both cases; the environment lives in UBI volumes
# (ubootenv/ubootenv2), so no on-image env tail is needed.
#
# The image is always padded to the full container size (512 KiB by default),
# so the script also reports the real footprint: the 2 KiB BL1 prefix region
# (always reserved) plus the FIP, as a byte/hex/KiB count and as a percentage
# of the container, together with the remaining zero padding.
#
# Usage:
#   airoha_pack_boot.sh --fip _legacy.fip -o bl2-bl31-uboot.bin
#   airoha_pack_boot.sh --bl1 bl1.bin --fip _legacy.fip -o bl1-bl2-bl31-uboot.bin
#   airoha_pack_boot.sh --bl1 bl1.bin --fip _legacy.fip -o out.bin --size 0x100000

set -euo pipefail

usage() {
	sed -n '2,31p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
	exit 0
}

BL1="" FIP="" OUT="" SIZE=$((0x80000))

while [ $# -gt 0 ]; do
	case "$1" in
		--bl1)       BL1="$2"; shift 2 ;;
		--fip)       FIP="$2"; shift 2 ;;
		-o|--output) OUT="$2"; shift 2 ;;
		--size)      SIZE=$(( $2 )); shift 2 ;;
		-h|--help)   usage ;;
		*) echo "Error: unknown option: $1" >&2; exit 1 ;;
	esac
done

[ -n "$FIP" ] || { echo "Error: --fip is required" >&2; exit 1; }
[ -n "$OUT" ] || { echo "Error: -o/--output is required" >&2; exit 1; }
[ -f "$FIP" ] || { echo "Error: $FIP not found" >&2; exit 1; }
[ "$SIZE" -gt $((0x800)) ] || { echo "Error: --size must be larger than 0x800" >&2; exit 1; }

BL1_MAX=$((0x800))   # BL1 is capped at 0x800, the FIP follows at 0x800
FIP_OFF=$((0x800))

# Zero-filled base image
head -c "$SIZE" /dev/zero > "$OUT"

# Optional BL1 at 0x0, truncated to 0x800 bytes; without it the prefix is zero
bl1_size=0
if [ -n "$BL1" ]; then
	[ -f "$BL1" ] || { echo "Error: $BL1 not found" >&2; exit 1; }
	bl1_size=$(stat -c %s "$BL1")
	[ "$bl1_size" -gt "$BL1_MAX" ] && bl1_size=$BL1_MAX
	dd if="$BL1" of="$OUT" bs=1 count="$bl1_size" conv=notrunc status=none
fi

# Internal FIP at 0x800
fip_size=$(stat -c %s "$FIP")
if [ "$fip_size" -gt $((SIZE - FIP_OFF)) ]; then
	echo "Error: FIP too large for image: $fip_size > $((SIZE - FIP_OFF)) bytes" >&2
	exit 1
fi
dd if="$FIP" of="$OUT" bs=1 seek="$FIP_OFF" conv=notrunc status=none

# Real (non-padded) footprint: the 2 KiB BL1 prefix region is always
# reserved, so the used area is FIP_OFF + FIP size.
used=$((FIP_OFF + fip_size))
free=$((SIZE - used))
pct=$((used * 1000 / SIZE))
pad_pct=$((1000 - pct))

echo "bootimg:     $OUT ($SIZE bytes, 0x$(printf '%x' "$SIZE"), FIP @0x800)"
if [ "$bl1_size" -gt 0 ]; then
	echo "bl1:         0x00000  0x$(printf '%x' "$bl1_size")"
else
	echo "bl1:         (none, 2 KiB zero prefix)"
fi
echo "fip:         0x00800  0x$(printf '%x' "$fip_size")"
echo "used:        0x$(printf '%08x' "$used")  $used bytes ($((used / 1024)) KiB) of $((SIZE / 1024)) KiB  [$((pct / 10)).$((pct % 10))%]"
echo "padding:     0x$(printf '%08x' "$free")  $free bytes ($((free / 1024)) KiB) zero-filled  [$((pad_pct / 10)).$((pad_pct % 10))%]"
echo "done"
