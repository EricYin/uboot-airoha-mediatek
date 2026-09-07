#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# Airoha OpenWrt-style chainload FIT packer
#
# Builds the chainload FIT the same way the OpenWrt build system does
# (Build/an7581-chainloader in openwrt-an7581.mk): a plain FIT whose kernel
# is the LZMA-compressed u-boot.bin.lzma (fallback: the uncompressed
# u-boot.bin) plus the board u-boot.dtb, generated with tools/mkits.sh and
# mkimage.  No shim prefix is prepended.
#
# Output (in <output-dir>, default U-Boot top level):
#   <board>-chainload-uboot.itb
#
# Options:
#   --soc <name>          SoC name (default: an7581, informational only)
#   --board <name>        Board name, e.g. w1700k / xr1710g
#   --payload <file>      Uncompressed U-Boot binary (default: <top>/u-boot.bin)
#   --payload-lzma <file> LZMA-compressed U-Boot binary (default:
#                         <payload>.lzma); used as the FIT kernel when present
#   --dtb <file>          Board DTB (default: <top>/u-boot.dtb)
#   --output-dir <dir>    Output directory (default: <top>)
#   --mkimage <path>      mkimage tool (default: <top>/tools/mkimage)
#   --mkits <path>        mkits.sh script (default: <top>/tools/mkits.sh)
#   --load-addr <hex>     Kernel load address (default: 0x80200000)
#   --entry-addr <hex>    Kernel entry point (default: 0x80200000)
#   --fdt-addr <hex>      FDT load address (default: 0x82000000)
#   --config <name>       FIT configuration name (default: conf-uboot)
#   --arch <name>         FIT architecture (default: arm64)
#   --version <str>       FIT kernel version string (default: u-boot)
#
# Exit status is 0 on success, non-zero on any error.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SOC="an7581"
BOARD=""
PAYLOAD="$UBOOT_DIR/u-boot.bin"
PAYLOAD_LZMA="$PAYLOAD.lzma"
DTB="$UBOOT_DIR/u-boot.dtb"
OUTPUT_DIR="$UBOOT_DIR"
MKIMAGE="$UBOOT_DIR/tools/mkimage"
MKITS="$UBOOT_DIR/tools/mkits.sh"
LOAD_ADDR=0x80200000
ENTRY_ADDR=0x80200000
FDT_ADDR=0x82000000
CONFIG_NAME="conf-uboot"
ARCH="arm64"
VERSION="u-boot"

while [ $# -gt 0 ]; do
	case "$1" in
		--soc)          SOC="$2"; shift 2 ;;
		--board)        BOARD="$2"; shift 2 ;;
		--payload)      PAYLOAD="$2"; shift 2 ;;
		--payload-lzma) PAYLOAD_LZMA="$2"; shift 2 ;;
		--dtb)          DTB="$2"; shift 2 ;;
		--output-dir)   OUTPUT_DIR="$2"; shift 2 ;;
		--mkimage)      MKIMAGE="$2"; shift 2 ;;
		--mkits)        MKITS="$2"; shift 2 ;;
		--load-addr)    LOAD_ADDR="$2"; shift 2 ;;
		--entry-addr)   ENTRY_ADDR="$2"; shift 2 ;;
		--fdt-addr)     FDT_ADDR="$2"; shift 2 ;;
		--config)       CONFIG_NAME="$2"; shift 2 ;;
		--arch)         ARCH="$2"; shift 2 ;;
		--version)      VERSION="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,60p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
			exit 0
			;;
		-*)
			echo "Error: unknown option: $1" >&2
			exit 1
			;;
		*)
			echo "Error: unexpected positional argument: $1" >&2
			exit 1
			;;
	esac
done

if [ -z "$BOARD" ]; then
	echo "Error: no board specified (use --board)" >&2
	exit 1
fi

# ---- input checks ----
if [ ! -f "$PAYLOAD" ]; then
	echo "Error: payload not found: $PAYLOAD" >&2
	exit 1
fi
if [ ! -f "$DTB" ]; then
	echo "Error: dtb not found: $DTB (build u-boot first)" >&2
	exit 1
fi
if [ ! -f "$MKIMAGE" ]; then
	echo "Error: mkimage not found: $MKIMAGE (build u-boot first)" >&2
	exit 1
fi
if [ ! -f "$MKITS" ]; then
	echo "Error: mkits.sh not found: $MKITS" >&2
	exit 1
fi

# Kernel: prefer the LZMA-compressed binary (COMP=lzma, like OpenWrt),
# otherwise fall back to the uncompressed one (COMP=none).
KERNEL="$PAYLOAD"
COMPRESS="none"
if [ -f "$PAYLOAD_LZMA" ]; then
	KERNEL="$PAYLOAD_LZMA"
	COMPRESS="lzma"
fi

KERNEL="$(realpath "$KERNEL")"
DTB="$(realpath "$DTB")"
MKIMAGE="$(realpath "$MKIMAGE")"
MKITS="$(realpath "$MKITS")"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"
mkdir -p "$OUTPUT_DIR"

OUTPUT_ITB="$OUTPUT_DIR/${BOARD}-chainload-uboot.itb"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# mkits.sh writes /incbin/() references using the basenames of the kernel
# and dtb files, so stage both in the temporary data dir given to mkimage -i.
cp "$KERNEL" "$TMPDIR/"
cp "$DTB" "$TMPDIR/"

echo "  [CHAINLOADER] Preparing FIT source ($SOC/$BOARD, kernel=$(basename "$KERNEL"), comp=$COMPRESS)..."
bash "$MKITS" \
	-D "$BOARD" \
	-o "$TMPDIR/u-boot.its" \
	-k "$TMPDIR/$(basename "$KERNEL")" -C "$COMPRESS" \
	-a "$LOAD_ADDR" -e "$ENTRY_ADDR" \
	-c "$CONFIG_NAME" -A "$ARCH" -v "$VERSION" \
	-d "$TMPDIR/$(basename "$DTB")" -s "$FDT_ADDR"

echo "  [CHAINLOADER] Building OpenWrt-style chainload FIT..."
"$MKIMAGE" \
	-D "-i $TMPDIR" \
	-f "$TMPDIR/u-boot.its" \
	"$OUTPUT_ITB"

echo "Done! chainload FIT: $OUTPUT_ITB ($(wc -c < "$OUTPUT_ITB") bytes)"
