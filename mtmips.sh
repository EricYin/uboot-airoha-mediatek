#!/bin/sh
# ============================================================================
# mtmips.sh - Build U-Boot for MediaTek MTMIPS platform
#             (MT7620 / MT7621 / MT7628 & MT7688)
#
# Usage:
#   SOC=<mt7620|mt7621|mt7628|mt7688> BOARD=<board_name> ./mtmips.sh
#
# Examples:
#   SOC=mt7620 BOARD=rfb                ./mtmips.sh
#   SOC=mt7621 BOARD=rfb                ./mtmips.sh
#   SOC=mt7621 BOARD=nand_rfb           ./mtmips.sh
#   SOC=mt7628 BOARD=rfb			    ./mtmips.sh
#   SOC=mt7688 BOARD=linkit-smart  		./mtmips.sh
#
# Note: MT7628 and MT7688 share the same toolchain (ramips/mt76x8)
#       and use "mt7628_" as the defconfig prefix.
#       Output directory/file use the actual SOC name passed by user.
#
# Environment variables:
#   SOC           Target SoC (required: mt7620, mt7621, mt7628, or mt7688)
#   BOARD         Target board name (required)
#   TOOLCHAIN     Cross-compiler prefix (auto-detected if empty)
#   AUTO_DL       Set to 1 to auto-download the toolchain without prompting
#                 when no local copy is found (also via --auto-download / -d)
#   FORCE_DL      Set to 1 to force re-download the toolchain even if a local
#                 one is found (also via --force-download / -f)
#   JOBS          Parallel make jobs (default: nproc)
#   STAGING_DIR   Staging directory (passed to make)
#
# Output:
#   output_<soc>/<soc>-u-boot-<board>.bin
#
# Note: Toolchain should be placed in the parent directory (sibling of u-boot),
#       NOT inside the u-boot source tree. This avoids LTO plugin issues that
#       occur when the toolchain is a subdirectory of the build tree.
# ============================================================================

set -e

# ---------------------------------------------------------------------------
# Defaults for command-line / env options
# ---------------------------------------------------------------------------
AUTO_DL=${AUTO_DL:-0}
FORCE_DL=${FORCE_DL:-0}

# ---------------------------------------------------------------------------
# --help / -h
# ---------------------------------------------------------------------------
show_help() {
	cat <<'EOF'
Usage: SOC=<mt7620|mt7621|mt7628|mt7688> BOARD=<board_name> [OPTIONS] ./mtmips.sh

Build U-Boot for MediaTek MTMIPS (MT7620 / MT7621 / MT7628 & MT7688) platform.

MT7628 and MT7688 share the same defconfig prefix ("mt7628_"); the output
name uses the actual SOC name.  All four SoCs are built with one OpenWrt
ramips (mipsel, 24kc) toolchain: any ../openwrt*ramips*/ already present is
reused, the mt7621 one is downloaded when there is none.

Required:
  SOC=<mt7620|mt7621|mt7628|mt7688>   Target SoC
  BOARD=<board>                       Target board name

Options (environment or command line):
  --auto-download,-d   If no local toolchain is found, download it
                       automatically without prompting (env: AUTO_DL=1)
  --force-download,-f  Force (re-)download the toolchain even if a local
                       copy is found (env: FORCE_DL=1)
  TOOLCHAIN=...         Cross-compiler prefix (auto-detected from
                        ../openwrt*/toolchain-mipsel*)
  JOBS=<n>              Parallel make jobs (default: nproc)
  STAGING_DIR=...       Staging directory (auto-detected from TOOLCHAIN)
  STAGE_SRAM_SRC=...    Local path to mt7621_stage_sram.bin (mt7621 only;
                        downloaded from upstream if unset and absent)

Examples:
  SOC=mt7620 BOARD=rfb                ./mtmips.sh
  SOC=mt7621 BOARD=rfb                ./mtmips.sh
  SOC=mt7621 BOARD=nand_rfb           ./mtmips.sh
  SOC=mt7628 BOARD=rfb                ./mtmips.sh
  SOC=mt7688 BOARD=linkit-smart       ./mtmips.sh
  SOC=mt7621 BOARD=rfb -d             ./mtmips.sh   # auto-download if absent
  SOC=mt7621 BOARD=rfb -f             ./mtmips.sh   # force re-download
EOF
}

# ---------------------------------------------------------------------------
# Parse command-line arguments (env vars can also be used)
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--help|-h|help)
			show_help
			exit 0
			;;
		--auto-download|-d)
			AUTO_DL=1
			;;
		--force-download|-f)
			FORCE_DL=1
			;;
		--)
			shift
			break
			;;
		*)
			echo "Unknown option: $1"
			echo "Try '$0 --help' for more information."
			exit 1
			;;
	esac
	shift
done

# ---------------------------------------------------------------------------
# Validate SOC
# ---------------------------------------------------------------------------
if [ -z "$SOC" ]; then
	echo "Usage: SOC=<mt7620|mt7621|mt7628|mt7688> BOARD=<board_name> $0"
	echo "Try '$0 --help' for more information."
	exit 1
fi

case "$SOC" in
	mt7620)
		SOC_ID="mt7620"
		SOC_DEFCONFIG="mt7620"
		;;
	mt7621)
		SOC_ID="mt7621"
		SOC_DEFCONFIG="mt7621"
		;;
	mt7628)
		SOC_ID="mt7628"
		SOC_DEFCONFIG="mt7628"
		;;
	mt7688)
		SOC_ID="mt7688"
		SOC_DEFCONFIG="mt7688"
		;;
	*)
		echo "Error: Unsupported SOC='$SOC'. Valid values: mt7620, mt7621, mt7628, mt7688"
		exit 1
		;;
esac

# ---------------------------------------------------------------------------
# Toolchain: one for the whole ramips family
#
# OpenWrt builds all three ramips subtargets (mt7620, mt7621, mt76x8) with the
# same CPU type (24kc), the same GCC and musl, and the same "mipsel-openwrt-linux-"
# prefix (toolchain-mipsel_24kc_gcc-*), so mt7620/mt7621/mt7628/mt7688 are all
# served by one toolchain - U-Boot supplies its own -march=mips32r2 -mtune=24kc
# from arch/mips/Makefile, not the toolchain defaults.  Any ramips toolchain
# already unpacked in the parent directory is reused, whichever subtarget it
# came from; the mt7621 one is downloaded when none is present.
# ---------------------------------------------------------------------------
TOOLCHAIN_SUBPATH="ramips/mt7621"
TOOLCHAIN_PATTERN="openwrt*ramips*"

if [ -z "$BOARD" ]; then
	echo "Usage: SOC=${SOC} BOARD=<board_name> $0"
	echo "Try '$0 --help' for more information."
	exit 1
fi

UBOOT_DIR=.
OUTPUT_DIR="output_mtmips"

die()
{
	echo "Error: $*"
	exit 1
}

# Read a CONFIG_ value from the generated .config
get_config()
{
	grep -oP "^CONFIG_$1=\K.*" "$UBOOT_DIR/.config" 2>/dev/null || true
}

# URL of the prebuilt OpenWrt toolchain (auto-built from SOC)
TOOLCHAIN_URL_NAME=$(echo "$TOOLCHAIN_SUBPATH" | tr '/' '-')
TOOLCHAIN_URL="${TOOLCHAIN_URL:-https://downloads.openwrt.org/releases/25.12.5/targets/${TOOLCHAIN_SUBPATH}/openwrt-toolchain-25.12.5-${TOOLCHAIN_URL_NAME}_gcc-14.3.0_musl.Linux-x86_64.tar.zst}"

# ---------------------------------------------------------------------------
# Auto-detect or download toolchain (look in parent directory, not U-Boot tree)
# ---------------------------------------------------------------------------
PARENT_DIR="$(cd "$UBOOT_DIR/.."; pwd)"

find_toolchain() {
	TOOLCHAIN_BIN=""
	for dir in $PARENT_DIR/$TOOLCHAIN_PATTERN/toolchain-mipsel*/bin; do
		if [ -d "$dir" ]; then
			TOOLCHAIN_BIN=$(cd "$dir" && pwd)
			return 0
		fi
	done
	return 1
}

download_toolchain() {
	echo "Downloading toolchain from: $TOOLCHAIN_URL"
	cd "$PARENT_DIR" || return 1
	if command -v wget >/dev/null 2>&1; then
		wget -O - "$TOOLCHAIN_URL" | tar --zstd -xf - || return 1
	elif command -v curl >/dev/null 2>&1; then
		curl -L "$TOOLCHAIN_URL" | tar --zstd -xf - || return 1
	else
		echo "Neither wget nor curl found. Install one or download manually: $TOOLCHAIN_URL"
		return 1
	fi
	cd "$UBOOT_DIR" || return 1
}

if [ -z "$TOOLCHAIN" ]; then
	if [ "$FORCE_DL" = "1" ]; then
		echo "FORCE_DL set: (re-)downloading toolchain from: $TOOLCHAIN_URL"
		download_toolchain || die "Toolchain download failed."
		find_toolchain || die "Toolchain not found after extraction."
	elif ! find_toolchain; then
		if [ "$AUTO_DL" = "1" ]; then
			echo "AUTO_DL set: downloading toolchain from: $TOOLCHAIN_URL"
			download_toolchain || die "Toolchain download failed."
			find_toolchain || die "Toolchain not found after extraction."
		else
			echo "Toolchain not found in parent directory ($PARENT_DIR)."
			read -p "Download it now? [Y/n] " dlcc
			dlcc=${dlcc:-Y}
			case "$dlcc" in
				[Yy]* )
					download_toolchain || die "Toolchain download failed."
					find_toolchain || die "Toolchain not found after extraction."
					;;
				* )
					die "Toolchain required. Set TOOLCHAIN=... or place ${TOOLCHAIN_PATTERN}/toolchain-mipsel*/ in $PARENT_DIR."
					;;
			esac
		fi
	fi
	TOOLCHAIN="${TOOLCHAIN_BIN}/mipsel-openwrt-linux-"
fi

if [ -z "$STAGING_DIR" ]; then
	STAGING_DIR="${TOOLCHAIN_BIN%/bin}"
fi

# ---------------------------------------------------------------------------
# Build config
# ---------------------------------------------------------------------------
UBOOT_CFG="${SOC_DEFCONFIG}_${BOARD}_defconfig"

# ---------------------------------------------------------------------------
# Environment checks
# ---------------------------------------------------------------------------
echo "======================================================================"
echo "MTMIPS U-Boot Build (SOC=${SOC}, family=${SOC_ID})"
echo "======================================================================"

command -v python3 >/dev/null 2>&1 || die "Python 3 is not installed."
command -v "${TOOLCHAIN}gcc" >/dev/null 2>&1 || die "${TOOLCHAIN}gcc not found!"

echo "SOC:           $SOC"
echo "BOARD:         $BOARD"
echo "Toolchain:     $TOOLCHAIN"
echo "STAGING_DIR:   $STAGING_DIR"
echo "Defconfig:     $UBOOT_CFG"

[ -f "$UBOOT_DIR/configs/$UBOOT_CFG" ] || die "Defconfig not found: $UBOOT_DIR/configs/$UBOOT_CFG"

# ---------------------------------------------------------------------------
# Parallel jobs
# ---------------------------------------------------------------------------
if [ -z "$JOBS" ]; then
	if command -v nproc >/dev/null 2>&1; then
		JOBS=$(nproc)
	else
		JOBS=1
	fi
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo "======================================================================"
echo "Building U-Boot..."
echo "======================================================================"

rm -f "$UBOOT_DIR/u-boot.bin" "$UBOOT_DIR/u-boot-with-spl.bin"
cp -f "$UBOOT_DIR/configs/$UBOOT_CFG" "$UBOOT_DIR/.config"
make -C "$UBOOT_DIR" olddefconfig
make -C "$UBOOT_DIR" clean

# ---------------------------------------------------------------------------
# mt7621 DDR init blob (mt7621_stage_sram.bin)
#
# The MT7621 binman image embeds a DDR initialization binary blob
# (arch/mips/dts/mt7621-u-boot.dtsi, type "blob-ext"). It must be present in
# the build (source) directory before the final build step, matching the
# documented flow:
#   $ cp mt7621_stage_sram.bin ./build/mt7621_stage_sram.bin
# ---------------------------------------------------------------------------
if [ "$SOC" = "mt7621" ]; then
	STAGE_SRAM="mt7621_stage_sram.bin"
	STAGE_SRAM_NOPRINT="mt7621_stage_sram_noprint.bin"
	BOARD_DIR="$UBOOT_DIR/board/mediatek/mt7621"
	STAGE_SRAM_URL="https://raw.githubusercontent.com/mtk-openwrt/mt7621-lowlevel-preloader/master/mt7621_stage_sram.bin"
	STAGE_SRAM_NOPRINT_URL="https://raw.githubusercontent.com/mtk-openwrt/mt7621-lowlevel-preloader/master/mt7621_stage_sram_noprint.bin"

	# Local cache dir: prefer the in-repo board directory (already ships the
	# blobs); fall back to the build dir when it is not writable.
	if [ -d "$BOARD_DIR" ] && [ -w "$BOARD_DIR" ]; then
		LOCAL_DIR="$BOARD_DIR"
	else
		LOCAL_DIR="$UBOOT_DIR"
	fi

	stage_sram_from_local() {
		# $1 = source path -> copy into build dir as $STAGE_SRAM
		cp -f "$1" "$UBOOT_DIR/$STAGE_SRAM" \
			|| die "Failed to copy $1 to $UBOOT_DIR/$STAGE_SRAM"
		echo "Stage SRAM blob copied from local: $1"
	}

	if [ -f "$UBOOT_DIR/$STAGE_SRAM" ]; then
		echo "Stage SRAM blob already present: $UBOOT_DIR/$STAGE_SRAM"
	elif [ -n "$STAGE_SRAM_SRC" ]; then
		[ -f "$STAGE_SRAM_SRC" ] || die "STAGE_SRAM_SRC='$STAGE_SRAM_SRC' not found."
		stage_sram_from_local "$STAGE_SRAM_SRC"
	elif [ -f "$BOARD_DIR/$STAGE_SRAM" ]; then
		stage_sram_from_local "$BOARD_DIR/$STAGE_SRAM"
	elif [ -f "$BOARD_DIR/$STAGE_SRAM_NOPRINT" ]; then
		stage_sram_from_local "$BOARD_DIR/$STAGE_SRAM_NOPRINT"
	else
		echo "mt7621 requires the DDR init blob $STAGE_SRAM for binman."
		echo "No local copy found; attempting to download from upstream..."
		stage_sram_download() {
			# $1 = url, $2 = local filename to cache
			if command -v wget >/dev/null 2>&1; then
				wget -O "$LOCAL_DIR/$2" "$1"
			elif command -v curl >/dev/null 2>&1; then
				curl -L -o "$LOCAL_DIR/$2" "$1"
			else
				return 2
			fi
		}
		DL_NAME=""
		if stage_sram_download "$STAGE_SRAM_URL" "$STAGE_SRAM"; then
			DL_NAME="$STAGE_SRAM"
			echo "Stage SRAM blob downloaded to local: $LOCAL_DIR/$STAGE_SRAM"
		elif stage_sram_download "$STAGE_SRAM_NOPRINT_URL" "$STAGE_SRAM_NOPRINT"; then
			DL_NAME="$STAGE_SRAM_NOPRINT"
			echo "Stage SRAM blob (noprint) downloaded to local: $LOCAL_DIR/$STAGE_SRAM_NOPRINT"
		else
			die "Download failed or no network tool. Place $STAGE_SRAM in $BOARD_DIR (or set STAGE_SRAM_SRC=<path>)."
		fi
		# Make the blob available in the build dir for binman.
		stage_sram_from_local "$LOCAL_DIR/$DL_NAME"
	fi
	[ -f "$UBOOT_DIR/$STAGE_SRAM" ] \
		|| die "$STAGE_SRAM not found in $UBOOT_DIR; required by binman for mt7621."
fi

make -C "$UBOOT_DIR" CROSS_COMPILE="${TOOLCHAIN}" STAGING_DIR="${STAGING_DIR}" -j "$JOBS" all

# Determine output image: respect CONFIG_BUILD_TARGET (e.g. u-boot-with-spl.bin for SPL builds)
UBOOT_BIN=$(get_config "BUILD_TARGET")
UBOOT_BIN=$(echo "$UBOOT_BIN" | tr -d '"')
[ -n "$UBOOT_BIN" ] || UBOOT_BIN="u-boot.bin"

[ -f "$UBOOT_DIR/$UBOOT_BIN" ] || die "U-Boot build failed! $UBOOT_BIN not generated."
echo "U-Boot build done! (image: $UBOOT_BIN)"

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
echo "======================================================================"
echo "Copying output files..."
echo "======================================================================"

mkdir -p "$OUTPUT_DIR"

MD5SUM=$(md5sum "$UBOOT_DIR/$UBOOT_BIN" | awk '{print $1}')
echo "$UBOOT_BIN md5: $MD5SUM"

UBOOTNAME="${SOC_ID}-u-boot-${BOARD}.bin"
cp -f "$UBOOT_DIR/$UBOOT_BIN" "$OUTPUT_DIR/$UBOOTNAME"

echo "${SOC_ID}-u-boot-${BOARD} build done"
echo "Output: $OUTPUT_DIR/$UBOOTNAME"
