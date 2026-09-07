#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# Airoha chainloader image packer (generic)
#
# Bundles the prebuilt chainloader shim, the per-board control DTS and the
# freshly built u-boot.bin into the chainloader FIT and combined slot images.
#
# Inputs (per-board sources, tools/build_airoha/chainloader/<soc>_<board>/):
#   shim.bin        ARM64 shim executable
#   control.dts     chainloader control device tree source
#   chainloader.its FIT image source template (__DTB__/__SHIM__/__PAYLOAD__/
#                   __KCOMP__ placeholders)
#
# Outputs (in <output-dir>, default U-Boot top level):
#   <board>-chainloader-prefix-shim.uImage  legacy uImage carrying the
#                                           LZMA-compressed shim, with the
#                                           ih_os byte patched to U-Boot (0x03)
#   <board>-chainloader-control.dtb         compiled control DTB
#   <board>-chainloader.itb                 chainloader FIT image
#   <board>-chainloader-slot.bin            combined slot image:
#                                           [0x0 .. 0x2100) prefix shim
#                                           [0x2100 .. end) FIT
#
# Usage:
#   airoha_pack_chainloader.sh [payload] [output_dir] [mkimage] [dumpimage] [board]
#   airoha_pack_chainloader.sh [options]
#
# Options (override the positional arguments):
#   --soc <name>          SoC directory name (default: an7581)
#   --board <name>        Board name, e.g. xg2010g / xr1710g
#   --payload <file>      U-Boot binary to embed (default: <top>/u-boot.bin)
#   --output-dir <dir>    Output directory (default: <top>)
#   --mkimage <path>      mkimage tool (default: <top>/tools/mkimage)
#   --dtc <path>          dtc compiler (default: <top>/scripts/dtc/dtc)
#   --dumpimage <path>    dumpimage tool, used to verify the FIT (optional)
#   --lzma <cmd>          LZMA encoder command (default: auto lzma/xz)
#   --prefix-offset <hex> FIT offset inside the slot image (default: 0x2100)
#   --shim-addr <hex>     Shim load/entry address (default: 0x80088000)
#
# Exit status is 0 on success, non-zero on any error.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CHAINLOADER_DIR="$UBOOT_DIR/tools/build_airoha/chainloader"

# ---- defaults (overridable by positional args / options) ----
SOC="an7581"
BOARD=""
PAYLOAD="$UBOOT_DIR/u-boot.bin"
OUTPUT_DIR="$UBOOT_DIR"
MKIMAGE="$UBOOT_DIR/tools/mkimage"
DUMPIMAGE="$UBOOT_DIR/tools/dumpimage"
DTC="$UBOOT_DIR/scripts/dtc/dtc"
LZMA_CMD=""
PREFIX_OFFSET=$((0x2100))
SHIM_ADDR=0x80088000
IH_OS_U_BOOT=3

# Remember which values were explicitly set by options, so that the legacy
# positional arguments (payload output_dir mkimage dumpimage board) only fill
# in the slots that were not already provided via options.
_soc_set=0
_board_set=0
_payload_set=0
_output_set=0
_mkimage_set=0
_dumpimage_set=0
POS_ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
		--soc)           SOC="$2"; _soc_set=1; shift 2 ;;
		--board)         BOARD="$2"; _board_set=1; shift 2 ;;
		--payload)       PAYLOAD="$2"; _payload_set=1; shift 2 ;;
		--output-dir)    OUTPUT_DIR="$2"; _output_set=1; shift 2 ;;
		--mkimage)       MKIMAGE="$2"; _mkimage_set=1; shift 2 ;;
		--dtc)           DTC="$2"; shift 2 ;;
		--dumpimage)     DUMPIMAGE="$2"; _dumpimage_set=1; shift 2 ;;
		--lzma)          LZMA_CMD="$2"; shift 2 ;;
		--prefix-offset) PREFIX_OFFSET=$(( $2 )); shift 2 ;;
		--shim-addr)     SHIM_ADDR="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,60p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
			exit 0
			;;
		-*)
			echo "Error: unknown option: $1" >&2
			exit 1
			;;
		*)
			POS_ARGS+=("$1")
			shift
			;;
	esac
done

# Apply the legacy positional arguments to slots not set via options
_i=0
for _arg in "${POS_ARGS[@]}"; do
	case $_i in
		0) [ "$_payload_set"  -eq 0 ] && { PAYLOAD="$_arg"; _payload_set=1; } ;;
		1) [ "$_output_set"   -eq 0 ] && { OUTPUT_DIR="$_arg"; _output_set=1; } ;;
		2) [ "$_mkimage_set"  -eq 0 ] && { MKIMAGE="$_arg"; _mkimage_set=1; } ;;
		3) [ "$_dumpimage_set" -eq 0 ] && { DUMPIMAGE="$_arg"; _dumpimage_set=1; } ;;
		4) [ "$_board_set"    -eq 0 ] && { BOARD="$_arg"; _board_set=1; } ;;
	esac
	_i=$((_i + 1))
done

if [ -z "$BOARD" ]; then
	echo "Error: no board specified (use --board or the 5th positional arg)" >&2
	exit 1
fi

BOARD_DIR="$CHAINLOADER_DIR/${SOC}_${BOARD}"
SHIM_BIN="$BOARD_DIR/shim.bin"
CONTROL_DTS="$BOARD_DIR/control.dts"
ITS_TEMPLATE="$BOARD_DIR/chainloader.its"

# ---- input checks ----
if [ ! -d "$BOARD_DIR" ]; then
	echo "Error: chainloader sources not found for board '$BOARD': $BOARD_DIR" >&2
	exit 1
fi
for f in "$SHIM_BIN" "$CONTROL_DTS" "$ITS_TEMPLATE"; do
	if [ ! -f "$f" ]; then
		echo "Error: missing chainloader source for board '$BOARD': $f" >&2
		exit 1
	fi
done
if [ ! -f "$PAYLOAD" ]; then
	echo "Error: payload not found: $PAYLOAD" >&2
	exit 1
fi
if [ ! -f "$MKIMAGE" ]; then
	echo "Error: mkimage not found: $MKIMAGE (build u-boot.bin first)" >&2
	exit 1
fi
if [ ! -x "$DTC" ]; then
	echo "Error: dtc not found: $DTC (build u-boot.bin first)" >&2
	exit 1
fi

# ---- LZMA encoder detection ----
if [ -z "$LZMA_CMD" ]; then
	if command -v lzma >/dev/null 2>&1; then
		LZMA_CMD="lzma -9 -c"
	elif command -v xz >/dev/null 2>&1; then
		LZMA_CMD="xz --format=lzma --stdout"
	else
		echo "Error: no LZMA encoder found (install lzma or xz)" >&2
		exit 1
	fi
fi

# ---- normalize paths (after existence checks) ----
PAYLOAD="$(realpath "$PAYLOAD")"
MKIMAGE="$(realpath "$MKIMAGE")"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"
if [ -f "$DUMPIMAGE" ]; then
	DUMPIMAGE="$(realpath "$DUMPIMAGE")"
fi
if [ -f "$DTC" ]; then
	DTC="$(realpath "$DTC")"
fi

mkdir -p "$OUTPUT_DIR"

BOARD_UPPER="$(printf '%s' "$BOARD" | tr '[:lower:]' '[:upper:]')"

OUTPUT_PREFIX_SHIM="$OUTPUT_DIR/${BOARD}-chainloader-prefix-shim.uImage"
OUTPUT_CONTROL_DTB="$OUTPUT_DIR/${BOARD}-chainloader-control.dtb"
OUTPUT_FIT="$OUTPUT_DIR/${BOARD}-chainloader.itb"
OUTPUT_SLOT="$OUTPUT_DIR/${BOARD}-chainloader-slot.bin"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# ---- 1. shim.bin -> shim.lzma -> prefix shim legacy uImage ----
echo "  [CHAINLOADER] Compressing shim ($SOC/$BOARD, LZMA)..."
# shellcheck disable=SC2086
$LZMA_CMD "$SHIM_BIN" > "$TMPDIR/shim.lzma"

echo "  [CHAINLOADER] Building prefix shim legacy uImage..."
"$MKIMAGE" -A arm64 -O linux -C none -a "$SHIM_ADDR" -e "$SHIM_ADDR" \
	-n "$BOARD_UPPER chainloader prefix shim" \
	-d "$TMPDIR/shim.lzma" "$TMPDIR/prefix-shim.uImage"
# Patch ih_os (byte 0x1C of the legacy image header) to U-Boot (0x03) so the
# Airoha loader treats the prefix as a U-Boot legacy image.
printf "\\x$(printf '%02x' "$IH_OS_U_BOOT")" | dd of="$TMPDIR/prefix-shim.uImage" \
	bs=1 seek=$((0x1C)) count=1 conv=notrunc 2>/dev/null
mv "$TMPDIR/prefix-shim.uImage" "$OUTPUT_PREFIX_SHIM"

# ---- 2. control.dts -> control.dtb ----
echo "  [CHAINLOADER] Compiling control DTB..."
"$DTC" -I dts -O dtb -o "$OUTPUT_CONTROL_DTB" "$CONTROL_DTS"

# ---- 3. prepare ITS (sed) and build the chainloader FIT ----
echo "  [CHAINLOADER] Preparing chainloader FIT source..."
sed \
	-e "s|__DTB__|$OUTPUT_CONTROL_DTB|g" \
	-e "s|__SHIM__|$SHIM_BIN|g" \
	-e "s|__PAYLOAD__|$PAYLOAD|g" \
	-e "s|__KCOMP__|none|g" \
	"$ITS_TEMPLATE" > "$TMPDIR/${BOARD}-chainloader.its"

echo "  [CHAINLOADER] Building chainloader FIT image..."
"$MKIMAGE" -f "$TMPDIR/${BOARD}-chainloader.its" "$OUTPUT_FIT"
if [ -x "$DUMPIMAGE" ]; then
	"$DUMPIMAGE" -l "$OUTPUT_FIT" 2>/dev/null || true
fi

# ---- 4. slot image: prefix shim [0 .. 0x2100) + FIT [0x2100 .. end) ----
echo "  [CHAINLOADER] Building chainloader slot image..."
dd if="$OUTPUT_PREFIX_SHIM" of="$OUTPUT_SLOT" bs=1 count=$PREFIX_OFFSET conv=notrunc 2>/dev/null
dd if="$OUTPUT_FIT" of="$OUTPUT_SLOT" bs=1 seek=$PREFIX_OFFSET conv=notrunc 2>/dev/null

# ---- summary ----
echo ""
echo "Done! chainloader images for '$BOARD' (SoC: $SOC):"
echo "  Prefix shim: $OUTPUT_PREFIX_SHIM ($(wc -c < "$OUTPUT_PREFIX_SHIM") bytes)"
echo "  Control DTB: $OUTPUT_CONTROL_DTB ($(wc -c < "$OUTPUT_CONTROL_DTB") bytes)"
echo "  FIT:         $OUTPUT_FIT ($(wc -c < "$OUTPUT_FIT") bytes)"
echo "  Slot:        $OUTPUT_SLOT ($(wc -c < "$OUTPUT_SLOT") bytes)"
echo ""
echo "Magic check:"
echo "  Offset 0x0000: $(dd if="$OUTPUT_SLOT" bs=1 count=4 2>/dev/null | od -A n -t x1 | tr -d ' \n')"
echo "  Offset $PREFIX_OFFSET: $(dd if="$OUTPUT_SLOT" bs=1 skip=$PREFIX_OFFSET count=4 2>/dev/null | od -A n -t x1 | tr -d ' \n')"
