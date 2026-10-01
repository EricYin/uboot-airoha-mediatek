#!/bin/bash
#===============================================================================
# econet.sh - EcoNet MIPS (EN751221 / EN751627 / EN7528 / EN7580) build script
#
# Usage: SOC=<soc> BOARD=<board> [OPTIONS] ./econet.sh
#
# An EcoNet device cannot be built from this repository alone; the boot chain
# is split over two trees, just like the MediaTek Filogic boards:
#
#   uboot-2026.10-rc5 (this tree)   U-Boot proper          -> u-boot.bin
#   ../airoha_mips_dramc            DRAMC + chainload +
#                                   TCBoot flash loader    -> out/<soc>/tcboot.bin
#
# U-Boot proper is entered only *after* DRAM is up, so this tree produces just
# u-boot.bin; the DRAM calibration, the post-DRAM flash reader and the TCBoot
# loader live in airoha_mips_dramc.  econet.sh builds both and attaches
# u-boot.bin behind the TCBoot loader:
#
#   <soc>_<board>-tcboot.bin    TCBoot loader + ECNT descriptor + u-boot.bin,
#                               i.e. the flashable bootloader image
#   <soc>_<board>-bootext.bin   optional FE-SRAM recovery/chainload image
#   <soc>_<board>-recovery-chainloader.bin
#                               optional BootROM/XMODEM helper (en751221)
#
# u-boot.bin is an intermediate of that chain, so it is not copied into the
# output directory unless COPY_UBOOT=1 asks for it.
#
# Required:
#   SOC=<soc>       Target SoC: en751221 | en751627 | en7528 | en7580
#   BOARD=<board>   Board name, i.e. the U-Boot defconfig without the
#                   "${SOC}_" prefix and the "_defconfig" suffix
#                   (the reference boards use BOARD=reference)
#
# Options:
#   STAGE=<stage>       all (default) | uboot | dramc
#   DRAMC_DIR=<path>    Early-boot tree (default: ../airoha_mips_dramc)
#   TOOLCHAIN=<prefix>  Cross-compiler prefix (default: per-SoC, see below)
#   AUTO_DL=1           Download the toolchain without prompting when no local
#                       copy is found (also --auto-download / -d)
#   FORCE_DL=1          Force (re-)download the toolchain even when a local
#                       copy is found (also --force-download / -f)
#   JOBS=<n>            Parallel make jobs (default: nproc)
#   STAGING_DIR=<path>  Passed to make (default: the toolchain root on LE)
#   COPY_UBOOT=1        Also collect the intermediate u-boot.bin
#   DRAMC_EXTRA=0       Skip the bootext / recovery chainloader artifacts
#   DRAMC_LLVM=1        Build the DRAMC/early-boot stages with LLVM/Clang
#   DRAMC_NM=<nm>       nm used by the DRAMC build (default: llvm-nm if present)
#   PAD_TO=<size>       Pad the final TCBoot image (e.g. PAD_TO=0x100000)
#
# Toolchain selection:
#   No EcoNet-specific toolchain exists, so the existing ones are reused and
#   picked by the SoC's endianness:
#
#     en751221 / en751627   big endian    -> system mips-linux-gnu-
#                                            (apt install gcc-mips-linux-gnu)
#     en7528 / en7580       little endian -> MT7621 OpenWrt toolchain
#                                            (ramips/mt7621, prefix
#                                            mipsel-openwrt-linux-), looked up
#                                            next to this tree and downloaded
#                                            there when missing, exactly like
#                                            mtmips.sh does
#
# Toolchain download (little endian SoCs only; a download is prompted for when
# no local copy is found):
#   -d, --auto-download    download without prompting          (env AUTO_DL=1)
#   -f, --force-download   force (re-)download even when a local copy exists
#                                                              (env FORCE_DL=1)
#
# Examples:
#   SOC=en7528   BOARD=reference ./econet.sh
#   SOC=en751221 BOARD=reference ./econet.sh
#   SOC=en751627 BOARD=reference ./econet.sh
#   SOC=en7580   BOARD=reference ./econet.sh
#
#   # Toolchain handling:
#   SOC=en7528 BOARD=reference ./econet.sh -d    # download if missing
#   SOC=en7528 BOARD=reference ./econet.sh -f    # force re-download
#
#   # U-Boot only, no early-boot tree required:
#   SOC=en7528 BOARD=reference STAGE=uboot ./econet.sh
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
# Defaults for command-line / environment options
#------------------------------------------------------------------------------
AUTO_DL="${AUTO_DL:-0}"
FORCE_DL="${FORCE_DL:-0}"

#------------------------------------------------------------------------------
# --help / -h: print usage extracted from the header comment block
#------------------------------------------------------------------------------
usage() {
	sed -n '2,/^[^#]/p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
	exit 0
}

#------------------------------------------------------------------------------
# Parse command-line arguments (the same switches are available as env vars)
#------------------------------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--help|-h|help)
			usage
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
			error "Unknown option: $1"
			echo "Try '$0 --help' for more information."
			exit 1
			;;
	esac
	shift
done

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
#
# Unlike mediatek.sh - where every board pairs with its own TF-A configuration -
# the early-boot side of an EcoNet board is fully described by its SoC:
# endianness, toolchain, DRAMC target and payload entry point.  There is
# therefore no separate board table; adding a board is adding a defconfig.
#------------------------------------------------------------------------------
SOC="${SOC,,}" # Transform to lowercase

if [ -z "${SOC}" ] || [ -z "${BOARD}" ]; then
	error "SOC and BOARD environment variables must be specified."
	echo "Usage: SOC=<soc> BOARD=<board> [OPTIONS] $0"
	echo "Try '$0 --help' for more information."
	exit 1
fi

case "${SOC}" in
	en751221|en751627)
		SOC_ENDIAN="big"
		SOC_TOOLCHAIN="mips-linux-gnu-"
		SOC_TOOLCHAIN_HINT="sudo apt install -y gcc-mips-linux-gnu"
		;;
	en7528|en7580)
		SOC_ENDIAN="little"
		SOC_TOOLCHAIN="mipsel-openwrt-linux-"
		SOC_TOOLCHAIN_HINT=""
		;;
	*)
		error "Not supported SOC: ${SOC}"
		echo "Supported: en751221, en751627, en7528, en7580 (EcoNet MIPS)"
		exit 1
		;;
esac

SOC_UPPER="${SOC^^}"

#------------------------------------------------------------------------------
# Path and Toolchain Configuration
#------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="${SCRIPT_DIR}"
PARENT_DIR="$(cd "${UBOOT_DIR}/.." && pwd)"
OUTPUT_DIR="${UBOOT_DIR}/output_econet"

DEFCONFIG="${SOC}_${BOARD}_defconfig"
DEFCONFIG_PATH="${UBOOT_DIR}/configs/${DEFCONFIG}"
OUTPUT_PREFIX="${SOC}_${BOARD}"

DRAMC_DIR="${DRAMC_DIR:-${UBOOT_DIR}/../airoha_mips_dramc}"
DRAMC_OUT="${DRAMC_DIR}/out"
DRAMC_TOOL="${DRAMC_OUT}/host/econet-image"

STAGE="${STAGE:-all}"
COPY_UBOOT="${COPY_UBOOT:-0}"
DRAMC_EXTRA="${DRAMC_EXTRA:-1}"
DRAMC_LLVM="${DRAMC_LLVM:-0}"
PAD_TO="${PAD_TO:-0}"

case "${STAGE}" in
	all|uboot|dramc)
		;;
	*)
		error "Unsupported STAGE: ${STAGE} (use all, uboot or dramc)"
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

# Filled in while resolving the toolchain, the early-boot tree and the build.
TOOLCHAIN=""
STAGING_DIR="${STAGING_DIR:-}"
DRAMC_TOOLCHAIN=""
DRAMC_NM_SELECTED=""
DRAMC_CAP_TCBOOT=""
DRAMC_CAP_BOOTEXT=""
DRAMC_CAP_RECOVERY=""
UBOOT_BUILD_TARGET=""
UBOOT_LOAD_ADDR="0x81000000"

# Files copied by the current build: print_summary only lists these.
COPIED_FILES=()
TCBOOT_ARTIFACT=""

#------------------------------------------------------------------------------
# Helpers
#------------------------------------------------------------------------------

# Insert an "_md5-<hash>" tag before the file extension: the naming the
# failsafe web UI expects on an uploaded image.  The tag format follows the
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
	[ -f "$file" ] || die "Missing artifact: $file"
	md5=$(md5sum "$file" | awk '{print $1}')
	md5_dest=$(insert_md5_into_name "$dest" "$md5")
	cp -f "$file" "$md5_dest"
	COPIED_FILES+=("${md5_dest}")
	info "${label} (md5: ${md5}) -> ${md5_dest}"
}

# Read a CONFIG_ value from the generated .config (quotes stripped).
get_uboot_config() {
	local val
	val=$(sed -n "s/^CONFIG_$1=//p" "${UBOOT_DIR}/.config" 2>/dev/null | head -1)
	val="${val%\"}"
	val="${val#\"}"
	echo "${val}"
}

# Read a capability flag out of airoha_mips_dramc's images/<soc>.mk, so the
# script follows the early-boot tree instead of duplicating its per-SoC status
# (en7580, for instance, only ships DRAMC + chainload today).
#   dramc_flag <soc> <variable> <default>
dramc_flag() {
	local soc="$1" var="$2" def="$3" val
	val=$(sed -n "s/^[[:space:]]*${var}[[:space:]]*:=[[:space:]]*\([yn]\)[[:space:]]*$/\1/p" \
		"${DRAMC_DIR}/images/${soc}.mk" 2>/dev/null | head -1)
	echo "${val:-${def}}"
}

# Pad a file up to a size with 0xff, the erased state of the boot flash.
pad_file() {
	local file="$1" size="$2" cur
	cur=$(stat -c%s "$file")
	if [ "${cur}" -lt "${size}" ]; then
		dd if=/dev/zero bs=1 count=$((size - cur)) 2>/dev/null | \
			tr '\000' '\377' >> "${file}"
	fi
}

#------------------------------------------------------------------------------
# Toolchain resolution
#
# Big endian SoCs reuse the system mips-linux-gnu toolchain; little endian SoCs
# reuse the MT7621 OpenWrt toolchain, which is looked up next to this tree and
# downloaded there when missing - the same convention mtmips.sh uses.
#------------------------------------------------------------------------------
OPENWRT_TOOLCHAIN_PATTERN="openwrt*mt7621*"
OPENWRT_TOOLCHAIN_URL="${OPENWRT_TOOLCHAIN_URL:-https://downloads.openwrt.org/releases/25.12.5/targets/ramips/mt7621/openwrt-toolchain-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst}"

OPENWRT_TOOLCHAIN_BIN=""

find_openwrt_toolchain() {
	local dir
	OPENWRT_TOOLCHAIN_BIN=""
	for dir in ${PARENT_DIR}/${OPENWRT_TOOLCHAIN_PATTERN}/toolchain-mipsel*/bin; do
		if [ -d "${dir}" ]; then
			OPENWRT_TOOLCHAIN_BIN=$(cd "${dir}" && pwd)
			return 0
		fi
	done
	return 1
}

download_openwrt_toolchain() {
	info "Downloading toolchain from: ${OPENWRT_TOOLCHAIN_URL}"
	cd "${PARENT_DIR}"
	if command -v wget >/dev/null 2>&1; then
		wget -O - "${OPENWRT_TOOLCHAIN_URL}" | tar --zstd -xf - || return 1
	elif command -v curl >/dev/null 2>&1; then
		curl -L "${OPENWRT_TOOLCHAIN_URL}" | tar --zstd -xf - || return 1
	else
		error "Neither wget nor curl found. Download manually: ${OPENWRT_TOOLCHAIN_URL}"
		return 1
	fi
	cd "${UBOOT_DIR}"
}

resolve_toolchain() {
	step "Resolve Toolchain [${SOC_UPPER}, ${SOC_ENDIAN} endian]"

	if [ -n "${TOOLCHAIN}" ]; then
		info "Toolchain: ${TOOLCHAIN} (from environment)"
	else
		case "${SOC_ENDIAN}" in
			big)
				if [ "${FORCE_DL}" = "1" ]; then
					warn "FORCE_DL has no effect on big endian SoCs: ${SOC} uses"
					warn "the system mips-linux-gnu- toolchain."
				fi
				TOOLCHAIN="${SOC_TOOLCHAIN}"
				info "Toolchain: ${TOOLCHAIN} (big endian, system)"
				;;
			little)
				if [ "${FORCE_DL}" = "1" ]; then
					info "FORCE_DL set: (re-)downloading toolchain from:"
					info "  ${OPENWRT_TOOLCHAIN_URL}"
					download_openwrt_toolchain || \
						die "Toolchain download failed."
					find_openwrt_toolchain || \
						die "Toolchain not found after extraction."
				elif ! find_openwrt_toolchain; then
					if [ "${AUTO_DL}" = "1" ]; then
						info "AUTO_DL set: downloading toolchain from:"
						info "  ${OPENWRT_TOOLCHAIN_URL}"
						download_openwrt_toolchain || \
							die "Toolchain download failed."
						find_openwrt_toolchain || \
							die "Toolchain not found after extraction."
					else
						warn "MT7621 OpenWrt toolchain not found in ${PARENT_DIR}."
						printf "Download it now? [Y/n] "
						dlcc=""
						read -r dlcc || true
						case "${dlcc:-Y}" in
							[Yy]*)
								download_openwrt_toolchain || \
									die "Toolchain download failed."
								find_openwrt_toolchain || \
									die "Toolchain not found after extraction."
								;;
							*)
								die "Toolchain required. Set TOOLCHAIN=<prefix> or place ${OPENWRT_TOOLCHAIN_PATTERN}/toolchain-mipsel*/ in ${PARENT_DIR}."
								;;
						esac
					fi
				fi
				TOOLCHAIN="${OPENWRT_TOOLCHAIN_BIN}/${SOC_TOOLCHAIN}"
				info "Toolchain: ${TOOLCHAIN} (little endian, MT7621 OpenWrt)"
				;;
		esac
	fi

	if ! command -v "${TOOLCHAIN}gcc" &>/dev/null; then
		error "Cross toolchain not found: ${TOOLCHAIN}gcc"
		if [ -n "${SOC_TOOLCHAIN_HINT}" ]; then
			error "Please install it: ${SOC_TOOLCHAIN_HINT}"
		fi
		exit 1
	fi
	info "gcc: $(${TOOLCHAIN}gcc --version | head -1)"

	# The OpenWrt toolchain is a wrapper that needs STAGING_DIR to locate its
	# sysroot, the same requirement mtmips.sh has.  It is only derivable when
	# the prefix points at a toolchain directory.
	if [ "${SOC_ENDIAN}" = "little" ] && [ -z "${STAGING_DIR}" ]; then
		case "${TOOLCHAIN}" in
			*/bin/*)
				STAGING_DIR="${TOOLCHAIN%/bin/*}"
				;;
		esac
	fi
	if [ -n "${STAGING_DIR}" ]; then
		info "STAGING_DIR: ${STAGING_DIR}"
	fi

	# The early-boot stages are linked for the same core and endianness as
	# U-Boot proper, so they use the same toolchain.
	DRAMC_TOOLCHAIN="${TOOLCHAIN}"
}

#------------------------------------------------------------------------------
# Environment Check
#------------------------------------------------------------------------------
check_environment() {
	step "Environment Check [SOC: ${SOC_UPPER}] [BOARD: ${BOARD}]"

	if ! command -v python3 >/dev/null 2>&1; then
		error "Python 3 is not installed."
		error "Please install: sudo apt install -y python3"
		exit 1
	fi
	info "Python3: $(python3 --version 2>&1)"

	if [ ! -f "${DEFCONFIG_PATH}" ]; then
		error "Defconfig not found: ${DEFCONFIG_PATH}"
		echo "Configurations available for ${SOC}:"
		ls -1 "${UBOOT_DIR}/configs" 2>/dev/null | grep -E "^${SOC}_.*_defconfig$" | sed 's/^/  /'
		exit 1
	fi
	info "Defconfig: ${DEFCONFIG}"

	if [ "${STAGE}" != "uboot" ]; then
		if [ ! -d "${DRAMC_DIR}" ]; then
			error "Early-boot tree not found: ${DRAMC_DIR}"
			error "Clone airoha_mips_dramc next to this tree, or set DRAMC_DIR=<path>."
			exit 1
		fi
		if [ ! -f "${DRAMC_DIR}/Makefile" ]; then
			error "${DRAMC_DIR}/Makefile not found: is ${DRAMC_DIR} the airoha_mips_dramc tree?"
			exit 1
		fi
		if [ ! -f "${DRAMC_DIR}/images/${SOC}.mk" ]; then
			error "${SOC} is not supported by the early-boot tree:"
			error "  ${DRAMC_DIR}/images/${SOC}.mk not found"
			exit 1
		fi
		if ! command -v cc >/dev/null 2>&1; then
			error "Host C compiler 'cc' is not available."
			error "The early-boot tree builds its host image tool with it."
			exit 1
		fi
		info "Early-boot tree: ${DRAMC_DIR}"
	fi

	mkdir -p "${OUTPUT_DIR}"
	info "Environment Check passed"
}

#------------------------------------------------------------------------------
# Resolve the early-boot targets
#------------------------------------------------------------------------------
resolve_dramc_targets() {
	step "Resolve Early-Boot Targets [${SOC}]"

	DRAMC_CAP_TCBOOT=$(dramc_flag "${SOC}" SOC_TCBOOT n)
	DRAMC_CAP_BOOTEXT=$(dramc_flag "${SOC}" SOC_BOOTEXT n)
	DRAMC_CAP_RECOVERY=$(dramc_flag "${SOC}" SOC_RECOVERY_CHAINLOADER n)

	if [ "${DRAMC_CAP_TCBOOT}" != "y" ]; then
		warn "${SOC} has no standalone TCBoot target in the early-boot tree, so no"
		warn "flashable ${OUTPUT_PREFIX}-tcboot.bin can be produced (see its README:"
		warn "the DRAMC reconstruction for this SoC is still WIP)."
	fi
	if [ "${DRAMC_CAP_BOOTEXT}" != "y" ]; then
		info "${SOC} has no standalone bootext target; skipping it"
	fi
	if [ "${DRAMC_CAP_RECOVERY}" != "y" ]; then
		info "${SOC} has no BootROM recovery chainloader; skipping it"
	fi
}

#------------------------------------------------------------------------------
# Resolve the nm used by the early-boot build
#
# That tree parses `nm` output and expects plain 32-bit addresses.  binutils'
# nm (up to at least 2.38) sign-extends 32-bit MIPS addresses whose top bit is
# set - 0xbfc002b0 becomes 0xffffffffbfc002b0 - which the parser rejects, so
# every stage that reads symbols fails on such a toolchain.  llvm-nm always
# prints the plain value, so prefer it when available.
#------------------------------------------------------------------------------
resolve_dramc_nm() {
	step "Resolve DRAMC nm"

	if [ -n "${DRAMC_NM}" ]; then
		DRAMC_NM_SELECTED="${DRAMC_NM}"
		info "nm: ${DRAMC_NM_SELECTED} (from environment)"
		return 0
	fi

	if [ "${DRAMC_LLVM}" = "1" ]; then
		info "nm: llvm-nm (LLVM build)"
		return 0
	fi

	if command -v llvm-nm >/dev/null 2>&1; then
		DRAMC_NM_SELECTED="llvm-nm"
		info "nm: llvm-nm (the cross nm sign-extends 32-bit addresses on old binutils)"
	else
		DRAMC_NM_SELECTED="${DRAMC_TOOLCHAIN}nm"
		warn "llvm-nm not found; falling back to ${DRAMC_NM_SELECTED}"
		warn "If the build fails with 'missing ... interval symbols', the cross nm is"
		warn "sign-extending 32-bit addresses; install LLVM or set DRAMC_NM=<nm>."
	fi
}

#------------------------------------------------------------------------------
# Build U-Boot
#------------------------------------------------------------------------------
build_uboot() {
	step "Build U-Boot [${SOC_UPPER}]"

	local makeargs=(-C "${UBOOT_DIR}" ARCH=mips CROSS_COMPILE="${TOOLCHAIN}")
	if [ -n "${STAGING_DIR}" ]; then
		makeargs+=(STAGING_DIR="${STAGING_DIR}")
	fi

	cp -f "${DEFCONFIG_PATH}" "${UBOOT_DIR}/.config"
	make "${makeargs[@]}" olddefconfig

	# A target that names a configuration header which does not exist fails
	# halfway through the build with a bare compiler error; catch it here.
	local config_name
	config_name=$(get_uboot_config SYS_CONFIG_NAME)
	if [ -n "${config_name}" ] && \
	   [ ! -f "${UBOOT_DIR}/include/configs/${config_name}.h" ]; then
		error "${SOC} selects CONFIG_SYS_CONFIG_NAME=\"${config_name}\", but"
		error "  ${UBOOT_DIR}/include/configs/${config_name}.h does not exist."
		error "The board port is incomplete in this tree; either add that header"
		error "(and board/airoha/${config_name}/), or point CONFIG_SYS_BOARD and"
		error "CONFIG_SYS_CONFIG_NAME at the SoC it shares its code with."
		exit 1
	fi

	make "${makeargs[@]}" clean
	make "${makeargs[@]}" -j "${JOBS}" all

	UBOOT_BUILD_TARGET=$(get_uboot_config BUILD_TARGET)
	if [ -z "${UBOOT_BUILD_TARGET}" ]; then
		UBOOT_BUILD_TARGET="u-boot.bin"
	fi

	if [ ! -f "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" ]; then
		die "U-Boot build failed: ${UBOOT_BUILD_TARGET} not generated"
	fi
	info "U-Boot build done: ${UBOOT_BUILD_TARGET} ($(stat -c%s "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}") bytes)"
}

#------------------------------------------------------------------------------
# Detect Build Features
#------------------------------------------------------------------------------
detect_build_features() {
	step "Detect Build Features"

	local soc_family
	soc_family=$(get_uboot_config SYS_SOC)
	if [ -z "${soc_family}" ]; then
		soc_family="${SOC}"
	fi
	UBOOT_DTB=$(get_uboot_config DEFAULT_DEVICE_TREE)
	UBOOT_LOAD_ADDR=$(get_uboot_config TEXT_BASE)
	if [ -z "${UBOOT_LOAD_ADDR}" ]; then
		UBOOT_LOAD_ADDR="0x81000000"
	fi

	echo "SOC:                  ${SOC}"
	echo "SOC family:           ${soc_family}"
	echo "BOARD:                ${BOARD}"
	echo "Defconfig:            ${DEFCONFIG}"
	echo "Device tree:          ${UBOOT_DTB}"
	echo "U-Boot image:         ${UBOOT_BUILD_TARGET}"
	echo "Endianness:           ${SOC_ENDIAN}"
	echo "Toolchain:            ${TOOLCHAIN}"
	echo "Load/entry address:   ${UBOOT_LOAD_ADDR}"
	echo "Stage:                ${STAGE}"
	if [ "${STAGE}" != "uboot" ]; then
		echo "Early-boot tree:      ${DRAMC_DIR}"
		echo "DRAMC TCBoot:         ${DRAMC_CAP_TCBOOT}"
		echo "DRAMC bootext:        ${DRAMC_CAP_BOOTEXT}"
		echo "DRAMC recovery:       ${DRAMC_CAP_RECOVERY}"
		echo "DRAMC nm:             ${DRAMC_NM_SELECTED:-llvm-nm}"
	fi
}

#------------------------------------------------------------------------------
# Build the early-boot stages
#
# DRAMC and chainload are always built; TCBoot (the flash loader that receives
# U-Boot) plus the optional bootext / recovery images follow the per-SoC
# capabilities the early-boot tree advertises.
#------------------------------------------------------------------------------
compiler_accepts_oz() {
	local tmp ret=1
	tmp=$(mktemp -d)
	echo 'int probe_oz;' > "${tmp}/probe.c"
	if "${DRAMC_TOOLCHAIN}gcc" -Oz -c "${tmp}/probe.c" -o "${tmp}/probe.o" 2>/dev/null; then
		ret=0
	fi
	rm -rf "${tmp}"
	return "${ret}"
}

# Only en751627 overrides the TCBoot size option with -Oz; the others keep the
# -Os default, so the fallback below must not be announced (or applied) for
# them.
dramc_uses_oz() {
	grep -q 'TCBOOT_SIZE_OPT.*-Oz' "${DRAMC_DIR}/images/${SOC}.mk" 2>/dev/null
}

build_dramc() {
	step "Build Early-Boot Stages [${SOC}]"

	local targets=("${SOC}-dramc" "${SOC}-chainload")
	local makeargs=(-C "${DRAMC_DIR}" "O=${DRAMC_OUT}")

	if [ "${DRAMC_CAP_TCBOOT}" = "y" ]; then
		targets+=("${SOC}-tcboot")
	fi
	if [ "${DRAMC_EXTRA}" = "1" ]; then
		if [ "${DRAMC_CAP_BOOTEXT}" = "y" ]; then
			targets+=("${SOC}-bootext")
		fi
		if [ "${DRAMC_CAP_RECOVERY}" = "y" ]; then
			targets+=("${SOC}-recovery")
		fi
	fi

	makeargs+=(CROSS_COMPILE="${DRAMC_TOOLCHAIN}")
	if [ "${DRAMC_LLVM}" = "1" ]; then
		makeargs+=(LLVM=1)
	fi
	if [ -n "${DRAMC_NM_SELECTED}" ]; then
		makeargs+=(TOOL_NM="${DRAMC_NM_SELECTED}")
	fi

	# en751627 asks for -Oz, which GCC only accepts from GCC 12 on; fall back
	# to -Os when the compiler rejects it.  The flash.lds ASSERT and the
	# tcboot-base layout check still validate the result.
	if [ "${DRAMC_LLVM}" != "1" ] && dramc_uses_oz && ! compiler_accepts_oz; then
		warn "${DRAMC_TOOLCHAIN}gcc does not support -Oz (needs GCC >= 12)."
		warn "Building the TCBoot stages with -Os instead; flash.lds still"
		warn "checks that the stages stay clear of the manufacturing data."
		makeargs+=(TCBOOT_SIZE_OPT=-Os)
	fi

	if [ -n "${STAGING_DIR}" ]; then
		export STAGING_DIR
	fi

	info "Targets: ${targets[*]}"
	make "${makeargs[@]}" -j "${JOBS}" "${targets[@]}" || \
		die "Early-boot build failed (see the airoha_mips_dramc README for per-SoC status)."

	if [ ! -f "${DRAMC_OUT}/${SOC}/${SOC}-dramc.bin" ]; then
		die "DRAMC image was not generated."
	fi
	if [ ! -f "${DRAMC_OUT}/${SOC}/${SOC}-chainload.bin" ]; then
		die "Chainload image was not generated."
	fi
	info "Early-boot build done"
}

#------------------------------------------------------------------------------
# Attach U-Boot to the TCBoot loader
#
# The standalone TCBoot image ships without a payload on purpose: its loader
# understands only the small ECNT descriptor, and the image builder - this step
# or an OpenWrt image recipe - chooses what goes behind it.  The descriptor
# carries the load/entry pair, which for U-Boot proper is its CONFIG_TEXT_BASE.
#------------------------------------------------------------------------------
package_tcboot() {
	step "Attach U-Boot to the TCBoot Loader"

	local base="${DRAMC_OUT}/${SOC}/tcboot.bin"
	local out="${DRAMC_OUT}/${SOC}/tcboot-image.bin"

	if [ ! -f "${base}" ]; then
		die "TCBoot base image not found: ${base}"
	fi
	if [ ! -x "${DRAMC_TOOL}" ]; then
		die "Host image tool not found: ${DRAMC_TOOL}"
	fi

	rm -f "${out}"
	"${DRAMC_TOOL}" tcboot \
		--boot "${base}" \
		--payload "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" \
		--load "${UBOOT_LOAD_ADDR}" \
		--entry "${UBOOT_LOAD_ADDR}" \
		--output "${out}"

	if [ ! -s "${out}" ]; then
		die "TCBoot image was not composed."
	fi
	info "Composed: ${out} ($(stat -c%s "${out}") bytes)"

	if [ "${PAD_TO}" != "0" ]; then
		pad_file "${out}" "$((PAD_TO))"
		info "Padded to $(stat -c%s "${out}") bytes with 0xff"
	fi
}

#------------------------------------------------------------------------------
# Copy Output Files
#------------------------------------------------------------------------------
copy_outputs() {
	step "Copy Output Files"

	cd "${UBOOT_DIR}"
	mkdir -p "${OUTPUT_DIR}"

	# Drop this board's artifacts from a previous build: the md5 tag makes
	# every rebuild a new filename, so without this the directory would keep
	# growing and it would not be obvious which pair is the current one.
	# A STAGE=uboot run only remakes U-Boot, so it must not discard an
	# already-built flashable image.
	local old
	if [ "${STAGE}" = "uboot" ]; then
		for old in "${OUTPUT_DIR}/${OUTPUT_PREFIX}-${UBOOT_BUILD_TARGET}"*; do
			if [ -f "${old}" ]; then
				rm -f "${old}"
			fi
		done
	else
		for old in "${OUTPUT_DIR}/${OUTPUT_PREFIX}-"*; do
			if [ -f "${old}" ]; then
				rm -f "${old}"
			fi
		done
	fi

	if [ "${COPY_UBOOT}" = "1" ]; then
		copy_with_md5 "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-${UBOOT_BUILD_TARGET}" \
			"${UBOOT_BUILD_TARGET}"
	else
		info "${UBOOT_BUILD_TARGET} left in place: ${UBOOT_DIR}/${UBOOT_BUILD_TARGET}"
		info "  (set COPY_UBOOT=1 to also collect it into ${OUTPUT_DIR}/)"
	fi

	if [ "${STAGE}" = "uboot" ]; then
		return 0
	fi

	if [ -f "${DRAMC_OUT}/${SOC}/tcboot-image.bin" ]; then
		copy_with_md5 "${DRAMC_OUT}/${SOC}/tcboot-image.bin" \
			"${OUTPUT_DIR}/${OUTPUT_PREFIX}-tcboot.bin" \
			"tcboot.bin (TCBoot loader + U-Boot)"
		TCBOOT_ARTIFACT="$(basename "${COPIED_FILES[${#COPIED_FILES[@]}-1]}")"
	fi

	if [ "${DRAMC_EXTRA}" = "1" ]; then
		if [ "${DRAMC_CAP_BOOTEXT}" = "y" ] && \
		   [ -f "${DRAMC_OUT}/${SOC}/bootext.bin" ]; then
			copy_with_md5 "${DRAMC_OUT}/${SOC}/bootext.bin" \
				"${OUTPUT_DIR}/${OUTPUT_PREFIX}-bootext.bin" \
				"bootext.bin"
		fi
		if [ "${DRAMC_CAP_RECOVERY}" = "y" ]; then
			local recovery
			recovery=$(ls -1 "${DRAMC_OUT}/${SOC}/"*recovery*chainloader*.bin 2>/dev/null | head -1 || true)
			if [ -n "${recovery}" ]; then
				copy_with_md5 "${recovery}" \
					"${OUTPUT_DIR}/${OUTPUT_PREFIX}-recovery-chainloader.bin" \
					"recovery-chainloader.bin"
			fi
		fi
	fi
}

#------------------------------------------------------------------------------
# Print Summary
#------------------------------------------------------------------------------
print_summary() {
	echo ""
	echo "==========================================================================="
	echo -e "  ${GREEN}${SOC_UPPER} ${BOARD} U-Boot + early-boot build completed!${NC}"
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
		if [ -n "${TCBOOT_ARTIFACT}" ]; then
			echo ""
			echo "  Flash ${TCBOOT_ARTIFACT} at offset 0 of the boot region."
		fi
	else
		echo "    (nothing collected: this build has no flashable artifact)"
	fi

	if [ "${STAGE}" != "uboot" ] && [ "${DRAMC_CAP_TCBOOT}" != "y" ]; then
		echo ""
		echo "  Note: ${SOC} has no TCBoot target in the early-boot tree yet, so no"
		echo "        flashable image was produced."
	fi
	if [ "${STAGE}" = "uboot" ]; then
		echo ""
		echo "  Note: STAGE=uboot, the early-boot stages were skipped."
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
	echo "	EcoNet U-Boot ${UBOOT_VERSION} Build Script"
	echo "  Build for ${SOC_UPPER} ${BOARD}"
	echo "  Source:    ${UBOOT_DIR}"
	echo "  Defconfig: ${DEFCONFIG}"
	echo "  Output:    ${OUTPUT_DIR}"
	echo "==========================================================================="

	resolve_toolchain
	check_environment

	if [ "${STAGE}" != "uboot" ]; then
		resolve_dramc_targets
		resolve_dramc_nm
	fi

	if [ "${STAGE}" = "dramc" ]; then
		if [ ! -f "${UBOOT_DIR}/.config" ]; then
			die "STAGE=dramc needs a configured tree; run STAGE=uboot (or all) first."
		fi
		UBOOT_BUILD_TARGET=$(get_uboot_config BUILD_TARGET)
		if [ -z "${UBOOT_BUILD_TARGET}" ]; then
			UBOOT_BUILD_TARGET="u-boot.bin"
		fi
		if [ ! -f "${UBOOT_DIR}/${UBOOT_BUILD_TARGET}" ]; then
			die "STAGE=dramc needs an existing ${UBOOT_BUILD_TARGET}; run STAGE=uboot (or all) first."
		fi
		info "Reusing existing ${UBOOT_BUILD_TARGET}"
	else
		build_uboot
	fi

	detect_build_features

	if [ "${STAGE}" != "uboot" ]; then
		build_dramc
		if [ "${DRAMC_CAP_TCBOOT}" = "y" ]; then
			package_tcboot
		fi
	fi

	copy_outputs
	print_summary
}

main "$@"
