#!/bin/bash
#===============================================================================
# mediatek.sh - MediaTek Filogic (MT798x) U-Boot + TF-A build script
#
# Usage: SOC=<soc> BOARD=<board> [OPTIONS] ./mediatek.sh
#
# A MediaTek Filogic firmware cannot be produced by this repository alone: the
# boot chain is split over two trees.
#
#   uboot-2026.10	  (this tree)   U-Boot          -> u-boot.bin       (BL33)
#   ../atf-mtksoc     (TF-A)        BL2  / BL31     -> bl2.img / fip.bin
#
# mediatek.sh builds U-Boot, hands u-boot.bin to the TF-A tree as BL33, runs
# the TF-A configuration that matches the board and collects the two artifacts
# a device needs:
#
#   <soc>_<board>-preloader.bin      <- TF-A bl2.img  (BL2, BROM image)
#   <soc>_<board>-bl31-uboot.fip     <- TF-A fip.bin  (BL31 + U-Boot)
#
# Boards that boot from eMMC/SD also need a partition table, which is flashed
# as a separate image.  Tools/ptgen builds those layouts; which one a board
# needs is recorded as GPT= in the board table:
#
#   <soc>_<board>-emmc-gpt.bin       <- tools/ptgen  (eMMC layout)
#   <soc>_<board>-sdmmc-gpt.bin      <- tools/ptgen  (SD layout)
#
# u-boot.bin is an intermediate of that chain, so it is not copied into the
# output directory unless COPY_UBOOT=1 asks for it.
#
# Which TF-A configuration belongs to which U-Boot board is table-driven and
# lives in mediatek-boards.cfg next to this script.
#
# Required:
#   SOC=<soc>       Target SoC: mt7981 | mt7986 | mt7987 | mt7988
#   BOARD=<board>   Board name, i.e. the U-Boot defconfig without the
#                   "${SOC}_" prefix and the "_defconfig" suffix.  Boards
#                   whose defconfig carries a longer SoC prefix are addressed
#                   by the board part alone (BOARD=bpir3_emmc builds
#                   mt7986a_bpir3_emmc_defconfig).
#
# Options:
#   STAGE=<stage>       all (default) | uboot | atf
#   ATF_DIR=<path>      TF-A source tree (default: ../atf-mtksoc)
#   BOARDS_FILE=<path>  Board table (default: ./mediatek-boards.cfg)
#   TOOLCHAIN=<prefix>  Cross-compiler prefix (default: aarch64-linux-gnu-)
#   JOBS=<n>            Parallel make jobs (default: nproc)
#   STAGING_DIR=<path>  Passed to U-Boot's make (default: empty)
#   FIP_COMPRESS=1      Compress the FIP payloads (XZ) in the TF-A build
#   DEBUG=1             Build TF-A into its debug tree (build/<plat>/debug)
#   COPY_UBOOT=1        Also collect the intermediate u-boot.bin (default: 0,
#                       only preloader.bin / bl31-uboot.fip are collected)
#   ROOTFS_PARTSIZE=<MB>
#                       Size of the production (rootfs) partition written into
#                       the GPT images, in MiB (default: 104)
#
# Toolchain auto-detection:
#   mt7981 / mt7986 / mt7987 / mt7988  ->  aarch64-linux-gnu-  (AArch64)
#
# The MediaTek Filogic SoCs are all AArch64. The older 32-bit ARMv7 MediaTek
# SoCs (MT7620 / MT7621 / MT7628 / MT7688) are built by mtmips.sh.
#
# Examples:
#   SOC=mt7981 BOARD=rfb-ubi		./mediatek.sh
#   SOC=mt7986 BOARD=rfb-ubi        ./mediatek.sh
#   SOC=mt7987 BOARD=emmc_rfb       ./mediatek.sh
#   SOC=mt7988 BOARD=rfb            ./mediatek.sh
#
#   # U-Boot only, no TF-A tree required:
#   SOC=mt7981 BOARD=rfb-ubi STAGE=uboot ./mediatek.sh
#
#   # Build only this tree's part and keep the TF-A tree out of it:
#   SOC=mt7981 BOARD=rfb ./mediatek.sh STAGE=uboot
#===============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()    { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; }
step()    { echo -e "\n${BLUE}=== $* ===${NC}"; }
die()     { error "$*"; exit 1; }

#------------------------------------------------------------------------------
# --help / -h: print usage extracted from the header comment block
#------------------------------------------------------------------------------
usage() {
	sed -n '2,/^[^#]/p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
	exit 0
}

case "${1:-}" in
	--help|-h|help)
		usage
		;;
esac

# ---------------------------------------------------------------------------
# Extract U-Boot version from Makefile
# ---------------------------------------------------------------------------
UBOOT_VERSION=$(awk -F'= ' '
    /^VERSION =/      {v=$2}
    /^PATCHLEVEL =/   {p=$2}
    /^SUBLEVEL =/     {s=$2}
    /^EXTRAVERSION =/ {e=$2}
    /^NAME =/         {n=$2}
    END {
        ver = v "." p
        if (s != "") ver = ver "." s
        ver = ver e "-" n
        print ver
    }' Makefile)

#------------------------------------------------------------------------------
# SOC and BOARD parameter check
#------------------------------------------------------------------------------
SOC="${SOC,,}" # Transform to lowercase

if [ -z "${SOC}" ] || [ -z "${BOARD}" ]; then
	error "SOC and BOARD environment variables must be specified."
	echo "Usage: SOC=<soc> BOARD=<board> [OPTIONS] $0"
	echo "Try '$0 --help' for more information."
	exit 1
fi

case "${SOC}" in
	mt7981|mt7986|mt7987|mt7988)
		DEFAULT_TOOLCHAIN="aarch64-linux-gnu-"
		;;
	*)
		error "Not supported SOC: ${SOC}"
		echo "Supported: mt7981, mt7986, mt7987, mt7988 (MediaTek Filogic, AArch64)"
		echo "The ARMv7 MediaTek SoCs (MT7620/MT7621/MT7628/MT7688) are built by mtmips.sh."
		exit 1
		;;
esac

SOC_UPPER="${SOC^^}"

#------------------------------------------------------------------------------
# Path and Toolchain Configuration
#------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="${SCRIPT_DIR}"
OUTPUT_DIR="${UBOOT_DIR}/output_mediatek"
TOOLCHAIN="${TOOLCHAIN:-${DEFAULT_TOOLCHAIN}}"

#------------------------------------------------------------------------------
# Defconfig resolution
#
# The obvious name is "${SOC}_${BOARD}_defconfig", but a few boards carry an
# extra SoC suffix (mt7986a_bpir3_emmc_defconfig), so fall back to a single
# "${SOC}*_${BOARD}_defconfig" match.  The resulting basename is also the key
# used to look the board up in mediatek-boards.cfg.
#------------------------------------------------------------------------------
defconfig_matches=()
if [ -f "${UBOOT_DIR}/configs/${SOC}_${BOARD}_defconfig" ]; then
	defconfig_matches+=("${SOC}_${BOARD}_defconfig")
else
	shopt -s nullglob
	for f in "${UBOOT_DIR}/configs/${SOC}"*_"${BOARD}"_defconfig; do
		defconfig_matches+=("${f##*/}")
	done
	shopt -u nullglob
fi

case "${#defconfig_matches[@]}" in
	0)
		error "Defconfig not found: ${UBOOT_DIR}/configs/${SOC}_${BOARD}_defconfig"
		echo "Configurations available for ${SOC}:"
		ls -1 "${UBOOT_DIR}/configs" 2>/dev/null | grep -E "^${SOC}.*_defconfig$" | sed 's/^/  /'
		exit 1
		;;
	1)
		DEFCONFIG="${defconfig_matches[0]}"
		;;
	*)
		error "Multiple defconfigs match SOC=${SOC} BOARD=${BOARD}:"
		printf '  %s\n' "${defconfig_matches[@]}"
		error "Please pick a unique BOARD name."
		exit 1
		;;
esac

DEFCONFIG_PATH="${UBOOT_DIR}/configs/${DEFCONFIG}"
BOARD_KEY="${DEFCONFIG%_defconfig}"
OUTPUT_PREFIX="${BOARD_KEY}"

ATF_DIR="${ATF_DIR:-${UBOOT_DIR}/../atf-mtksoc}"
BOARDS_FILE="${BOARDS_FILE:-${UBOOT_DIR}/mediatek-boards.cfg}"
STAGE="${STAGE:-all}"
COPY_UBOOT="${COPY_UBOOT:-0}"

# TF-A always runs on the same AArch64 toolchain as the Filogic U-Boot build.
ATF_TOOLCHAIN="${TOOLCHAIN}"

case "${STAGE}" in
	all|uboot|atf)
		;;
	*)
		error "Unsupported STAGE: ${STAGE} (use all, uboot or atf)"
		exit 1
		;;
esac

# Size of the production (rootfs) partition of the GPT images, in MiB.  The
# default matches OpenWrt's TARGET_ROOTFS_PARTSIZE; both "104" and "104M" are
# accepted.
ROOTFS_PARTSIZE="${ROOTFS_PARTSIZE:-104}"
ROOTFS_PARTSIZE="${ROOTFS_PARTSIZE%[Mm]}"
case "${ROOTFS_PARTSIZE}" in
	''|*[!0-9]*)
		error "ROOTFS_PARTSIZE must be a size in MiB, e.g. ROOTFS_PARTSIZE=104"
		exit 1
		;;
esac

if [ -z "${JOBS}" ]; then
	if command -v nproc &>/dev/null; then
		JOBS=$(nproc)
	else
		JOBS=1
	fi
fi

# Board table values, filled in by resolve_board()
BOARD_NAME=""
BL2_BOOTCFG=""
BL2_DRAM=""
ATF_CONFIG=""
GPT=""

# TF-A build description, filled in by resolve_atf_config()
ATF_PLAT=""
ATF_BUILD_PLAT=""

# Files actually copied by the current build; print_summary only lists these.
COPIED_FILES=()

#------------------------------------------------------------------------------
# Helpers
#------------------------------------------------------------------------------

# Insert an "_md5-<hash>" tag before the file extension so the failsafe web
# UI can verify an upload against the filename.  The tag format follows the
# rule enforced by failsafe/embedded/fsdata-bootstrap/main.js:
#     /(?:^|[._-])md5-([0-9a-fA-F]{32})(?:$|[._-])/
# i.e. the tag is delimited by start/".", "_" or "-" on both sides.
insert_md5_into_name() {
	local path="$1" md5="$2"
	local fullbase="${path##*/}"
	local dir="${path%/*}"
	local name="${fullbase%.*}"
	if [ "${name}" = "${fullbase}" ]; then
		# No extension: tag is simply appended to the basename.
		echo "${dir:+$dir/}${fullbase}_md5-${md5}"
	else
		local ext="${fullbase##*.}"
		echo "${dir:+$dir/}${name}_md5-${md5}.${ext}"
	fi
}

copy_with_md5() {
	local file=$1
	local dest=$2
	local label=$3
	local md5 md5_dest
	md5=$(md5sum "$file" | awk '{print $1}')
	md5_dest=$(insert_md5_into_name "$dest" "$md5")
	cp -f "$file" "$md5_dest"
	COPIED_FILES+=("${md5_dest}")
	info "${label} (md5: ${md5}) -> ${md5_dest}"
}

config_enabled() {
	grep -q "^$1=y$" "${UBOOT_DIR}/.config"
}

# Read one field of one block from the board table.
#   board_field <block name> <field name>
board_field() {
	awk -v key="$1" -v field="$2" '
		/^[ \t]*#/ { next }
		$1 == "define" { cur = $2; next }
		$1 == "endef"  { cur = ""; next }
		cur == key {
			line = $0
			sub(/^[ \t]+/, "", line)
			if (line ~ ("^" field "[ \t]*:?=")) {
				sub(("^" field "[ \t]*:?=[ \t]*"), "", line)
				sub(/[ \t]+$/, "", line)
				sub(/^"/, "", line)
				sub(/"$/, "", line)
				print line
				exit
			}
		}
	' "$BOARDS_FILE"
}

# List the board table blocks belonging to one SoC.
board_list() {
	awk -v soc="$1" '
		$1 == "define" && $2 ~ ("^" soc "_") { print $2 }
	' "$BOARDS_FILE"
}

# Read a make variable out of the TF-A build/.config (quotes stripped).
atf_config_value() {
	local key="$1" val
	val=$(sed -n "s/^${key}=//p" "${ATF_DIR}/build/.config" 2>/dev/null | head -1)
	val="${val%\"}"
	val="${val#\"}"
	echo "${val}"
}

#------------------------------------------------------------------------------
# Board table lookup
#------------------------------------------------------------------------------
resolve_board() {
	step "Resolve Board [${BOARD_KEY}]"

	[ -f "${BOARDS_FILE}" ] || \
		die "Board table not found: ${BOARDS_FILE}"

	grep -qE "^[ \t]*define[ \t]+${BOARD_KEY}[ \t]*$" "${BOARDS_FILE}" || {
		error "No board table entry for '${BOARD_KEY}' in ${BOARDS_FILE}"
		local known
		known=$(board_list "${SOC}")
		if [ -n "${known}" ]; then
			echo "Known ${SOC} boards:"
			echo "${known}" | sed 's/^/  /'
		else
			echo "No ${SOC} board is recorded in the table yet."
		fi
		echo "Add a 'define ${BOARD_KEY}' block to ${BOARDS_FILE} first."
		exit 1
	}

	BOARD_NAME="$(board_field "${BOARD_KEY}" NAME)"
	BL2_BOOTCFG="$(board_field "${BOARD_KEY}" BL2_BOOTCFG)"
	BL2_DRAM="$(board_field "${BOARD_KEY}" BL2_DRAM)"
	ATF_CONFIG="$(board_field "${BOARD_KEY}" ATF_CONFIG)"
	GPT="$(board_field "${BOARD_KEY}" GPT)"

	local layout
	for layout in ${GPT}; do
		case "${layout}" in
			emmc|sdmmc)
				;;
			*)
				error "Invalid GPT layout '${layout}' for '${BOARD_KEY}' in ${BOARDS_FILE}"
				echo "Supported: emmc, sdmmc (space-separated for several)."
				exit 1
				;;
		esac
	done

	if [ -z "${ATF_CONFIG}" ]; then
		if [ -z "${BL2_BOOTCFG}" ]; then
			if [ "${STAGE}" = "uboot" ]; then
				warn "No TF-A counterpart recorded for '${BOARD_KEY}' (U-Boot only)."
			else
				error "No TF-A counterpart recorded for '${BOARD_KEY}'."
				echo "TF-A does not ship a matching configuration for this board yet,"
				echo "so STAGE=${STAGE} cannot be completed. Build U-Boot only with:"
				echo "  SOC=${SOC} BOARD=${BOARD} STAGE=uboot $0"
				exit 1
			fi
		else
			local bootcfg="${BL2_BOOTCFG//-/_}"
			if [ -n "${BL2_DRAM}" ]; then
				ATF_CONFIG="${SOC}_${bootcfg}_${BL2_DRAM//-/_}_defconfig"
			else
				ATF_CONFIG="${SOC}_${bootcfg}_defconfig"
			fi
		fi
	fi

	if [ -n "${ATF_CONFIG}" ]; then
		info "Board: ${BOARD_NAME:-${BOARD}}"
		info "TF-A config: ${ATF_CONFIG}"
	fi
}

#------------------------------------------------------------------------------
# TF-A configuration resolution
#
# Checks that the defconfig the board table picked actually exists in the TF-A
# tree.  The output path (build/<plat>/<build type>/) is only known once the
# defconfig has been applied, so it is resolved inside build_atf().
#------------------------------------------------------------------------------
resolve_atf_config() {
	if [ -z "${ATF_CONFIG}" ]; then
		# The board table has no TF-A counterpart (U-Boot only build).
		return 0
	fi

	local atf_cfg_path="${ATF_DIR}/configs/${ATF_CONFIG}"
	if [ ! -f "${atf_cfg_path}" ]; then
		error "TF-A config not found: ${atf_cfg_path}"
		echo "The board table entry for '${BOARD_KEY}' resolves to '${ATF_CONFIG}'."
		echo "Configurations available for ${SOC}:"
		ls -1 "${ATF_DIR}/configs" 2>/dev/null | grep -E "^${SOC}_.*_defconfig$" | sed 's/^/  /'
		echo "Fix BL2_BOOTCFG/BL2_DRAM (or set ATF_CONFIG) in ${BOARDS_FILE}."
		exit 1
	fi
	info "TF-A config: ${atf_cfg_path}"
}

#------------------------------------------------------------------------------
# Ensure failsafe JS dependencies
#------------------------------------------------------------------------------
ensure_failsafe_js_deps() {
	local failsafe_dir="${UBOOT_DIR}/failsafe"
	local embed_dir="${failsafe_dir}/embedded"
	local package_json="${embed_dir}/package.json"
	local marker="${embed_dir}/.npm-install-done"

	if [ ! -f "${package_json}" ]; then
		info "Skipping failsafe JS dependency setup: ${package_json} not found."
		return 0
	fi

	if [ -f "${marker}" ] && [ -d "${embed_dir}/node_modules/terser" ] && [ -d "${embed_dir}/node_modules/clean-css" ] && [ -d "${embed_dir}/node_modules/html-minifier-terser" ]; then
		info "Failsafe JS build dependencies already installed."
		return 0
	fi

	command -v npm &>/dev/null || { error "npm is not installed on this system."; exit 1; }
	info "Installing failsafe JS build dependencies..."
	( cd "${embed_dir}" && npm install --no-audit --no-fund ) || exit 1
	touch "${marker}"
	info "Failsafe JS build dependencies installed."
}

#------------------------------------------------------------------------------
# Environment Check
#------------------------------------------------------------------------------
check_environment() {
	step "Environment Check [SOC: ${SOC_UPPER}] [BOARD: ${BOARD}]"

	# --- npm ---
	if ! command -v npm &>/dev/null; then
		error "npm is not installed on this system."
		exit 1
	fi
	info "npm: $(npm --version 2>&1)"

	info "Checking failsafe JS dependencies..."
	ensure_failsafe_js_deps

	# --- Python 3 ---
	if ! command -v python3 &>/dev/null; then
		error "Python 3 is not installed."
		error "Please install: sudo apt install -y python3"
		exit 1
	fi
	info "Python3: $(python3 --version 2>&1)"

	# --- Cross Toolchain ---
	if ! command -v "${TOOLCHAIN}gcc" &>/dev/null; then
		error "Cross toolchain not found: ${TOOLCHAIN}gcc"
		error "Please install the appropriate toolchain or set TOOLCHAIN=<prefix>"
		exit 1
	fi
	info "Toolchain: $(${TOOLCHAIN}gcc --version | head -1)"

	# --- Defconfig ---
	[ -f "${DEFCONFIG_PATH}" ] || die "Defconfig not found: ${DEFCONFIG_PATH}"
	info "Defconfig: ${DEFCONFIG}"

	# --- TF-A tree (only when it is going to be used) ---
	if [ "${STAGE}" != "uboot" ] && [ -n "${ATF_CONFIG}" ]; then
		if [ ! -d "${ATF_DIR}" ]; then
			error "TF-A tree not found: ${ATF_DIR}"
			error "Clone it next to this tree, or set ATF_DIR=<path>."
			exit 1
		fi
		if [ ! -f "${ATF_DIR}/Makefile" ]; then
			error "${ATF_DIR}/Makefile not found: is ${ATF_DIR} a TF-A tree?"
			exit 1
		fi
		info "TF-A tree: ${ATF_DIR}"

		# TF-A's Kconfig front-end needs a 'python' (not just python3), and
		# the device tree compiler is used for the embedded BL2 DT.
		if ! command -v python &>/dev/null; then
			error "'python' is not available; TF-A's Kconfig front-end requires it."
			error "Please install: sudo apt install -y python-is-python3"
			exit 1
		fi
		if ! command -v dtc &>/dev/null; then
			error "'dtc' is not available; install device-tree-compiler:"
			error "  sudo apt install -y device-tree-compiler"
			exit 1
		fi
	fi

	mkdir -p "${OUTPUT_DIR}"
	info "Environment Check passed"
}

#------------------------------------------------------------------------------
# Configure U-Boot
#------------------------------------------------------------------------------
configure_uboot() {
	step "Configure U-Boot"

	cd "${UBOOT_DIR}"

	cp -f "${DEFCONFIG_PATH}" "${UBOOT_DIR}/.config"
	make olddefconfig

	info "U-Boot configured"
}

#------------------------------------------------------------------------------
# Detect Build Features
#------------------------------------------------------------------------------
detect_build_features() {
	step "Detect Build Features"

	SOC_FAMILY=$(sed -n 's/^CONFIG_SYS_SOC="\(.*\)"$/\1/p' "${UBOOT_DIR}/.config")
	[ -n "${SOC_FAMILY}" ] || SOC_FAMILY="${SOC}"
	UBOOT_DTB=$(sed -n 's/^CONFIG_DEFAULT_DEVICE_TREE="\(.*\)"$/\1/p' "${UBOOT_DIR}/.config")
	UBOOT_BUILD_TARGET=$(sed -n 's/^CONFIG_BUILD_TARGET="\(.*\)"$/\1/p' "${UBOOT_DIR}/.config")
	[ -n "${UBOOT_BUILD_TARGET}" ] || UBOOT_BUILD_TARGET="u-boot.bin"

	if config_enabled CONFIG_MTD_SPI_NAND; then
		UBOOT_SPI_NAND="y"
	else
		UBOOT_SPI_NAND="n"
	fi

	if config_enabled CONFIG_MMC; then
		UBOOT_MMC="y"
	else
		UBOOT_MMC="n"
	fi

	if config_enabled CONFIG_MTD_UBI || config_enabled CONFIG_ENV_IS_IN_UBI; then
		UBOOT_LAYOUT="UBI"
	else
		UBOOT_LAYOUT="raw"
	fi

	echo "SOC:                  ${SOC}"
	echo "SOC family:           ${SOC_FAMILY}"
	echo "BOARD:                ${BOARD}"
	echo "Board name:           ${BOARD_NAME:-${BOARD}}"
	echo "Defconfig:            ${DEFCONFIG}"
	echo "Device tree:          ${UBOOT_DTB}"
	echo "U-Boot image:         ${UBOOT_BUILD_TARGET}"
	echo "Toolchain:            ${TOOLCHAIN}"
	echo "Features:             spi-nand: ${UBOOT_SPI_NAND}, mmc: ${UBOOT_MMC}, layout: ${UBOOT_LAYOUT}"
	echo "GPT layouts:          ${GPT:-<none>}"
	if [ -n "${GPT}" ]; then
		echo "Rootfs partition:     ${ROOTFS_PARTSIZE} MiB"
	fi
	echo "Stage:                ${STAGE}"
	if [ -n "${ATF_CONFIG}" ]; then
		echo "TF-A dir:             ${ATF_DIR}"
		echo "TF-A config:          ${ATF_CONFIG}"
		echo "TF-A boot cfg:        ${BL2_BOOTCFG:-<unset>}"
		echo "TF-A DRAM:            ${BL2_DRAM:-<auto>}"
	fi
}

#------------------------------------------------------------------------------
# Build U-Boot
#------------------------------------------------------------------------------
build_uboot() {
	step "Build U-Boot [${SOC_UPPER}]"

	cd "${UBOOT_DIR}"

	rm -f "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}"

	make clean
	make CROSS_COMPILE="${TOOLCHAIN}" STAGING_DIR="${STAGING_DIR:-}" -j "${JOBS}" all

	if [ ! -f "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" ]; then
		error "U-Boot build failed: ${UBOOT_BUILD_TARGET} not generated"
		exit 1
	fi
	info "U-Boot build done: $(stat -c%s "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}") bytes"
}

#------------------------------------------------------------------------------
# Build TF-A
#
# BL33 is handed over as an absolute path so the TF-A tree does not need a
# copy of u-boot.bin, and so a stale copy can never be picked up.
#------------------------------------------------------------------------------
build_atf() {
	step "Build TF-A [${SOC_UPPER}] [${ATF_CONFIG}]"

	cd "${ATF_DIR}"

	rm -rf "${ATF_DIR}/build"

	info "Applying TF-A config: ${ATF_CONFIG}"
	make "${ATF_CONFIG}"

	# Re-resolve now that build/.config exists: the PLAT value baked into it
	# decides the output directory (build/<plat>/<build type>/).  The build
	# type mirrors TF-A's own DEBUG switch (defaults.mk): DEBUG != 0 selects
	# the debug tree, anything else release.
	ATF_PLAT="${SOC}"
	local plat debug build_type
	plat=$(atf_config_value PLAT)
	[ -n "${plat}" ] && ATF_PLAT="${plat}"
	debug="${DEBUG:-0}"
	if [ "${debug}" != "0" ]; then
		build_type="debug"
	else
		build_type="release"
	fi
	ATF_BUILD_PLAT="${ATF_DIR}/build/${ATF_PLAT}/${build_type}"
	info "TF-A output: ${ATF_BUILD_PLAT}"

	# FIP compression is a build option rather than a config one in some
	# revisions; honour FIP_COMPRESS=1 the same way the TF-A tree does.
	if [ "${FIP_COMPRESS:-0}" = "1" ]; then
		info "Enabling FIP compression (XZ)"
		sed -i 's/^# _ENABLE_FIP_COMPRESS is not set/_ENABLE_FIP_COMPRESS=y/' \
			"${ATF_DIR}/build/.config"
		grep -q '^FIP_COMPRESS=1$' "${ATF_DIR}/build/.config" || \
			echo 'FIP_COMPRESS=1' >> "${ATF_DIR}/build/.config"
	fi

	make CROSS_COMPILE="${ATF_TOOLCHAIN}" \
	     BL33="${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" \
	     -j "${JOBS}"

	# Should the computed build type be wrong, fall back to whichever tree
	# actually holds the artifacts rather than failing on a path guess.
	local d
	for d in "${ATF_DIR}/build/${ATF_PLAT}/release" "${ATF_DIR}/build/${ATF_PLAT}/debug"; do
		if [ -f "${d}/fip.bin" ] || [ -f "${d}/bl2.img" ]; then
			if [ "${d}" != "${ATF_BUILD_PLAT}" ]; then
				warn "TF-A artifacts found in ${d}, using it as the output directory"
				ATF_BUILD_PLAT="${d}"
			fi
			break
		fi
	done

	if [ ! -f "${ATF_BUILD_PLAT}/bl2.img" ] && [ ! -f "${ATF_BUILD_PLAT}/bl2.bin" ]; then
		error "TF-A build failed: neither bl2.img nor bl2.bin was generated"
		exit 1
	fi
	if [ ! -f "${ATF_BUILD_PLAT}/fip.bin" ]; then
		error "TF-A build failed: fip.bin was not generated"
		exit 1
	fi
	info "TF-A build done"
}

#------------------------------------------------------------------------------
# Copy Output Files
#
#   preloader.bin    the BL2 BROM image, built for the board's boot device;
#                    bl2.img is the plain image, bl2.bin the RAM-boot one.
#   bl31-uboot.fip   BL31 + U-Boot, the image the bootloader chain loads.
#------------------------------------------------------------------------------
copy_outputs() {
	step "Copy Output Files"

	cd "${UBOOT_DIR}"
	mkdir -p "${OUTPUT_DIR}"

	# Drop this board's artifacts from a previous build: the md5 tag makes
	# every rebuild a new filename, so without this the directory would keep
	# growing and it would not be obvious which pair is the current one.
	local old
	for old in "${OUTPUT_DIR}/${OUTPUT_PREFIX}-"*; do
		[ -f "${old}" ] || continue
		rm -f "${old}"
	done

	# U-Boot itself is an intermediate: the flashable artifacts are the ones
	# TF-A derives from it, so it is only collected on request.
	if [ "${COPY_UBOOT}" = "1" ]; then
		copy_with_md5 "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-${UBOOT_BUILD_TARGET}" \
			"${UBOOT_BUILD_TARGET}"
	else
		info "${UBOOT_BUILD_TARGET} left in place: ${UBOOT_DIR}/${UBOOT_BUILD_TARGET}"
		info "  (set COPY_UBOOT=1 to also collect it into ${OUTPUT_DIR}/)"
	fi

	if [ -z "${ATF_CONFIG}" ]; then
		return 0
	fi

	local bl2_img="${ATF_BUILD_PLAT}/bl2.img"
	if [ -f "${bl2_img}" ]; then
		copy_with_md5 "${bl2_img}" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-preloader.bin" \
			"preloader.bin (bl2.img)"
	else
		# RAM boot devices have no BROM header, so TF-A only emits bl2.bin.
		local bl2_bin="${ATF_BUILD_PLAT}/bl2.bin"
		[ -f "${bl2_bin}" ] || die "Neither bl2.img nor bl2.bin was generated."
		warn "bl2.img not generated (RAM boot build); using bl2.bin as preloader."
		copy_with_md5 "${bl2_bin}" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-preloader.bin" \
			"preloader.bin (bl2.bin)"
	fi

	copy_with_md5 "${ATF_BUILD_PLAT}/fip.bin" \
		"${OUTPUT_DIR}/${OUTPUT_PREFIX}-bl31-uboot.fip" \
		"bl31-uboot.fip (fip.bin)"
}

#------------------------------------------------------------------------------
# GPT images (eMMC / SDMMC)
#
# Boards booting from eMMC/SD carry their partition table as a separate image,
# generated here with tools/ptgen (built along with U-Boot whenever CONFIG_MMC
# is enabled).  The layouts are the MediaTek Filogic / OpenWrt ones and line up
# with the offsets U-Boot and TF-A expect:
#
#   ubootenv 512k @ 4M      factory  2M @ 4608k
#   fip        4M @ 6656k   recovery 32M @ 12M
#   production ${ROOTFS_PARTSIZE}M @ 64M
#
#   The eMMC layout keeps BL2 in the eMMC boot partition, so the user area
#   starts at the first MiB boundary (-l 1024) and only needs the environment
#   onwards.  The SD layout has BL2 in the user area as well (bl2, 4079k @ 17k,
#   hybrid MBR so a card reader can find it) plus an install partition for the
#   factory image.
#------------------------------------------------------------------------------

# ptgen is a U-Boot host tool, so it exists only if U-Boot was built.  Build
# the host tools on demand so STAGE=atf (which reuses an existing u-boot.bin)
# can still emit the GPT images.  Sets PTGEN to the tool path.
ensure_ptgen() {
	PTGEN="${UBOOT_DIR}/tools/ptgen"

	if [ ! -x "${PTGEN}" ]; then
		info "ptgen not built yet; building the host tools (make tools-only)"
		( cd "${UBOOT_DIR}" && \
		  make CROSS_COMPILE="${TOOLCHAIN}" STAGING_DIR="${STAGING_DIR:-}" \
			-j "${JOBS}" tools-only )
	fi

	[ -x "${PTGEN}" ] || die "ptgen was not built: ${PTGEN} (needs CONFIG_MMC=y)"
}

# Generate one GPT image.  ptgen prints a start/size pair per partition, which
# nothing here consumes, so keep it out of the build log and only show it if the
# generation failed.
ptgen_gpt() {
	local ptgen="$1" out="$2"
	shift 2

	if ! "${ptgen}" -g -o "${out}" "$@" >/dev/null 2>"${out}.log"; then
		error "GPT generation failed: $(basename "${out}")"
		sed 's/^/    /' "${out}.log"
		rm -f "${out}" "${out}.log"
		exit 1
	fi
	rm -f "${out}.log"
}

generate_gpt() {
	if [ -z "${GPT}" ]; then
		return 0
	fi

	step "Generate GPT [${GPT}]"

	if [ "${UBOOT_MMC}" != "y" ]; then
		die "GPT is requested for '${BOARD_KEY}' but CONFIG_MMC is not enabled in ${DEFCONFIG}."
	fi

	local PTGEN tmp out layout
	ensure_ptgen
	tmp=$(mktemp -d "${OUTPUT_DIR}/.gpt.XXXXXX")

	for layout in ${GPT}; do
		out="${tmp}/${layout}-gpt.bin"
		case "${layout}" in
		emmc)
			ptgen_gpt "${PTGEN}" "${out}" \
				-a 1 -l 1024 \
				-t 0x83 -N ubootenv   -r -p 512k@4M \
				-t 0x83 -N factory    -r -p 2M@4608k \
				-t 0xef -N fip        -r -p 4M@6656k \
				        -N recovery   -r -p 32M@12M \
				-t 0x2e -N production    -p "${ROOTFS_PARTSIZE}M@64M"
			;;
		sdmmc)
			ptgen_gpt "${PTGEN}" "${out}" \
				-a 1 -l 1024 -H \
				-t 0x83 -N bl2        -r -p 4079k@17k \
				-t 0x83 -N ubootenv   -r -p 512k@4M \
				-t 0x83 -N factory    -r -p 2M@4608k \
				-t 0xef -N fip        -r -p 4M@6656k \
				        -N recovery   -r -p 32M@12M \
				        -N install    -r -p 20M@44M \
				-t 0x2e -N production    -p "${ROOTFS_PARTSIZE}M@64M"
			;;
		esac

		copy_with_md5 "${out}" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-${layout}-gpt.bin" \
			"${layout}-gpt.bin"
	done

	rm -rf "${tmp}"
}

#------------------------------------------------------------------------------
# Print Summary
#------------------------------------------------------------------------------
print_summary() {
	echo ""
	echo "==========================================================================="
	echo -e "  ${GREEN}${SOC_UPPER} ${BOARD} U-Boot + TF-A build completed!${NC}"
	echo "==========================================================================="
	echo ""
	echo "  Output directory: ${OUTPUT_DIR}/"
	echo ""

	if [ ${#COPIED_FILES[@]} -gt 0 ]; then
		local f base size
		for f in "${COPIED_FILES[@]}"; do
			[ -f "$f" ] || continue
			base=$(basename "$f")
			size=$(stat -c%s "$f")
			printf "    %-68s  %10s bytes\n" "${base}" "${size}"
		done
	else
		echo "    (nothing collected: this build has no flashable artifact)"
	fi

	if [ "${STAGE}" = "uboot" ]; then
		echo ""
		echo "  Note: STAGE=uboot, the TF-A stage was skipped, so no"
		echo "        preloader.bin / bl31-uboot.fip was produced."
		echo "        U-Boot: ${UBOOT_DIR}/${UBOOT_BUILD_TARGET}"
	fi

	echo ""
	echo "==========================================================================="
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------
main() {
	echo ""
	echo "==========================================================================="
	echo "	MediaTek U-Boot ${UBOOT_VERSION} Build Script"
	echo "  Build for ${SOC_UPPER} ${BOARD}"
	echo "  Source:    ${UBOOT_DIR}"
	echo "  Defconfig: ${DEFCONFIG}"
	echo "  Output:    ${OUTPUT_DIR}"
	echo "==========================================================================="

	resolve_board
	if [ "${STAGE}" != "uboot" ]; then
		resolve_atf_config
	fi
	check_environment
	configure_uboot
	detect_build_features

	if [ "${STAGE}" = "atf" ]; then
		# Reuse an existing u-boot.bin instead of rebuilding it.
		if [ ! -f "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" ]; then
			die "STAGE=atf needs an existing ${UBOOT_BUILD_TARGET}; run STAGE=uboot (or all) first."
		fi
		info "Reusing existing ${UBOOT_BUILD_TARGET}"
		build_atf
	else
		build_uboot
		if [ "${STAGE}" != "uboot" ]; then
			build_atf
		fi
	fi

	copy_outputs
	generate_gpt

	print_summary
}

main "$@"
