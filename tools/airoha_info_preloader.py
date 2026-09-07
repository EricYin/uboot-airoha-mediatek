#!/usr/bin/env python3
"""
airoha_info_preloader.py - Extract BL2 Preloader Metadata from Airoha SoC Binaries

Supports two binary modes:
  struct mode: en7523, en7562 — flash table is a compile-time struct array,
               all strings are plain ASCII in the binary.
  LZMA mode:   an7563, an7581, an7583 — preloader data is LZMA-compressed,
               requires decompression to extract DRAM/version info.

Binary Layout (LZMA mode):
  [Stage-1 preloader (raw ARM)]    @ 0x0000
  [Optimization Header]            @ 0x3800 (8 or 9 u32)
  [BL22 LZMA stream]               @ header_end
  [BL23 LZMA stream]               @ header_end + bl22_csz
  [Flash Table LZMA stream]        @ header_end + bl22_csz + bl23_csz
  [CRC32 (4 bytes)]                @ EOF - 4

Usage:
  python3 airoha_info_preloader.py <bl2.bin>              # show all info
  python3 airoha_info_preloader.py <bl2.bin> --dram       # DRAM info only
  python3 airoha_info_preloader.py <bl2.bin> --build      # build info only
  python3 airoha_info_preloader.py <bl2.bin> --flash      # flash table only
  python3 airoha_info_preloader.py <bl2.bin> --all        # full detail
  python3 airoha_info_preloader.py <bl2.bin> --raw        # dump all strings
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

# ---------------------------------------------------------------------------
# Utility: ASCII string extraction
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
# LZMA decompression
# ---------------------------------------------------------------------------

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
        if result.returncode == 0:
            return result.stdout
        return None
    finally:
        os.unlink(tmp_path)


def find_lzma_streams(data, start=0x2000):
    """Find all LZMA-Alone streams in binary data.
    LZMA-Alone signature: 0x5d, dict_size=0x800000, uncomp_size != 0.
    Returns list of (offset, compressed_size, uncompressed_size).
    """
    streams = []
    i = start
    while i < len(data) - 13:
        if data[i] == 0x5d:
            dict_sz = struct.unpack_from('<I', data, i + 1)[0]
            uncomp = struct.unpack_from('<Q', data, i + 5)[0] & 0xFFFFFFFFFFFF  # 6 bytes
            if dict_sz == 0x800000 and 0 < uncomp < 0x400000 and uncomp != 0xFFFFFFFFFFFF:
                streams.append((i, uncomp))
        i += 1
    return streams


# ---------------------------------------------------------------------------
# Optimization header parsing (LZMA mode)
# ---------------------------------------------------------------------------

def find_opt_header(data):
    """Find optimization header in LZMA-mode BL2 binary.
    
    The header is at fixed offset 0x3800. Header can be 8 u32 (0x20 bytes)
    or 9 u32 (0x24 bytes). Detect by checking where the LZMA stream starts.
    
    Returns dict with:
      'offset': header offset in file
      'size': header size in bytes (0x20 or 0x24)
      'bl22_csz': total compressed size of BL22 LZMA stream
      'bl23_csz': total compressed size of BL23 LZMA stream
      'ft_csz': total compressed size of flash table LZMA stream
      'va_base': virtual address base for BL22
      'fields': raw 9-u32 list from header location
    or None if no valid header found.
    """
    # Read up to 9 u32 from 0x3800
    if len(data) < 0x3800 + 0x24:
        return None

    fields = struct.unpack_from('<9I', data, 0x3800)
    
    bl22_csz = fields[0]
    bl23_csz = fields[1]
    ft_csz = fields[2]
    va_base = fields[4]  # typically 0x08004000

    # Validate fields
    if not (0x1000 < bl22_csz < 0x200000):
        return None
    if not (0x1000 < bl23_csz < 0x200000):
        return None
    if not (0x100 < ft_csz < 0x20000):
        return None

    # Determine header size: check if LZMA starts at 0x3820 (8-u32) or 0x3824 (9-u32)
    header_size = 0x20  # default: 8 u32
    lzma_start = 0x3820

    # Check if byte at 0x3820 is valid LZMA property (most common: 0x5d)
    if data[0x3820] not in (0x5d, 0x6d, 0x7d, 0x5c, 0x6c, 0x7c):
        # Not a valid LZMA property at 0x3820, try 0x3824
        if data[0x3824] in (0x5d, 0x6d, 0x7d, 0x5c, 0x6c, 0x7c, 0x3d, 0x4d):
            header_size = 0x24
            lzma_start = 0x3824

    # Verify LZMA header at chosen offset
    if lzma_start + 13 <= len(data):
        props = data[lzma_start]
        dict_sz = struct.unpack_from('<I', data, lzma_start + 1)[0]
        if not (dict_sz <= 0x800000 and props <= 0xE0):
            # Fall back to the other offset
            if header_size == 0x20:
                header_size = 0x24
                lzma_start = 0x3824
            else:
                header_size = 0x20
                lzma_start = 0x3820

    return {
        'offset': 0x3800,
        'size': header_size,
        'bl22_csz': bl22_csz,
        'bl23_csz': bl23_csz,
        'ft_csz': ft_csz,
        'va_base': va_base,
        'fields': fields,
        'lzma_start': lzma_start,
    }


# ---------------------------------------------------------------------------
# Mode detection
# ---------------------------------------------------------------------------

def detect_mode(data):
    """Return 'lzma' or 'struct' based on binary content."""
    hdr = find_opt_header(data)
    if hdr is not None:
        return 'lzma', hdr
    return 'struct', None


# ---------------------------------------------------------------------------
# Struct mode analysis
# ---------------------------------------------------------------------------

def analyze_struct(data, filename):
    """Extract metadata from struct-mode BL2 (en7523/en7562)."""
    results = {'filename': filename, 'mode': 'struct', 'size': len(data)}

    # Build info patterns - also match standalone version strings like v2.10.0
    build_pats = [r'Built\s*:', r'v\d+\.\d+\(', r'BL2:', r'VERSION',
                  r'v\d+\.\d+\.\d+', r'^\(.*\):[0-9a-f]+']
    results['build_strings'] = find_strings(data, build_pats)

    # DRAM patterns
    dram_pats = [
        r'DRAMC\s+V\d+\.\d+', r'dram_type', r'DDR\d+\s+PLL',
        r'\[Dramc\]', r'PCDDR', r'DDR\d+', r'Not support this DDR',
        r'dram r/w error', r'MPLL', r'bus2dram',
        r'Package ID', r'Unknow',
        r'DDR\d+\s+reserved',
    ]
    results['dram_strings'] = find_strings(data, dram_pats)

    # Flash detection: look for SPI-NAND device IDs
    flash_pats = [r'SPI_NAND_DEVICE_ID_', r'GD5F', r'W25N', r'MX35', r'F50L', r'TC58']
    results['flash_strings'] = find_strings(data, flash_pats)

    # Chip identification
    if 'en7523' in filename.lower():
        results['chip'] = 'en7523'
    elif 'en7562' in filename.lower():
        results['chip'] = 'en7562'
    else:
        # Try to detect from strings
        for _, s in results['dram_strings']:
            m = re.search(r'(EN75\d+|AN75\d+)', s, re.IGNORECASE)
            if m:
                results['chip'] = m.group(1).lower()
                break
        else:
            results['chip'] = 'unknown'

    return results


# ---------------------------------------------------------------------------
# LZMA mode analysis
# ---------------------------------------------------------------------------

def analyze_lzma(data, filename, opt_hdr):
    """Extract metadata from LZMA-mode BL2 (an7563/an7581/an7583)."""
    results = {'filename': filename, 'mode': 'lzma', 'size': len(data)}
    results['opt_header'] = opt_hdr

    # --- Stage-1 strings (uncompressed preloader section) ---
    stage1_end = opt_hdr['offset']
    stage1_data = data[:stage1_end]
    
    build_pats = [r'Built\s*:', r'v\d+\.\d+\(', r'BL2:', r'VERSION',
                  r'v\d+\.\d+\.\d+', r'^\(.*\):[0-9a-f]+']
    results['stage1_strings'] = find_strings(stage1_data, build_pats + [r'ERROR:', r'WARNING:'])

    # --- Decompress BL22 ---
    bl22_lzma_off = opt_hdr['lzma_start']
    bl22_lzma_end = bl22_lzma_off + opt_hdr['bl22_csz']
    bl22_lzma = data[bl22_lzma_off:bl22_lzma_end]
    bl22_dec = decompress_lzma_alone(bl22_lzma)

    if bl22_dec:
        results['bl22_decomp_size'] = len(bl22_dec)
        # Build info - also match standalone vX.Y.Z and adjacent (release):commit strings
        build_pats = [r'Built\s*:', r'v\d+\.\d+\(', r'BL2:',
                      r'v\d+\.\d+\.\d+', r'^\(.*\):[0-9a-f]+']
        results['build_strings'] = find_strings(bl22_dec, build_pats)

        # DRAM info
        dram_pats = [
            r'DRAMC\s+V\d+\.\d+', r'dram_type', r'DDR\d+\s+PLL',
            r'\[Dramc\]', r'PCDDR', r'DDR\d+', r'Not support this DDR',
            r'dram r/w error', r'MPLL', r'bus2dram',
            r'Package ID', r'Unknow', r'DDR\d+\s+reserved',
            r'SSC MPLL', r'data_rate',
        ]
        results['dram_strings'] = find_strings(bl22_dec, dram_pats)
    else:
        results['bl22_error'] = 'decompression failed'

    # --- Decompress BL23 ---
    bl23_lzma_off = bl22_lzma_end
    bl23_lzma_end = bl23_lzma_off + opt_hdr['bl23_csz']
    if bl23_lzma_end > len(data):
        results['bl23_error'] = 'stream exceeds file (truncated)'
    else:
        bl23_dec = decompress_lzma_alone(data[bl23_lzma_off:bl23_lzma_end])
        if bl23_dec:
            results['bl23_decomp_size'] = len(bl23_dec)
        else:
            results['bl23_error'] = 'decompression failed'

    # --- Decompress flash table ---
    ft_lzma_off = bl23_lzma_end
    ft_lzma_end = ft_lzma_off + opt_hdr['ft_csz']
    if ft_lzma_end > len(data):
        results['ft_error'] = 'stream exceeds file (truncated)'
    else:
        ft_dec = decompress_lzma_alone(data[ft_lzma_off:ft_lzma_end])
        if ft_dec:
            results['ft_decomp_size'] = len(ft_dec)
            results['ft_entries'] = len(parse_flash_table_raw(ft_dec))
        else:
            results['ft_error'] = 'decompression failed'

    # --- Layout continuity: all segments must be back-to-back and end at EOF-4 ---
    results['layout_end'] = ft_lzma_end
    results['layout_ok'] = (ft_lzma_end == len(data) - 4)

    # --- Trailing CRC32 (no final XOR, covers data[:-4]) ---
    if len(data) >= 4:
        stored = struct.unpack_from('<I', data, len(data) - 4)[0]
        calc = zlib.crc32(data[:-4]) & 0xFFFFFFFF ^ 0xFFFFFFFF
        results['crc_stored'] = '0x%08X' % stored
        results['crc_calc'] = '0x%08X' % calc
        results['crc_ok'] = (stored == calc)
    else:
        results['crc_ok'] = False

    # --- Stage-1 (BL21) region sanity: non-empty, not all zero/0xFF ---
    s1 = data[:opt_hdr['offset']]
    nz = sum(1 for b in s1 if b not in (0, 0xFF))
    results['stage1_size'] = len(s1)
    results['stage1_nonzero'] = nz
    results['stage1_ok'] = len(s1) > 0 and nz > len(s1) // 8

    # --- Chip identification ---
    chip_map = {
        'AN7552': 'an7563', 'AN7581': 'an7581', 'AN7583': 'an7583',
        'EN7523': 'en7523', 'EN7562': 'en7562',
    }
    for _, s in (results.get('dram_strings') or []) + (results.get('build_strings') or []):
        for key, chip in chip_map.items():
            if key in s:
                results['chip'] = chip
                break
    if 'chip' not in results:
        # Try filename
        for chip in ['an7563', 'an7581', 'an7583', 'en7523', 'en7562']:
            if chip in filename.lower():
                results['chip'] = chip
                break
        else:
            results['chip'] = 'unknown'

    return results


# ---------------------------------------------------------------------------
# Flash table analysis (LZMA mode)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Binary flash table format (aligned with update_bl2_flash_table.py)
# ---------------------------------------------------------------------------
# Header: bl2_flash_H = 6 x u32 = 24 bytes
#   [0] flash_entry      – number of SPI-NAND entries
#   [1] flash_name_off   – offset to name string area in staging file
#   [2] flash_oob_off    – offset to oob layout area
#   [3] parallel_entry   – number of parallel-NAND entries
#   [4] parallel_name_off
#   [5] parallel_oob_off
#
# Entry layout (serialized by flash_table_gen::main()):
#   Offset  0: mfr_id              u8
#   Offset  1: dev_id              u8
#   Offset  2: device_size         u32
#   Offset  6: page_size           u32
#   Offset 10: erase_size          u32
#   Offset 14: oob_size            u32
#   Offset 18–52: (remaining fields, skipped by this reader)
#   TOTAL: 53 (or 54 with OTP)
#
# Name area: <u32 name_length><name_bytes...> per entry, NUL-padded

FT_HEADER_SIZE = 24

def _ft_read_names(data, name_off, count):
    """Read *count* length-prefixed name strings starting at *name_off*."""
    names = []
    off = name_off
    for _ in range(count):
        if off + 4 > len(data):
            break
        name_len = struct.unpack_from('<I', data, off)[0]
        off += 4
        if off + name_len > len(data):
            break
        raw = bytes(data[off:off + name_len])
        off += name_len
        name = raw.rstrip(b'\x00').decode('ascii', errors='replace')
        names.append(name)
    return names


def parse_flash_table_raw(data):
    """Parse a raw (decompressed) binary flash table staging file.
    Returns list of dicts: {idx, mfr_id, dev_id, name, device_mb, page_kb, erase_kb, oob_b}.
    """
    if len(data) < FT_HEADER_SIZE:
        return []

    flash_entry = struct.unpack_from('<I', data, 0)[0]
    flash_name_off = struct.unpack_from('<I', data, 4)[0]

    if flash_entry == 0:
        return []

    # Determine entry size from name offset
    entry_size = (flash_name_off - FT_HEADER_SIZE) // flash_entry
    if entry_size not in (53, 54):
        # Warn but proceed; may be a different build config
        pass

    entries = []
    skip_count = 0
    for i in range(flash_entry):
        off = FT_HEADER_SIZE + i * entry_size
        if off + 14 > len(data):
            break
        mfr = data[off]
        dev = data[off + 1]
        sz = struct.unpack_from('<I', data, off + 2)[0]
        page = struct.unpack_from('<I', data, off + 6)[0]
        erase = struct.unpack_from('<I', data, off + 10)[0]
        oob = struct.unpack_from('<I', data, off + 14)[0]

        skipped = (mfr == 0 and dev == 0 and sz == 0)
        if skipped:
            skip_count += 1
            continue

        entries.append({
            'idx': i,
            'mfr_id': mfr,
            'dev_id': dev,
            'name': '',
            'device_mb': sz // (1024 * 1024),
            'page_kb': page // 1024,
            'erase_kb': erase // 1024,
            'oob_b': oob,
            'skipped': False,
        })

    # Read names
    names = _ft_read_names(data, flash_name_off, flash_entry)
    for j, e in enumerate(entries):
        e['name'] = names[e['idx']] if e['idx'] < len(names) else '(missing)'

    return entries


def analyze_flash_table_lzma(data, opt_hdr):
    """Decompress and parse LZMA flash table."""
    ft_lzma_off = opt_hdr['lzma_start'] + opt_hdr['bl22_csz'] + opt_hdr['bl23_csz']
    ft_lzma_end = ft_lzma_off + opt_hdr['ft_csz']
    if ft_lzma_end > len(data):
        return None

    ft_lzma = data[ft_lzma_off:ft_lzma_end]
    ft_dec = decompress_lzma_alone(ft_lzma)
    if not ft_dec:
        return None

    entries = parse_flash_table_raw(ft_dec)
    return entries


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

def print_header(title):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")


def print_build_info(info):
    """Print build information from analysis results."""
    print_header("BUILD INFO")
    build_strs = info.get('build_strings') or info.get('stage1_strings') or []

    version = None
    commit = None
    extra_info = None
    build_date = None

    for _, s in build_strs:
        # v2.1(release):abc123 or v2.3():50866bc (classic 2-part version)
        m = re.search(r'v(\d+\.\d+)\(([^)]*)\):?\s*([0-9a-f]+)?', s)
        if m:
            version = m.group(1)
            if m.group(2):
                extra_info = m.group(2).strip()
            if m.group(3):
                commit = m.group(3)
            continue

        # Also match without parens: v2.3:50866bc
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

        # Standalone (release):commit or ():commit following a standalone vX.Y.Z
        m = re.search(r'^\(([^)]*)\):?\s*([0-9a-f]+)?', s)
        if m:
            if m.group(1):
                extra_info = m.group(1).strip()
            if m.group(2):
                commit = m.group(2)
            continue

        # Build date: Built : 16:15:16, Feb 20 2024
        m = re.search(r'Built\s*:\s*(.*)', s)
        if m:
            build_date = m.group(1).strip()
            continue

    if version:
        ver_str = f"v{version}"
        if extra_info:
            ver_str += f" ({extra_info})"
        print(f"  BL2 Version:       {ver_str}")
    if commit:
        print(f"  Git Commit:        {commit}")
    if build_date:
        print(f"  Build Date:        {build_date}")

    if not version and not build_date:
        print("  (no build info found)")


def print_dram_info(info):
    """Print DRAM/DDR information from analysis results."""
    print_header("DRAM / DDR INFO")
    dram_strs = info.get('dram_strings') or []

    # Group by category
    dramc_ver = []
    dram_type = []
    ddr_pll = []
    dramc_ac = []
    mpll = []
    other_ddr = []

    for _, s in dram_strs:
        if 'DRAMC' in s and 'V' in s:
            dramc_ver.append(s)
        elif 'dram_type' in s:
            dram_type.append(s)
        elif 'PLL' in s and 'DDR' in s:
            ddr_pll.append(s)
        elif '[Dramc]' in s:
            dramc_ac.append(s)
        elif 'MPLL' in s:
            mpll.append(s)
        else:
            other_ddr.append(s)

    if dramc_ver:
        print(f"  DRAMC Version:     {dramc_ver[0]}")
    if dram_type:
        for s in dram_type:
            print(f"  Supported Type:    {s}")
    if ddr_pll:
        print(f"  DDR PLL Modes:")
        for s in ddr_pll:
            print(f"    - {s}")
    if dramc_ac:
        print(f"  DRAMC Timing:      {dramc_ac[0] if dramc_ac else 'N/A'}")
    if mpll:
        for s in mpll:
            print(f"  MPLL:              {s}")

    # Print any uncategorized DDR strings
    uncategorized = [s for s in other_ddr
                     if not any(s in prev for prev in dramc_ver + dram_type + ddr_pll)]
    if uncategorized:
        print(f"  Other DDR Strings:")
        for s in uncategorized[:20]:
            print(f"    - {s}")


def print_flash_info(entries):
    """Print flash table entries."""
    print_header("SPI-NAND FLASH TABLE")
    if not entries:
        print("  (no entries found)")
        return

    # Filter valid entries
    valid = [e for e in entries if not e.get('skipped') and e.get('mfr_id', 0) != 0]
    if not valid:
        print("  (all entries are empty/skipped)")
        return

    print(f"  Total: {len(valid)} entries")
    print(f"  {'Idx':>3}  {'Mfr':>4}  {'Dev':>4}  {'Size(MB)':>9}  {'Page(KB)':>9}  {'Erase(KB)':>10}  {'OOB(B)':>7}  Name")
    print(f"  {'-'*3}  {'-'*4}  {'-'*4}  {'-'*9}  {'-'*9}  {'-'*10}  {'-'*7}  {'-'*30}")

    for e in valid:
        name_short = e.get('name', '')
        if len(name_short) > 30:
            name_short = name_short[:27] + '...'
        print(f"  {e['idx']:3d}  0x{e['mfr_id']:02x}  0x{e['dev_id']:02x}  "
              f"{e['device_mb']:7d} MB  {e['page_kb']:7d} KB  {e['erase_kb']:8d} KB  "
              f"{e['oob_b']:5d} B  {name_short}")


def print_structure_info(info):
    """Print binary structure details."""
    print_header("BINARY STRUCTURE")
    print(f"  File Size:         {info['size']} bytes (0x{info['size']:x})")
    print(f"  Mode:              {info['mode']}")

    if info['mode'] == 'lzma' and 'opt_header' in info:
        h = info['opt_header']
        print(f"  Opt Header @       0x{h['offset']:05x} ({h['size']} bytes, {h['size']//4} u32)")
        print(f"  BL22 LZMA @        0x{h['lzma_start']:05x} (csz=0x{h['bl22_csz']:x} / {h['bl22_csz']} bytes)")
        print(f"  BL23 LZMA @        0x{h['lzma_start'] + h['bl22_csz']:05x} (csz=0x{h['bl23_csz']:x} / {h['bl23_csz']} bytes)")
        ft_off = h['lzma_start'] + h['bl22_csz'] + h['bl23_csz']
        print(f"  Flash Table LZMA @ 0x{ft_off:05x} (csz=0x{h['ft_csz']:x} / {h['ft_csz']} bytes)")
        print(f"  VA Base:           0x{h['va_base']:08x}")

        if info.get('bl22_decomp_size'):
            print(f"  BL22 Decompressed: {info['bl22_decomp_size']} bytes (0x{info['bl22_decomp_size']:x})")


def print_chip_id(info):
    """Print chip identification."""
    chip = info.get('chip', 'unknown')
    filename = info.get('filename', '')
    print_header("CHIP IDENTIFICATION")
    print(f"  Filename:          {os.path.basename(filename)}")
    print(f"  Detected SoC:      {chip}")


def print_summary(info):
    """Print a compact preloader summary."""
    border = "=" * 60
    print(border)
    print("  Preloader Summary")
    print(border)

    # --- BL2 Version ---
    build_strs = info.get('build_strings') or info.get('stage1_strings') or []
    version = None
    extra_info = None
    commit = None
    build_date = None

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

        # Standalone (release):commit
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

    if version:
        ver_str = f"v{version}"
        if extra_info:
            ver_str += f" ({extra_info})"
        if commit:
            ver_str += f" commit:{commit}"
        print(f"  BL2 Version:       {ver_str}")
    else:
        print("  BL2 Version:       N/A")
    print(f"  Build Date:        {build_date}" if build_date else "  Build Date:        N/A")

    # --- DRAMC Version ---
    dram_strs = info.get('dram_strings') or []
    dramc_ver = None
    dram_type_strs = []
    ddr_pll_strs = []

    for _, s in dram_strs:
        if 'DRAMC' in s and 'V' in s:
            if dramc_ver is None:
                dramc_ver = s
        elif 'dram_type' in s:
            dram_type_strs.append(s)
        elif 'PLL' in s and 'DDR' in s and 'setting' in s.lower():
            ddr_pll_strs.append(s)

    print(f"  DRAMC Version:     {dramc_ver}" if dramc_ver else "  DRAMC Version:     N/A")

    # --- Supported Type ---
    if dram_type_strs:
        for s in dram_type_strs:
            print(f"  Supported Type:    {s}")
    else:
        print(f"  Supported Type:    N/A")

    # --- DDR PLL Modes ---
    if ddr_pll_strs:
        print(f"  DDR PLL Modes:")
        for s in ddr_pll_strs:
            print(f"    - {s}")
    else:
        print(f"  DDR PLL Modes:     N/A")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description='Extract BL2 preloader metadata from Airoha SoC binaries',
    )
    parser.add_argument('bl2_bin', help='Path to BL2 binary')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--all', action='store_true', default=True,
                       help='Show all available info (default)')
    group.add_argument('--dram', action='store_true',
                       help='Show only DRAM/DDR info')
    group.add_argument('--build', action='store_true',
                       help='Show only build/version info')
    group.add_argument('--flash', action='store_true',
                       help='Show only flash table')
    group.add_argument('--structure', action='store_true',
                       help='Show only binary structure')
    group.add_argument('--summary', action='store_true',
                       help='Show compact preloader summary')
    group.add_argument('--raw', action='store_true',
                       help='Dump all extracted strings by category')
    return parser.parse_args()


def main():
    args = parse_args()

    if not os.path.isfile(args.bl2_bin):
        print(f"ERROR: file not found: {args.bl2_bin}", file=sys.stderr)
        sys.exit(1)

    with open(args.bl2_bin, 'rb') as f:
        data = f.read()

    if len(data) < 0x4000:
        print(f"ERROR: file too small ({len(data)} bytes), not a valid BL2 binary",
              file=sys.stderr)
        sys.exit(1)

    mode, opt_hdr = detect_mode(data)

    if mode == 'struct':
        info = analyze_struct(data, args.bl2_bin)
    else:
        info = analyze_lzma(data, args.bl2_bin, opt_hdr)

    # Print what was requested
    if args.dram:
        print_dram_info(info)
    elif args.build:
        print_build_info(info)
    elif args.flash:
        if mode == 'lzma' and opt_hdr:
            entries = analyze_flash_table_lzma(data, opt_hdr)
            print_flash_info(entries)
        elif mode == 'struct':
            print_flash_info([])  # struct mode flash handled by update_bl2_flash_table.py
            print("  (use 'update_bl2_flash_table.py --list' for struct-mode flash table)")
    elif args.structure:
        print_structure_info(info)
    elif args.summary:
        print_summary(info)
    elif args.raw:
        print(f"=== Raw Strings ({args.bl2_bin}) ===")
        if mode == 'struct':
            all_strs = extract_strings(data, 6)
        else:
            # For LZMA, show strings from stage-1 and BL22 decompressed
            print(f"\n--- Stage-1 (uncompressed) ---")
            stage1 = data[:0x3800]
            for off, s in extract_strings(stage1, 6):
                print(f"  0x{off:05x}: {s}")
            if info.get('build_strings'):
                print(f"\n--- BL22 (decompressed) ---")
                for off, s in info['build_strings']:
                    print(f"  0x{off:05x}: {s}")
                for off, s in info.get('dram_strings', []):
                    print(f"  0x{off:05x}: {s}")
    else:
        # --all: show everything
        print_chip_id(info)
        print_structure_info(info)
        print_build_info(info)
        print_dram_info(info)

        if mode == 'lzma' and opt_hdr:
            entries = analyze_flash_table_lzma(data, opt_hdr)
            print_flash_info(entries)
        elif mode == 'struct':
            print_header("SPI-NAND FLASH TABLE")
            if info.get('flash_strings'):
                seen = set()
                for _, s in info['flash_strings']:
                    if s not in seen:
                        print(f"  {s}")
                        seen.add(s)
            else:
                print("  (use 'update_bl2_flash_table.py --list' for detailed flash table)")

    print()  # trailing newline


if __name__ == '__main__':
    main()
