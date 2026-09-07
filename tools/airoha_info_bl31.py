#!/usr/bin/env python3
"""
airoha_info_bl31.py - Extract BL31 (ARM Trusted Firmware) Metadata from Airoha SoC Binaries

Supports both uncompressed (.bin) and LZMA-compressed (.lzma) BL31 images.

Usage:
  python3 airoha_info_bl31.py <bl31.bin>               # show all info
  python3 airoha_info_bl31.py <bl31.lzma>               # auto-decompress then show info
  python3 airoha_info_bl31.py <bl31.bin> --summary      # compact summary
  python3 airoha_info_bl31.py <bl31.bin> --raw          # dump all strings
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile


# ---------------------------------------------------------------------------
# LZMA decompression
# ---------------------------------------------------------------------------

def is_lzma_data(data):
    """Check if data looks like an LZMA-Alone stream."""
    if len(data) < 13:
        return False
    if data[0] != 0x5d:
        return False
    dict_sz = struct.unpack_from('<I', data, 1)[0]
    return dict_sz <= 0x800000


def decompress_lzma_alone(data):
    """Decompress an LZMA-Alone stream. Returns decompressed bytes or None."""
    with tempfile.NamedTemporaryFile(suffix='.lzma', delete=False) as tf:
        tf.write(data)
        tmp_path = tf.name
    try:
        result = subprocess.run(
            ['xz', '--format=lzma', '-d', '-c', tmp_path],
            capture_output=True,
        )
        # xz may return non-zero with valid data on integrity warnings;
        # accept any non-empty output as success.
        if result.stdout and len(result.stdout) > 0x100:
            return result.stdout
        return None
    finally:
        os.unlink(tmp_path)


# ---------------------------------------------------------------------------
# String extraction
# ---------------------------------------------------------------------------

def extract_strings(data, min_len=4):
    """Yield (offset, string) from binary data."""
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % min_len, data):
        try:
            yield (m.start(), m.group().decode('ascii'))
        except UnicodeDecodeError:
            pass


def find_strings(data, patterns, min_len=4):
    """Return list of (offset, string) matching any of the given regex patterns."""
    results = []
    for off, s in extract_strings(data, min_len):
        for pat in patterns:
            if re.search(pat, s):
                results.append((off, s))
                break
    return results


# ---------------------------------------------------------------------------
# BL31 analysis
# ---------------------------------------------------------------------------

def analyze_bl31(data, filename):
    """Extract metadata from BL31 binary."""
    results = {
        'filename': filename,
        'size': len(data),
        'compressed': False,
    }

    # Check if data is LZMA compressed
    if is_lzma_data(data):
        results['compressed'] = True
        results['compressed_size'] = len(data)
        decompressed = decompress_lzma_alone(data)
        if decompressed:
            results['size'] = len(decompressed)
            data = decompressed
        else:
            results['error'] = 'LZMA decompression failed'
            return results

    # Extract build info - support vX.Y classic, vX.Y.Z standalone, and adjacent (tag):commit
    build_pats = [r'Built\s*:', r'v\d+\.\d+\(', r'v\d+\.\d+:',
                  r'v\d+\.\d+\.\d+', r'^\(.*\):[0-9a-f]+']

    # Also extract notable BL31 strings
    bl31_pats = [
        r'BL31:', r'NOTICE:', r'ERROR:', r'WARNING:',
        r'ATF', r'trusted', r'runtime service', r'ROTPK',
    ]

    results['build_strings'] = find_strings(data, build_pats)
    results['bl31_strings'] = find_strings(data, bl31_pats)

    return results


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

BORDER = "=" * 60


def print_build_info(info):
    """Print BL31 build/version information."""
    print(BORDER)
    print("  BL31 BUILD INFO")
    print(BORDER)

    build_strs = info.get('build_strings') or []
    version = None
    extra = None
    build_date = None

    for _, s in build_strs:
        # v2.1(release):<commit> or v2.3():<commit> (classic 2-part version)
        m = re.search(r'v(\d+\.\d+)\(([^)]*)\):?\s*([0-9a-fA-F].*)?', s)
        if m:
            version = m.group(1)
            paren_content = m.group(2).strip() if m.group(2) else None
            if m.group(3):
                # combine paren content + trailing commit info
                extra_parts = []
                if paren_content:
                    extra_parts.append(paren_content)
                tail = m.group(3).strip()
                if tail:
                    extra_parts.append(tail)
                extra = ':'.join(extra_parts) if extra_parts else None
            elif paren_content:
                extra = paren_content
            continue

        m = re.search(r'v(\d+\.\d+):\s*(.*)', s)
        if m:
            version = m.group(1)
            extra = m.group(2).strip()
            continue

        # Standalone 3-part version: v2.10.0
        m = re.search(r'v(\d+\.\d+\.\d+)$', s)
        if m:
            version = m.group(1)
            continue

        # Standalone (xxx):commit following a standalone vX.Y.Z
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

    if version:
        line = f"  BL31 Version:      v{version}"
        if extra:
            line += f" ({extra})"
        print(line)
    else:
        print(f"  BL31 Version:      N/A")

    if build_date:
        print(f"  Build Date:        {build_date}")
    else:
        print(f"  Build Date:        N/A")


def print_summary(info):
    """Compact summary output."""
    print(BORDER)
    print("  BL31 Summary")
    print(BORDER)

    build_strs = info.get('build_strings') or []
    version = None
    extra = None
    build_date = None
    version_raw = None
    standalone_version_seen = False

    for _, s in build_strs:
        # v2.1(release):<commit> or v2.3():<commit> (classic combined)
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
            version_raw = None  # will be reconstructed below
            continue

        # Standalone (xxx):commit following a standalone vX.Y.Z
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

        # Build date
        m = re.search(r'Built\s*:\s*(.*)', s)
        if m:
            build_date = m.group(1).strip()
            continue

    if standalone_version_seen and version:
        # Reconstruct version_raw string
        ver_parts = [f"v{version}"]
        if extra:
            ver_parts.append(f"({extra})")
        version_raw = ' '.join(ver_parts) if len(ver_parts) > 1 else ver_parts[0]

    if version_raw:
        print(f"  Version:           {version_raw}")
    elif version:
        print(f"  Version:           v{version}")
    else:
        print(f"  Version:           N/A")

    if build_date:
        print(f"  Build Date:        {build_date}")
    else:
        print(f"  Build Date:        N/A")


def print_general_info(info):
    """Print general file information."""
    print(BORDER)
    print("  BL31 FILE INFO")
    print(BORDER)
    print(f"  Filename:          {os.path.basename(info['filename'])}")
    if info.get('compressed'):
        print(f"  Format:            LZMA compressed")
        print(f"  Compressed Size:   {info['compressed_size']} bytes (0x{info['compressed_size']:x})")
        print(f"  Decompressed Size: {info['size']} bytes (0x{info['size']:x})")
    else:
        print(f"  Format:            Raw binary")
        print(f"  File Size:         {info['size']} bytes (0x{info['size']:x})")


def print_raw_strings(info):
    """Dump all extracted strings."""
    build_strs = info.get('build_strings') or []
    bl31_strs = info.get('bl31_strings') or []

    print(BORDER)
    print("  BUILD STRINGS")
    print(BORDER)
    for off, s in build_strs:
        print(f"  0x{off:05x}: {s}")

    if bl31_strs:
        print()
        print(BORDER)
        print("  BL31 STRINGS")
        print(BORDER)
        for off, s in bl31_strs:
            print(f"  0x{off:05x}: {s}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description='Extract BL31 metadata from Airoha SoC binaries',
    )
    parser.add_argument('bl31_bin', help='Path to BL31 binary (.bin or .lzma)')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--all', action='store_true', default=True,
                       help='Show all available info (default)')
    group.add_argument('--summary', action='store_true',
                       help='Show compact summary')
    group.add_argument('--build', action='store_true',
                       help='Show only build/version info')
    group.add_argument('--raw', action='store_true',
                       help='Dump all extracted strings')
    return parser.parse_args()


def main():
    args = parse_args()

    if not os.path.isfile(args.bl31_bin):
        print(f"ERROR: file not found: {args.bl31_bin}", file=sys.stderr)
        sys.exit(1)

    with open(args.bl31_bin, 'rb') as f:
        data = f.read()

    if len(data) < 256:
        print(f"ERROR: file too small ({len(data)} bytes), not a valid BL31 binary",
              file=sys.stderr)
        sys.exit(1)

    info = analyze_bl31(data, args.bl31_bin)

    if info.get('error'):
        print(f"ERROR: {info['error']}", file=sys.stderr)
        sys.exit(1)

    if args.raw:
        print_raw_strings(info)
    elif args.build:
        print_build_info(info)
    elif args.summary:
        print_summary(info)
    else:
        # --all: show everything
        print_general_info(info)
        print_build_info(info)

    print()


if __name__ == '__main__':
    main()
