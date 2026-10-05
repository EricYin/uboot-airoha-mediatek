#!/usr/bin/env python3
"""
gen_build_info_log.py - Generate version and build-date changelog for BL2/BL31
binaries found under the build_airoha directory.

The blobs are organized in two release groups (mirroring the build_airoha
directory layout, see tools/build_airoha/Makefile):
  legacy/      - SDK prebuilt BL2/BL31 binaries
  opensource/  - open-source BL2/BL31 binaries
Each group holds per-SoC/variant subdirectories (e.g. an7581_default,
en7562_fiberhome_sr1041f).  The log presents the images grouped by release
group and then by variant.

Usage:
  python3 gen_build_info_log.py
  Runs in-place and writes 'firmware_build_log.txt' next to this script.
"""

import os
import re
import sys
import glob
import hashlib
import zlib
from datetime import datetime

# Add the tools/ parent dir to sys.path so we can import the two analysis scripts
TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import airoha_info_bl31 as bl31_tool
import airoha_info_preloader as bl2_tool

# Release groups under tools/build_airoha/
RELEASE_GROUPS = ('legacy', 'opensource')
GROUP_LABELS = {
    'legacy': 'Legacy (SDK)',
    'opensource': 'Open source',
}

# Validity thresholds
BL2_MIN_SIZE = 0x4000   # preloader must be >= 16 KB
BL31_MIN_SIZE = 0x100   # TF-A payload must be >= 256 B


# ---------------------------------------------------------------------------
# Helpers: extract version / build-date from analysis results
# ---------------------------------------------------------------------------

def extract_bl31_info(analyzed_info):
    """Extract version / build-date from BL31 analysis result.

    Mirrors the logic used by bl31's print_summary / print_build_info.
    """
    build_strs = analyzed_info.get('build_strings') or []
    version = None
    extra = None
    build_date = None
    version_raw = None
    standalone_version_seen = False

    for _, s in build_strs:
        # Classic combined format: v2.1(release):<commit> or v2.3():<commit>
        m = re.search(r'v(\d+\.\d+)\(([^)]*)\):?\s*([0-9a-fA-F].*)?', s)
        if m:
            version = m.group(1)
            paren_content = m.group(2).strip() if m.group(2) else None
            if m.group(3):
                extra_parts = []
                if paren_content:
                    extra_parts.append(paren_content)
                tail = m.group(3).strip()
                if tail:
                    extra_parts.append(tail)
                extra = ':'.join(extra_parts) if extra_parts else None
            elif paren_content:
                extra = paren_content
            version_raw = s
            continue

        m = re.search(r'v(\d+\.\d+):\s*(.*)', s)
        if m:
            version = m.group(1)
            extra = m.group(2).strip()
            version_raw = s
            continue

        # Standalone 3-part version: v2.10.0
        m = re.search(r'v(\d+\.\d+\.\d+)$', s)
        if m:
            version = m.group(1)
            standalone_version_seen = True
            version_raw = None
            continue

        # Standalone (xxx):commit that follows a standalone vX.Y.Z token
        m = re.search(r'^\(([^)]*)\):?\s*([0-9a-fA-F].*)?', s)
        if m:
            paren_content = m.group(1).strip() if m.group(1) else None
            tail = m.group(2).strip() if m.group(2) else None
            extra_parts = []
            if paren_content:
                extra_parts.append(paren_content)
            if tail:
                extra_parts.append(tail)
            extra = ':'.join(extra_parts) if extra_parts else None
            continue

        # Build date: Built : 02:52:34, Jun 29 2021
        m = re.search(r'Built\s*:\s*(.*)', s)
        if m:
            build_date = m.group(1).strip()
            continue

    # Reconstruct version_raw for standalone vX.Y.Z versions
    if standalone_version_seen and version:
        ver_parts = [f"v{version}"]
        if extra:
            ver_parts.append(f"({extra})")
        version_raw = ' '.join(ver_parts) if len(ver_parts) > 1 else ver_parts[0]

    return {
        'version': version,
        'extra': extra,
        'build_date': build_date,
        'version_raw': version_raw,
        'size': analyzed_info.get('size', 0),
        'compressed': analyzed_info.get('compressed', False),
        'compressed_size': analyzed_info.get('compressed_size', 0),
    }


def extract_bl2_info(analyzed_info):
    """Extract version / build-date from BL2 analysis result.

    Mirrors the logic used by bl2's print_summary.
    """
    build_strs = analyzed_info.get('build_strings') or analyzed_info.get('stage1_strings') or []
    version = None
    build_date = None
    commit = None
    extra_info = None

    for _, s in build_strs:
        # Classic: v2.3():abc or v2.1(release):commit
        m = re.search(r'v(\d+\.\d+)\(([^)]*)\):?\s*([0-9a-f]+)?', s)
        if m:
            version = m.group(1)
            if m.group(2):
                extra_info = m.group(2).strip()
            if m.group(3):
                commit = m.group(3)
            continue

        m = re.search(r'v(\d+\.\d+):\s*([0-9a-f]+)', s)
        if m:
            version = m.group(1)
            commit = m.group(2)
            continue

        # Standalone 3-part version: v2.10.0
        m = re.search(r'v(\d+\.\d+\.\d+)$', s)
        if m:
            version = m.group(1)
            continue

        # Standalone (release):commit or ():commit
        m = re.search(r'^\(([^)]*)\):?\s*([0-9a-f]+)?', s)
        if m:
            if m.group(1):
                extra_info = m.group(1).strip()
            if m.group(2):
                commit = m.group(2)
            continue

        m = re.search(r'Built\s*:\s*(.*)', s)
        if m:
            build_date = m.group(1).strip()
            continue

    # DRAMC version
    dram_strs = analyzed_info.get('dram_strings') or []
    dramc_ver = None
    dram_type_strs = []
    for _, s in dram_strs:
        if 'DRAMC' in s and 'V' in s:
            if dramc_ver is None:
                dramc_ver = s
        elif 'dram_type' in s:
            dram_type_strs.append(s)

    return {
        'version': version,
        'commit': commit,
        'extra_info': extra_info,
        'build_date': build_date,
        'dramc_ver': dramc_ver,
        'dram_types': dram_type_strs,
        'chip': analyzed_info.get('chip', 'unknown'),
        'mode': analyzed_info.get('mode', 'unknown'),
        'size': analyzed_info.get('size', 0),
    }


# ---------------------------------------------------------------------------
# File scanning
# ---------------------------------------------------------------------------

def scan_binaries(build_airoha_dir):
    """Scan both release groups for bl2.bin and bl31.bin / bl31.lzma files.

    Returns a dict keyed by release group name ('legacy' / 'opensource'); each
    value is a (bl2_files, bl31_files) tuple.  BL31 is always pre-compressed
    in this layout (bl31.lzma); the uncompressed .bin fallback is kept for
    robustness in case a raw blob ever appears.
    """
    result = {}
    for group in RELEASE_GROUPS:
        group_dir = os.path.join(build_airoha_dir, group)
        bl2_files = sorted(glob.glob(os.path.join(group_dir, '**', 'bl2.bin'), recursive=True))
        # For BL31, prefer the uncompressed .bin; fall back to .lzma if no .bin exists
        bl31_candidates = {}
        for f in sorted(glob.glob(os.path.join(group_dir, '**', 'bl31.*'), recursive=True)):
            if f.endswith('.lzma') or f.endswith('.bin'):
                key = os.path.dirname(f)
                if f.endswith('.bin'):
                    bl31_candidates[key] = f  # .bin takes priority
                elif key not in bl31_candidates:
                    bl31_candidates[key] = f
        result[group] = (bl2_files, sorted(bl31_candidates.values()))
    return result


def variant_of(filepath, build_airoha_dir):
    """Return the SoC/variant name (e.g. 'an7581_default') for a blob file."""
    rel = os.path.relpath(filepath, build_airoha_dir)
    parts = rel.split(os.sep)
    if len(parts) >= 2 and parts[0] in RELEASE_GROUPS:
        return parts[1]
    return os.path.dirname(rel) or os.path.basename(os.path.dirname(filepath))


# ---------------------------------------------------------------------------
# Per-file analysis helpers
# ---------------------------------------------------------------------------

def compute_hashes(data):
    """Compute MD5 / SHA256 / CRC32 hashes for binary data."""
    return {
        'md5': hashlib.md5(data).hexdigest(),
        'sha256': hashlib.sha256(data).hexdigest(),
        'crc32': '0x%08X' % (zlib.crc32(data) & 0xFFFFFFFF),
    }


def analyze_bl2_file(filepath):
    """Analyze a single BL2 file: metadata + hashes + structural validity.

    Validity checks:
      - file length >= BL2_MIN_SIZE (16 KB)
      - layout can be detected (struct / lzma mode)
      - struct mode: build/version strings found (valid header region)
      - lzma mode (BL2 = stage-1 BL21 + opt header + BL22 + BL23 + flash
        table + trailing CRC):
          * BL22 / BL23 / flash-table LZMA streams all decompress cleanly
          * layout is continuous (no gap) and ends exactly at file_size - 4
          * trailing CRC32 (no-final-XOR, covers whole image) matches
          * stage-1 (BL21) region is non-empty (not all zero/0xFF)
    """
    checks = []
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        hashes = compute_hashes(data)
        if len(data) < BL2_MIN_SIZE:
            return {'filepath': filepath, 'error': f'File too small ({len(data)} bytes < 0x{BL2_MIN_SIZE:x})',
                    'hashes': hashes, 'status': 'INVALID',
                    'checks': [f'FAIL: size {len(data)} bytes < min 0x{BL2_MIN_SIZE:x}']}
        checks.append(f'OK: file size {len(data)} bytes (>= 0x{BL2_MIN_SIZE:x})')

        mode, opt_hdr = bl2_tool.detect_mode(data)
        if mode == 'struct':
            info = bl2_tool.analyze_struct(data, filepath)
            n_str = len(info.get('build_strings') or [])
            if n_str:
                checks.append(f'OK: struct layout parsed, {n_str} build/version strings')
            else:
                checks.append('WARN: struct layout parsed but no build/version strings')
        else:
            info = bl2_tool.analyze_lzma(data, filepath, opt_hdr)
            if info.get('bl22_error'):
                checks.append(f'FAIL: BL22 LZMA decompression error: {info["bl22_error"]}')
            else:
                dsize = info.get('bl22_decomp_size') or info.get('size') or 0
                checks.append(f'OK: BL22 stream decompressed ({dsize} bytes)')

            if info.get('bl23_error'):
                checks.append(f'FAIL: BL23 LZMA decompression error: {info["bl23_error"]}')
            else:
                checks.append(f'OK: BL23 stream decompressed ({info.get("bl23_decomp_size", 0)} bytes)')

            if info.get('ft_error'):
                checks.append(f'FAIL: flash table LZMA error: {info["ft_error"]}')
            else:
                checks.append(f'OK: flash table stream decompressed '
                              f'({info.get("ft_decomp_size", 0)} bytes, {info.get("ft_entries", 0)} entries)')

            if info.get('layout_ok'):
                checks.append('OK: layout continuous (stage1+hdr+BL22+BL23+ft ends at file_size-4)')
            else:
                checks.append(f'FAIL: layout not continuous (ft ends at 0x{info.get("layout_end", 0):x}, '
                              f'expected 0x{len(data) - 4:x})')

            if info.get('crc_ok'):
                checks.append(f'OK: trailing CRC32 (no-final-XOR) matches ({info.get("crc_calc")})')
            else:
                checks.append(f'FAIL: trailing CRC32 mismatch (stored {info.get("crc_stored")}, '
                              f'calc {info.get("crc_calc")})')

            if info.get('stage1_ok'):
                checks.append(f'OK: stage-1 BL21 region non-empty '
                              f'({info.get("stage1_nonzero", 0)}/{info.get("stage1_size", 0)} non-zero bytes)')
            else:
                checks.append('FAIL: stage-1 BL21 region empty or all-zero/0xFF')

        extracted = extract_bl2_info(info)
        extracted['filepath'] = filepath
        extracted['error'] = None
        extracted['hashes'] = hashes
        extracted['checks'] = checks
        extracted['status'] = 'INVALID' if any(c.startswith('FAIL') for c in checks) else 'VALID'
        return extracted
    except Exception as e:
        return {'filepath': filepath, 'error': str(e)}


def analyze_bl31_file(filepath):
    """Analyze a single BL31 file: metadata + hashes + decompressibility.

    Validity checks:
      - file length >= BL31_MIN_SIZE (256 B)
      - LZMA variant: the .lzma stream must decompress cleanly (props/header
        valid, output > 0); the decompressed payload hash is also reported
      - raw variant: layout parsed with build/version strings present
    """
    checks = []
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        hashes = compute_hashes(data)
        if len(data) < BL31_MIN_SIZE:
            return {'filepath': filepath, 'error': f'File too small ({len(data)} bytes < 0x{BL31_MIN_SIZE:x})',
                    'hashes': hashes, 'status': 'INVALID',
                    'checks': [f'FAIL: size {len(data)} bytes < min 0x{BL31_MIN_SIZE:x}']}
        checks.append(f'OK: file size {len(data)} bytes (>= 0x{BL31_MIN_SIZE:x})')

        info = bl31_tool.analyze_bl31(data, filepath)
        if info.get('error'):
            return {'filepath': filepath, 'error': info['error'], 'hashes': hashes,
                    'status': 'INVALID', 'checks': checks + [f'FAIL: {info["error"]}']}
        extracted = extract_bl31_info(info)
        extracted['filepath'] = filepath
        extracted['error'] = None
        extracted['hashes'] = hashes

        if info['compressed']:
            dec = bl31_tool.decompress_lzma_alone(data)
            if dec is None:
                extracted['status'] = 'INVALID'
                extracted['checks'] = checks + ['FAIL: LZMA decompression failed']
            else:
                extracted['decomp_hashes'] = compute_hashes(dec)
                extracted['status'] = 'VALID'
                extracted['checks'] = checks + [
                    f'OK: LZMA stream (props=0x5D) decompressed ({len(dec)} bytes)',
                ]
        else:
            n_str = len(info.get('build_strings') or [])
            extracted['status'] = 'VALID'
            extracted['checks'] = checks + [
                'OK: raw binary layout parsed',
                f'OK: {n_str} build/version strings' if n_str else 'WARN: no build/version strings',
            ]
        return extracted
    except Exception as e:
        return {'filepath': filepath, 'error': str(e)}


# ---------------------------------------------------------------------------
# Log formatting
# ---------------------------------------------------------------------------

BORDER = "=" * 120
SUB_BORDER = "-" * 120


def format_size(n):
    return f"{n} bytes (0x{n:x})"


def _bl2_row(r, build_airoha_dir):
    """Format one BL2 overview-table row; returns (variant, line)."""
    variant = variant_of(r['filepath'], build_airoha_dir)
    status = r.get('status') or ('ERROR' if r.get('error') else 'N/A')
    if r.get('error'):
        return variant, f"  {variant:<26} {'ERROR':<14} {r['error'][:22]:<24} {'-':<22} {'-':<8} {'-':<20} {status:<8}"
    version = f"v{r['version']}" if r['version'] else "N/A"
    bl2_ver_parts = []
    if r.get('extra_info'):
        bl2_ver_parts.append(r['extra_info'])
    if r.get('commit'):
        bl2_ver_parts.append(r['commit'])
    if bl2_ver_parts:
        version += "(" + ",".join(bl2_ver_parts) + ")"
    build_date = r['build_date'] or "N/A"
    dramc = r['dramc_ver'] or "N/A"
    mode = r['mode']
    size = format_size(r['size'])
    return variant, f"  {variant:<26} {version:<14} {build_date:<24} {dramc:<22} {mode:<8} {size:<20} {status:<8}"


def _bl31_row(r, build_airoha_dir):
    """Format one BL31 overview-table row; returns (variant, line)."""
    variant = variant_of(r['filepath'], build_airoha_dir)
    status = r.get('status') or ('ERROR' if r.get('error') else 'N/A')
    if r.get('error'):
        return variant, f"  {variant:<26} {'ERROR':<28} {r['error'][:22]:<24} {'-':<14} {'-':<24} {status:<8}"
    if r.get('version_raw'):
        version = r['version_raw']
    elif r['version']:
        version = f"v{r['version']}"
        if r.get('extra'):
            version += f"({r['extra']})"
    else:
        version = "N/A"
    build_date = r['build_date'] or "N/A"
    if r['compressed']:
        fmt = "LZMA compressed"
        size = f"{format_size(r['compressed_size'])} -> {format_size(r['size'])}"
    else:
        fmt = "Raw binary"
        size = format_size(r['size'])
    return variant, f"  {variant:<26} {version:<28} {build_date:<24} {fmt:<14} {size:<24} {status:<8}"


def generate_log(scanned, build_airoha_dir):
    """Build the full log text.

    scanned is the dict returned by scan_binaries(): group name ->
    (bl2_results, bl31_results).
    """
    lines = []

    # Header
    lines.append(BORDER)
    lines.append("  Airoha SoC BL2/BL31 Firmware Build Information Log")
    lines.append(BORDER)
    lines.append(f"  Generated:    {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"  Scanned dir:  {os.path.relpath(build_airoha_dir, os.path.dirname(TOOLS_DIR))}")
    total_bl2 = sum(len(v[0]) for v in scanned.values())
    total_bl31 = sum(len(v[1]) for v in scanned.values())
    counts = ", ".join(f"{g}: {len(v[0])}/{len(v[1])}" for g, v in scanned.items())
    lines.append(f"  BL2 files:    {total_bl2}   BL31 files: {total_bl31}   ({counts})")
    lines.append("")

    # ================================================================
    # Overview tables - per release group, BL2 then BL31
    # ================================================================
    for group in RELEASE_GROUPS:
        if group not in scanned:
            continue
        bl2_results, bl31_results = scanned[group]
        label = GROUP_LABELS.get(group, group)

        # BL2 overview
        lines.append(BORDER)
        lines.append(f"  [OVERVIEW] BL2 (Preloader) Build Information - {label}")
        lines.append(BORDER)
        header = f"  {'Variant':<26} {'Version':<14} {'Build Date':<24} {'DRAMC Ver':<22} {'Mode':<8} {'File Size':<20} {'Status':<8}"
        lines.append(header)
        lines.append("  " + "-" * (len(header) - 2))
        rows = [_bl2_row(r, build_airoha_dir) for r in bl2_results]
        for _, line in sorted(rows):
            lines.append(line)
        lines.append("")

        # BL31 overview
        lines.append(BORDER)
        lines.append(f"  [OVERVIEW] BL31 (ARM Trusted Firmware) Build Information - {label}")
        lines.append(BORDER)
        header = f"  {'Variant':<26} {'Version':<28} {'Build Date':<24} {'Format':<14} {'File Size':<24} {'Status':<8}"
        lines.append(header)
        lines.append("  " + "-" * (len(header) - 2))
        rows = [_bl31_row(r, build_airoha_dir) for r in bl31_results]
        for _, line in sorted(rows):
            lines.append(line)
        lines.append("")

    # ================================================================
    # Detail - grouped by release group, then by variant
    # ================================================================
    lines.append(BORDER)
    lines.append("  [DETAIL] Grouped by Release Group and Platform")
    lines.append(BORDER)
    lines.append("")

    for group in RELEASE_GROUPS:
        if group not in scanned:
            continue
        bl2_results, bl31_results = scanned[group]
        label = GROUP_LABELS.get(group, group)

        lines.append(SUB_BORDER)
        lines.append(f"  Release Group: {label} ({group}/)")
        lines.append(SUB_BORDER)
        lines.append("")

        # Collect all variants within this group
        all_variants = set()
        bl2_by_var = {}
        bl31_by_var = {}
        for r in bl2_results:
            var = variant_of(r['filepath'], build_airoha_dir)
            all_variants.add(var)
            bl2_by_var.setdefault(var, []).append(r)
        for r in bl31_results:
            var = variant_of(r['filepath'], build_airoha_dir)
            all_variants.add(var)
            bl31_by_var.setdefault(var, []).append(r)

        for var in sorted(all_variants):
            lines.append(SUB_BORDER)
            lines.append(f"  Platform: {var}")
            lines.append(SUB_BORDER)
            lines.append("")

            # BL2
            if var in bl2_by_var:
                for r in bl2_by_var[var]:
                    lines.append(f"  --- BL2: {os.path.basename(r['filepath'])} ---")
                    lines.append(f"    File path:  {os.path.relpath(r['filepath'], build_airoha_dir)}")
                    if r.get('error'):
                        lines.append(f"    Error:      {r['error']}")
                    else:
                        if r['version']:
                            v = f"v{r['version']}"
                            bl2_ver_parts = []
                            if r.get('extra_info'):
                                bl2_ver_parts.append(r['extra_info'])
                            if r.get('commit'):
                                bl2_ver_parts.append(f"commit: {r['commit']}")
                            if bl2_ver_parts:
                                v += " (" + ", ".join(bl2_ver_parts) + ")"
                            lines.append(f"    Version:    {v}")
                        else:
                            lines.append(f"    Version:    N/A")
                        lines.append(f"    Build Date: {r['build_date'] or 'N/A'}")
                        lines.append(f"    Detected Chip: {r['chip']}")
                        lines.append(f"    Mode:       {r['mode']}")
                        lines.append(f"    File Size:  {format_size(r['size'])}")
                        if r['dramc_ver']:
                            lines.append(f"    DRAMC:      {r['dramc_ver']}")
                        if r['dram_types']:
                            lines.append(f"    Supported DRAM Types:")
                            for dt in r['dram_types']:
                                lines.append(f"      - {dt}")
                        if r.get('hashes'):
                            lines.append(f"    Hashes:")
                            lines.append(f"      MD5:    {r['hashes']['md5']}")
                            lines.append(f"      SHA256: {r['hashes']['sha256']}")
                            lines.append(f"      CRC32:  {r['hashes']['crc32']}")
                        if r.get('checks'):
                            lines.append(f"    Validation:")
                            for c in r['checks']:
                                lines.append(f"      {c}")
                        if r.get('status'):
                            lines.append(f"    Status:     {r['status']}")
                    lines.append("")

            # BL31
            if var in bl31_by_var:
                for r in bl31_by_var[var]:
                    lines.append(f"  --- BL31: {os.path.basename(r['filepath'])} ---")
                    lines.append(f"    File path:  {os.path.relpath(r['filepath'], build_airoha_dir)}")
                    if r.get('error'):
                        lines.append(f"    Error:      {r['error']}")
                    else:
                        if r.get('version_raw'):
                            lines.append(f"    Version:    {r['version_raw']}")
                        elif r['version']:
                            v = f"v{r['version']}"
                            if r.get('extra'):
                                v += f" ({r['extra']})"
                            lines.append(f"    Version:    {v}")
                        else:
                            lines.append(f"    Version:    N/A")
                        lines.append(f"    Build Date: {r['build_date'] or 'N/A'}")
                        if r['compressed']:
                            lines.append(f"    Format:     LZMA compressed")
                            lines.append(f"    Compressed: {format_size(r['compressed_size'])}")
                            lines.append(f"    Decompressed: {format_size(r['size'])}")
                        else:
                            lines.append(f"    Format:     Raw binary")
                            lines.append(f"    File Size:  {format_size(r['size'])}")
                        if r.get('hashes'):
                            lines.append(f"    Hashes:")
                            lines.append(f"      MD5:    {r['hashes']['md5']}")
                            lines.append(f"      SHA256: {r['hashes']['sha256']}")
                            lines.append(f"      CRC32:  {r['hashes']['crc32']}")
                        if r.get('decomp_hashes'):
                            lines.append(f"    Decompressed Hashes:")
                            lines.append(f"      MD5:    {r['decomp_hashes']['md5']}")
                            lines.append(f"      SHA256: {r['decomp_hashes']['sha256']}")
                            lines.append(f"      CRC32:  {r['decomp_hashes']['crc32']}")
                        if r.get('checks'):
                            lines.append(f"    Validation:")
                            for c in r['checks']:
                                lines.append(f"      {c}")
                        if r.get('status'):
                            lines.append(f"    Status:     {r['status']}")
                    lines.append("")

    lines.append(BORDER)
    lines.append("  End of Log")
    lines.append(BORDER)
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    build_airoha_dir = os.path.dirname(os.path.abspath(__file__))
    output_file = os.path.join(build_airoha_dir, 'firmware_build_log.txt')

    print(f"[*] Scanning directory: {build_airoha_dir}")
    scanned = scan_binaries(build_airoha_dir)
    for group in RELEASE_GROUPS:
        bl2_files, bl31_files = scanned.get(group, ([], []))
        print(f"[*] [{group}/] Found BL2 files: {len(bl2_files)}, BL31 files: {len(bl31_files)}")

    # Analyze per group
    scanned_results = {}
    for group in RELEASE_GROUPS:
        bl2_files, bl31_files = scanned.get(group, ([], []))

        print(f"\n[*] [{group}/] Analyzing BL2 files...")
        bl2_results = []
        for f in bl2_files:
            print(f"  - {os.path.relpath(f, build_airoha_dir)} ... ", end="", flush=True)
            r = analyze_bl2_file(f)
            bl2_results.append(r)
            if r.get('error'):
                print(f"FAILED: {r['error']}")
            else:
                v = f"v{r['version']}" if r['version'] else "N/A"
                print(f"OK (version: {v}, date: {r['build_date'] or 'N/A'}, status: {r.get('status')})")

        print(f"\n[*] [{group}/] Analyzing BL31 files...")
        bl31_results = []
        for f in bl31_files:
            print(f"  - {os.path.relpath(f, build_airoha_dir)} ... ", end="", flush=True)
            r = analyze_bl31_file(f)
            bl31_results.append(r)
            if r.get('error'):
                print(f"FAILED: {r['error']}")
            else:
                if r.get('version_raw'):
                    v = r['version_raw']
                elif r['version']:
                    v = f"v{r['version']}"
                else:
                    v = "N/A"
                print(f"OK (version: {v}, date: {r['build_date'] or 'N/A'}, status: {r.get('status')})")

        scanned_results[group] = (bl2_results, bl31_results)

    # Generate log
    print("\n[*] Generating log...")
    log_content = generate_log(scanned_results, build_airoha_dir)

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(log_content)

    print(f"[✓] Log written to: {output_file}")
    print(f"    Log size: {os.path.getsize(output_file)} bytes")


if __name__ == '__main__':
    main()
