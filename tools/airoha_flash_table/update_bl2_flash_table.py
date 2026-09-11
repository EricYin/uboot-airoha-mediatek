#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
update_bl2_flash_table.py -- Update SPI-NAND flash table in Airoha BL2 binary.

Two operating modes are supported:

Mode "lzma" (default): for SoCs that embed the flash table as an LZMA-Alone
stream behind a 0x24-byte optimization header (an7563, an7581, an7583):
  1. Compiles and runs flash_table_gen to produce a new binary flash table
  2. Pads the table to the fixed size expected by BL2 (0x5300 bytes)
  3. Compresses it with LZMA-Alone (property=0x5d, 8 MiB dictionary) so the
     BL2-stage decompressor can handle it
  4. Locates the optimization header and old LZMA stream inside the BL2 image
  5. Replaces the old stream, updates flash_table_length in the header
  6. Recomputes the trailing CRC32 (without final XOR)
  - --list  prints all entries from the decompressed flash table

Mode "struct" (--mode struct): for SoCs whose BL2 embeds the flash table as
a compile-time C struct array with virtual-address name pointers
(en7523, en7562).  The table is edited in place:
  - --list  prints all entries
  - --entry N selects the entry to patch
  - field options (--mfr --dev --device-size --page-size --erase-size
    --oob-size --name ...) write the new values directly into the binary
  - the name string is overwritten in place; a longer name is rejected
  - there is no CRC or optimization header to update in this format

Usage examples:
  # LZMA mode (an7563 / an7581 / an7583):
  python3 update_bl2_flash_table.py bl2.bin --list
  python3 update_bl2_flash_table.py bl2.bin
  python3 update_bl2_flash_table.py bl2.bin --header-offset 0x3800
  python3 update_bl2_flash_table.py bl2.bin -o bl2_patched.bin

  # Struct mode (en7523 / en7562):
  python3 update_bl2_flash_table.py bl2.bin --mode struct --list
  python3 update_bl2_flash_table.py bl2.bin --mode struct --entry 8 \
      --mfr 0xef --dev 0xae --device-size 0x08000000 \
      --page-size 0x800 --erase-size 0x20000 --oob-size 0x40 --name W25N01KV
"""

import argparse
import binascii
import os
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# Constants matching the BL2 binary layout
# ---------------------------------------------------------------------------
OPT_HEADER_SIZE      = 0x24          # nine little-endian u32 fields
FLASH_TABLE_UNCOMP   = 0x5300        # uncompressed table size BL2 expects
LZMA_PROPERTY        = 0x5d          # lc=3, lp=0, pb=2
LZMA_DICT_SIZE       = 8 * 1024 * 1024  # 8 MiB

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def crc32_no_xor(data: bytes) -> int:
    """CRC32 *without* the usual final XOR with 0xFFFFFFFF.

    The Airoha BL2 images store their integrity CRC this way.
    """
    return binascii.crc32(data) & 0xFFFFFFFF ^ 0xFFFFFFFF


def find_opt_header(data: bytes, search_start=0x3000, search_end=0x10000):
    """Scan *data* for the BL2 optimization header.

    Returns (header_offset, lzma_offset, ft_len, uncomp_size, fields_tuple)
    or None when no plausible header is found.

    The header consists of nine little-endian u32 values.  A candidate must
    pass basic sanity checks on bl22_length, bl23_length and
    flash_table_length, and the computed LZMA position must contain a valid
    LZMA-Alone stream (±16 bytes tolerance for SoC-specific alignment).
    """
    end = min(search_end, len(data) - OPT_HEADER_SIZE)
    step = 4

    for off in range(search_start, end, step):
        fields = struct.unpack_from('<9I', data, off)
        bl22_l, bl23_l, ft_l = fields[0], fields[1], fields[2]

        # Heuristic bounds: each stage is tens of KB; ft is ~1 KB
        if not (0x1000 < bl22_l < 0x20000 and 0x1000 < bl23_l < 0x20000):
            continue
        if not (0x100 < ft_l < 0x2000):
            continue

        expected_lzma = off + OPT_HEADER_SIZE + bl22_l + bl23_l

        # Search ±16 bytes around the expected position for the LZMA stream.
        # Some SoCs (an7563) have a 4-byte misalignment relative to the
        # header-derived offset.
        for delta in range(-16, 17):
            lzma_off = expected_lzma + delta
            if lzma_off < 0 or lzma_off + 13 > len(data):
                continue
            if data[lzma_off] != LZMA_PROPERTY:
                continue

            dict_sz = struct.unpack_from('<I', data, lzma_off + 1)[0]
            if dict_sz != LZMA_DICT_SIZE:
                continue

            uncomp = struct.unpack_from('<Q', data, lzma_off + 5)[0]
            if 0x4000 < uncomp < 0x10000:
                return off, lzma_off, ft_l, uncomp, fields

    return None


def _find_lzma_tool():
    """Return the path to a BL2-compatible LZMA encoder.

    Prefers the classic ``lzma`` (LZMA SDK) encoder over ``xz`` because
    the SDK encoder writes correct uncompressed-size fields and produces
    streams that the BL2-stage decompressor can handle natively.
    """
    import shutil
    for candidate in ('lzma', 'xz'):
        path = shutil.which(candidate)
        if path:
            return candidate
    sys.exit('ERROR: neither "lzma" nor "xz" found on PATH. '
             'Install lzma or xz-utils.')


def compress_lzma_alone(data: bytes) -> bytes:
    """Compress *data* with LZMA-Alone using a host LZMA tool.

    Uses the classic ``lzma`` (LZMA SDK) encoder when available; falls
    back to ``xz --format=lzma``.  Both produce streams compatible with
    the Airoha BL2 decompressor (lc=3, lp=0, pb=2, 8 MiB dictionary).
    """
    cmd = _find_lzma_tool()

    args = [cmd]
    if cmd == 'xz':
        args += ['--format=lzma']
    args += ['-c', '-']  # read stdin, write stdout

    proc = subprocess.run(args, input=data, capture_output=True, check=True)
    compressed = proc.stdout

    # Sanity-check the header
    if len(compressed) < 13:
        sys.exit('ERROR: LZMA output too short')
    if compressed[0] != LZMA_PROPERTY:
        sys.exit(f'ERROR: LZMA property byte is 0x{compressed[0]:02x}, '
                 f'expected 0x{LZMA_PROPERTY:02x}')
    return compressed


def decompress_lzma_alone(data: bytes) -> bytes:
    """Decompress *data* (LZMA-Alone) using a host LZMA tool.

    Tries ``xz --decompress --format=lzma`` first; falls back to
    ``lzma -d``.  Both handle LZMA-Alone streams produced by the BL2
    build system.
    """
    import shutil

    # Prefer xz for its robustness with unknown uncompressed-size fields
    for cmd in ('xz', 'lzma'):
        path = shutil.which(cmd)
        if path is None:
            continue
        try:
            args = [path, '-d']
            if cmd == 'xz':
                args += ['--format=lzma']
            args += ['-c', '-']
            proc = subprocess.run(args, input=data, capture_output=True,
                                  check=True)
            if proc.returncode == 0 and proc.stdout:
                return proc.stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue

    sys.exit('ERROR: cannot decompress LZMA stream; install xz-utils or lzma')


def backup_file(path: str) -> str:
    """Create a timestamped backup of *path* before overwriting it.

    Returns the backup path so callers can print what was saved.
    """
    import shutil
    bak = path + '.bak'
    shutil.copy2(path, bak)
    return bak


# ---------------------------------------------------------------------------
# Parse binary flash table (as produced by flash_table_gen)
# ---------------------------------------------------------------------------
# The binary layout of one entry (serialised by flash_table_gen::main()):
#   mfr_id             u8      1
#   dev_id             u8      1
#   device_size        u32     4
#   page_size          u32     4
#   erase_size         u32     4
#   oob_size           u32     4
#   dummy_mode         u32     4
#   read_mode          u32     4
#   die_num            u8      1
#   write_mode         u32     4
#   feature            u32     4
#   ecc_fail_check_info struct 2   (2 x u8)
#   write_en_type      u32     4
#   unlock_block_info  struct  2   (2 x u8)
#   quad_en            struct  2   (2 x u8)
#   ecc_en             struct  3   (3 x u8)
#   [ otp_page_num     u8      1 ]  -- only when TCSUPPORT_NAND_FLASH_OTP
#   soc_ecc_ability    u8      1
#   extend_dev_id      struct  4   (u8 extend_len + u8[3] extend_id)
#   TOTAL                     53 (or 54 with OTP)

# Binary header (bl2_flash_H) has 6 x u32 = 24 bytes:
#   flash_entry, flash_name_off, flash_oob_off,
#   parallel_entry, parallel_name_off, parallel_oob_off
FT_HEADER_SIZE = 24

def _ft_read_names(data: bytes, name_off: int, count: int) -> list:
    """Read *count* NUL-terminated name strings starting at *name_off*."""
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
        # strip all trailing NULs
        name = raw.rstrip(b'\x00').decode('ascii', errors='replace')
        names.append(name)
    return names


def _ft_parse_entries(data: bytes, entry_count: int, entry_size: int) -> list:
    """Parse *entry_count* entries from the serialized binary table."""
    entries = []
    for i in range(entry_count):
        off = FT_HEADER_SIZE + i * entry_size
        if off + entry_size > len(data):
            break
        mfr = data[off]
        dev = data[off + 1]
        sz = struct.unpack_from('<I', data, off + 2)[0]
        page = struct.unpack_from('<I', data, off + 6)[0]
        erase = struct.unpack_from('<I', data, off + 10)[0]
        oob = struct.unpack_from('<I', data, off + 14)[0]
        entries.append((mfr, dev, sz, page, erase, oob))
    return entries


def parse_and_list_lzma_table(data: bytes) -> None:
    """Parse decompressed LZMA flash table and pretty-print all entries."""
    if len(data) < FT_HEADER_SIZE:
        sys.exit('ERROR: decompressed flash table is too small')

    flash_entry = struct.unpack_from('<I', data, 0)[0]
    flash_name_off = struct.unpack_from('<I', data, 4)[0]

    if flash_entry == 0:
        print('Flash table is empty (0 entries)')
        return

    # Detect per-entry size: (name_off - header) / count
    entry_size = (flash_name_off - FT_HEADER_SIZE) // flash_entry
    if entry_size not in (53, 54):
        print(f'  WARNING: unexpected entry size {entry_size} '
              f'(expected 53 or 54), proceeding anyway')

    entries = _ft_parse_entries(data, flash_entry, entry_size)
    names = _ft_read_names(data, flash_name_off, flash_entry)

    print(f'LZMA flash table: {flash_entry} entries  '
          f'(entry_size={entry_size})\n')
    print(f'{"idx":>3}  {"mfr":>4}  {"dev":>4}  {"size(MB)":>8}  '
          f'{"page":>5}  {"erase":>6}  {"oob":>4}  name')
    print(f'{"---":>3}  {"---":>4}  {"---":>4}  {"--------":>8}  '
          f'{"-----":>5}  {"------":>6}  {"----":>4}  ----')

    for i, entry in enumerate(entries):
        mfr, dev, sz, page, erase, oob = entry
        name = names[i] if i < len(names) else '(missing)'
        print(f'{i:3d}  0x{mfr:02x}   0x{dev:02x}   '
              f'{sz >> 20:8d}  {page:5d}  {erase:6d}  {oob:4d}  {name}')


# ---------------------------------------------------------------------------
# Struct mode: compile-time C struct array (en7523 / en7562 style)
# ---------------------------------------------------------------------------
# Each entry is a 32-bit little-endian ARM struct with virtual-address
# pointers.  Field offsets below were verified against en7523/en7562 BL2
# images (entry stride differs per SoC: 0x40 for en7523, 0x44 for en7562).
STRUCT_FIELD_OFF = {
    'mfr_id':       0x00,   # u8
    'dev_id':       0x01,   # u8
    'ptr_name':     0x04,   # u32 virtual address of name string
    'device_size':  0x08,   # u32
    'page_size':    0x0c,   # u32
    'erase_size':   0x10,   # u32
    'oob_size':     0x14,   # u32
    'dummy_mode':   0x18,   # u32
    'read_mode':    0x1c,   # u32
    'oob_free_layout': 0x20,  # u32 (keep as-is)
    'die_num':      0x24,   # u32
    'write_mode':   0x28,   # u32
    'feature':      0x2c,   # u32
}
STRUCT_VA_BASE = 0x08000000   # en7523/en7562 link base
KNOWN_MFR = (0xc8, 0xef, 0xc2, 0x2c, 0x98, 0xd5, 0xec, 0xbc, 0xcd,
             0xa1, 0x9b, 0x0b, 0x01)


def _read_cstr(data: bytes, off: int) -> str:
    """Read a NUL-terminated ASCII string at *off* (best effort)."""
    if off < 0 or off >= len(data):
        return '<out-of-range>'
    end = data.find(b'\x00', off)
    if end < 0:
        end = min(off + 64, len(data))
    try:
        return data[off:end].decode('ascii', 'replace')
    except UnicodeDecodeError:
        return data[off:end].hex()


def find_struct_array(data: bytes, va_base=STRUCT_VA_BASE):
    """Locate the compile-time struct array inside *data*.

    Returns (array_offset, entry_stride, entry_count) or None.

    Candidate entries are located purely by layout: a u32 whose high 16
    bits are zero (u8 mfr_id + u8 dev_id + 2 pad bytes) followed at +4 by
    a u32 virtual address that falls inside the image.  The largest run
    with a constant stride wins.  Name string *content* is deliberately
    not consulted, so patching a name does not break re-detection.
    """
    # Plausible VA window: inside the image, above the low 64 KiB
    # (code section), below the end of the image.
    lo = va_base + 0x10000
    hi = va_base + len(data)

    cands = []
    for i in range(0, len(data) - 8, 4):
        md = struct.unpack_from('<I', data, i)[0]
        ptr = struct.unpack_from('<I', data, i + 4)[0]
        if (md >> 16) != 0:
            continue
        if not (lo <= ptr <= hi):
            continue
        cands.append(i)

    if len(cands) < 3:
        return None

    # Group candidates into runs with a constant stride.
    cand_set = set(cands)
    best = None  # (stride, start, count)
    for stride in (0x40, 0x44, 0x48, 0x4c):
        for c in cands:
            if c - stride in cand_set:
                continue  # already counted as part of an earlier run
            count = 1
            n = c + stride
            while n in cand_set:
                count += 1
                n += stride
            if count >= 3 and (best is None or count > best[2]):
                best = (stride, c, count)

    if best is None:
        return None
    return best[1], best[0], best[2]


def struct_list(data: bytes, arr_off: int, stride: int, count: int,
                va_base=STRUCT_VA_BASE):
    """Print a human-readable table of all struct entries."""
    print(f'Struct flash table @ 0x{arr_off:x}: {count} entries, '
          f'stride 0x{stride:x}')
    print(f'{"idx":>3} {"mfr":>4} {"dev":>4} {"size(MB)":>8} '
          f'{"page":>5} {"erase":>6} {"oob":>4}  name')
    for i in range(count):
        e = arr_off + i * stride
        mfr = data[e + STRUCT_FIELD_OFF['mfr_id']]
        dev = data[e + STRUCT_FIELD_OFF['dev_id']]
        dev_sz = struct.unpack_from('<I', data,
                                    e + STRUCT_FIELD_OFF['device_size'])[0]
        page = struct.unpack_from('<I', data,
                                  e + STRUCT_FIELD_OFF['page_size'])[0]
        erase = struct.unpack_from('<I', data,
                                   e + STRUCT_FIELD_OFF['erase_size'])[0]
        oob = struct.unpack_from('<I', data,
                                 e + STRUCT_FIELD_OFF['oob_size'])[0]
        name_va = struct.unpack_from('<I', data,
                                     e + STRUCT_FIELD_OFF['ptr_name'])[0]
        name = _read_cstr(data, name_va - va_base)
        print(f'{i:3d} 0x{mfr:02x}  0x{dev:02x}  {dev_sz >> 20:8d} '
              f'{page:5d} {erase:6d} {oob:4d}  {name}')


def struct_patch(data: bytearray, arr_off: int, stride: int, count: int,
                 entry: int, fields: dict, new_name=None,
                 va_base=STRUCT_VA_BASE):
    """Patch one struct entry in place.  Returns list of change strings."""
    if not 0 <= entry < count:
        sys.exit(f'ERROR: entry {entry} out of range (0..{count - 1})')
    e = arr_off + entry * stride
    changes = []

    for key, val in fields.items():
        off = STRUCT_FIELD_OFF[key]
        if key in ('mfr_id', 'dev_id'):
            if not 0 <= val <= 0xff:
                sys.exit(f'ERROR: {key} must fit in a byte')
            if data[e + off] != val:
                changes.append(f'{key}: 0x{data[e + off]:02x}->0x{val:02x}')
            data[e + off] = val
        elif key in ('device_size', 'page_size', 'erase_size', 'oob_size',
                     'dummy_mode', 'read_mode', 'die_num', 'write_mode',
                     'feature'):
            if not 0 <= val <= 0xffffffff:
                sys.exit(f'ERROR: {key} must fit in u32')
            old = struct.unpack_from('<I', data, e + off)[0]
            if old != val:
                changes.append(f'{key}: 0x{old:x}->0x{val:x}')
            struct.pack_into('<I', data, e + off, val)
        else:
            sys.exit(f'ERROR: unsupported field {key}')

    if new_name is not None:
        name_va = struct.unpack_from('<I', data,
                                     e + STRUCT_FIELD_OFF['ptr_name'])[0]
        no = name_va - va_base
        old = _read_cstr(data, no)

        # Auto-prepend prefix: if the user supplied a short name like
        # "W25N01KV" and the old name contains "_SPI_NAND_DEVICE_ID_",
        # extract the prefix (including a possible leading space) and
        # prepend it so the result stays consistent with the existing
        # naming convention in the binary.
        PREFIX_MARKER = '_SPI_NAND_DEVICE_ID_'
        if PREFIX_MARKER not in new_name:
            idx = old.find(PREFIX_MARKER)
            if idx >= 0:
                prefix = old[:idx + len(PREFIX_MARKER)]
                new_name = prefix + new_name

        if len(new_name) > len(old):
            sys.exit(f'ERROR: new name "{new_name}" ({len(new_name)} chars) '
                     f'longer than existing "{old}" ({len(old)} chars); '
                     f'name area is packed, cannot grow in place')
        # Pad with NULs to wipe any leftover bytes of the old name.
        data[no:no + len(old) + 1] = new_name.encode() + b'\x00' * \
            (len(old) + 1 - len(new_name))
        changes.append(f'name: "{old}"->"{new_name}"')

    return changes


# ---------------------------------------------------------------------------
# Build flash_table_gen
# ---------------------------------------------------------------------------

def build_flash_table_gen(flash_table_dir: str) -> str:
    """Build the flash_table_gen host tool and return its path."""
    tool = os.path.join(flash_table_dir, 'flash_table_gen')
    if not os.path.isfile(tool):
        print(f'Building flash_table_gen in {flash_table_dir} ...')
        subprocess.run(['make', '-C', flash_table_dir, 'clean', 'all'],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if not os.path.isfile(tool):
        sys.exit(f'ERROR: {tool} was not built')
    return tool


def generate_flash_table(flash_table_gen: str) -> bytes:
    """Run flash_table_gen and return the uncompressed flash_table.bin."""
    cwd = os.path.dirname(flash_table_gen) or '.'
    subprocess.run([flash_table_gen], cwd=cwd, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    bin_path = os.path.join(cwd, 'flash_table.bin')
    with open(bin_path, 'rb') as fh:
        raw = fh.read()
    print(f'  flash_table_gen produced {len(raw)} bytes (0x{len(raw):x})')
    return raw


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Update SPI-NAND flash table in an Airoha BL2 binary')
    ap.add_argument('bl2_path', help='Path to the BL2 binary to patch')
    ap.add_argument('--mode', choices=('lzma', 'struct'), default='lzma',
                    help='Update strategy: "lzma" replaces the LZMA stream '
                         '(an7563/an7583); "struct" patches the compile-time '
                         'struct array in place (en7523/en7562)')
    ap.add_argument('--flash-table-dir', default=None,
                    help='Directory containing flash_table_gen Makefile '
                         '(default: <script-dir>/flash_table)')
    ap.add_argument('--output', '-o', default=None,
                    help='Output file (default: overwrite the input)')
    ap.add_argument('--header-offset', type=lambda x: int(x, 0), default=None,
                    help='Force optimization header offset, e.g. 0x3800 '
                         '(lzma mode)')

    # ---- Entry listing and patching options ----------------------------
    ap.add_argument('--list', action='store_true',
                    help='List all flash table entries (works in both lzma '
                         'and struct modes)')
    ap.add_argument('--entry', type=lambda x: int(x, 0), default=None,
                    help='Index of the entry to patch (struct mode only)')
    ap.add_argument('--mfr', type=lambda x: int(x, 0), default=None,
                    help='New mfr_id, e.g. 0xef (struct mode only)')
    ap.add_argument('--dev', type=lambda x: int(x, 0), default=None,
                    help='New dev_id, e.g. 0xae (struct mode only)')
    ap.add_argument('--device-size', type=lambda x: int(x, 0), default=None,
                    help='New device_size in bytes (struct mode only)')
    ap.add_argument('--page-size', type=lambda x: int(x, 0), default=None,
                    help='New page_size in bytes (struct mode only)')
    ap.add_argument('--erase-size', type=lambda x: int(x, 0), default=None,
                    help='New erase_size in bytes (struct mode only)')
    ap.add_argument('--oob-size', type=lambda x: int(x, 0), default=None,
                    help='New oob_size in bytes (struct mode only)')
    ap.add_argument('--name', default=None,
                    help='New model suffix, e.g. W25N01KV.  The '
                         '"_SPI_NAND_DEVICE_ID_" prefix (with optional '
                         'leading space) is auto-prepended from the '
                         'existing entry.  Full name also accepted. '
                         '(struct mode only; must not be longer than '
                         'the existing name)')
    ap.add_argument('--va-base', type=lambda x: int(x, 0),
                    default=STRUCT_VA_BASE,
                    help=f'Virtual-address base of the image (struct mode, '
                         f'default 0x{STRUCT_VA_BASE:x})')
    args = ap.parse_args()

    # ---- Load BL2 image ------------------------------------------------
    with open(args.bl2_path, 'rb') as fh:
        bl2 = bytearray(fh.read())
    print(f'Loaded BL2: {len(bl2)} bytes (0x{len(bl2):x})')

    # --list works for both modes; for lzma we decompress and parse ------
    if args.list and args.mode != 'struct':
        return _main_lzma_list(args, bl2)

    if args.mode == 'struct':
        return _main_struct(args, bl2)

    return _main_lzma(args, bl2)


def _main_struct(args, bl2: bytearray) -> int:
    """Handle --mode struct: in-place patch of the struct array."""
    # ---- Locate struct array -------------------------------------------
    found = find_struct_array(bl2, va_base=args.va_base)
    if found is None:
        sys.exit('ERROR: could not locate the compile-time struct flash '
                 'table.  This BL2 may use the LZMA layout instead; try '
                 'without --mode struct.')
    arr_off, stride, count = found
    print(f'Struct flash table: entries={count}, stride=0x{stride:x}, '
          f'array @ 0x{arr_off:x}')

    if args.list or not (args.entry is not None or args.mfr is not None or
                         args.dev is not None or args.device_size is not None
                         or args.page_size is not None or
                         args.erase_size is not None or
                         args.oob_size is not None or args.name is not None):
        struct_list(bl2, arr_off, stride, count, va_base=args.va_base)
        return 0

    if args.entry is None:
        sys.exit('ERROR: --entry is required to patch a struct entry')

    # ---- Build field dict ----------------------------------------------
    fields = {}
    for key, arg in (('mfr_id', args.mfr), ('dev_id', args.dev),
                     ('device_size', args.device_size),
                     ('page_size', args.page_size),
                     ('erase_size', args.erase_size),
                     ('oob_size', args.oob_size)):
        if arg is not None:
            fields[key] = arg
    if not fields and args.name is None:
        sys.exit('ERROR: nothing to change; pass field options or --name')

    changes = struct_patch(bl2, arr_off, stride, count, args.entry,
                           fields, new_name=args.name, va_base=args.va_base)
    print(f'Entry {args.entry} changes:')
    for c in changes:
        print(f'  {c}')

    # ---- Write output ---------------------------------------------------
    out_path = args.output if args.output else args.bl2_path
    if out_path == args.bl2_path:
        bak = backup_file(args.bl2_path)
        print(f'Backed up original to {bak}')
    with open(out_path, 'wb') as fh:
        fh.write(bl2)
    print(f'Wrote {out_path}')
    return 0


def _main_lzma_list(args, bl2: bytearray) -> int:
    """Handle --list for lzma mode: decompress flash table and print entries.

    Locates the optimization header + LZMA stream inside the BL2 image,
    decompresses it with the host LZMA tool, then parses and pretty-prints
    the binary flash table generated by flash_table_gen.
    """
    # ---- Locate optimization header ----------------------------------------
    if args.header_offset is not None:
        off = args.header_offset
        if off + OPT_HEADER_SIZE > len(bl2):
            sys.exit(f'ERROR: forced header offset 0x{off:x} is out of range')
        fields = struct.unpack_from('<9I', bl2, off)
        bl22_l, bl23_l = fields[0], fields[1]
        lzma_off = off + OPT_HEADER_SIZE + bl22_l + bl23_l
        ft_len = fields[2]
        if lzma_off + 13 > len(bl2) or bl2[lzma_off] != LZMA_PROPERTY:
            found_lzma = False
            for delta in range(-16, 17):
                lo = lzma_off + delta
                if 0 <= lo <= len(bl2) - 13 and bl2[lo] == LZMA_PROPERTY:
                    lzma_off = lo
                    found_lzma = True
                    break
            if not found_lzma:
                sys.exit(f'ERROR: cannot find LZMA stream near computed '
                         f'offset 0x{lzma_off:x}')
        hdr_off = off
    else:
        result = find_opt_header(bl2)
        if result is None:
            sys.exit(
                'ERROR: could not locate the optimization header.\n'
                'This BL2 image may not embed the flash table as an LZMA '
                'stream (en7523 / en7562 use --mode struct).\n'
                'Try forcing --header-offset if you know the correct offset.\n')
        hdr_off, lzma_off, ft_len, uncomp_old, fields = result

    print(f'Optimization header at 0x{hdr_off:05x}')
    print(f'Flash table LZMA   at 0x{lzma_off:05x}  (len=0x{ft_len:x})')

    # ---- Extract and decompress LZMA stream --------------------------------
    if lzma_off + ft_len > len(bl2):
        sys.exit(f'ERROR: flash table length 0x{ft_len:x} extends past EOF')

    lzma_data = bytes(bl2[lzma_off:lzma_off + ft_len])
    print(f'Decompressing {ft_len} bytes ...')
    decompressed = decompress_lzma_alone(lzma_data)
    print(f'Decompressed to {len(decompressed)} bytes (0x{len(decompressed):x})')

    # ---- Parse and print ---------------------------------------------------
    parse_and_list_lzma_table(decompressed)
    return 0


def _main_lzma(args, bl2: bytearray) -> int:
    """Handle the default lzma mode."""
    # ---- Resolve flash_table_gen directory ---------------------------------
    if args.flash_table_dir is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        args.flash_table_dir = os.path.join(script_dir, 'flash_table')
    if not os.path.isdir(args.flash_table_dir):
        sys.exit(f'ERROR: flash_table directory not found: {args.flash_table_dir}')

    # ---- Build tool --------------------------------------------------------
    tool = build_flash_table_gen(args.flash_table_dir)

    # ---- Generate new uncompressed table -----------------------------------
    raw_table = generate_flash_table(tool)

    if len(raw_table) > FLASH_TABLE_UNCOMP:
        sys.exit('ERROR: generated table is larger than BL2 expects '
                 f'({len(raw_table)} > {FLASH_TABLE_UNCOMP})')
    # Pad to the fixed size BL2 allocates for the decompressed table
    table = raw_table + b'\x00' * (FLASH_TABLE_UNCOMP - len(raw_table))
    print(f'  padded to {FLASH_TABLE_UNCOMP} bytes (0x{FLASH_TABLE_UNCOMP:x})')

    # ---- Compress with LZMA-Alone ------------------------------------------
    compressed = compress_lzma_alone(table)
    new_ft_len = len(compressed)
    print(f'  compressed to {new_ft_len} bytes (0x{new_ft_len:x})')

    # ---- Locate optimization header ----------------------------------------
    if args.header_offset is not None:
        off = args.header_offset
        if off + OPT_HEADER_SIZE > len(bl2):
            sys.exit(f'ERROR: forced header offset 0x{off:x} is out of range')
        fields = struct.unpack_from('<9I', bl2, off)
        bl22_l, bl23_l = fields[0], fields[1]
        lzma_off = off + OPT_HEADER_SIZE + bl22_l + bl23_l
        ft_len = fields[2]
        # Verify LZMA at computed position
        if lzma_off + 13 > len(bl2) or bl2[lzma_off] != LZMA_PROPERTY:
            # Try ±16 delta
            found = False
            for delta in range(-16, 17):
                lo = lzma_off + delta
                if 0 <= lo <= len(bl2) - 13 and bl2[lo] == LZMA_PROPERTY:
                    lzma_off = lo
                    found = True
                    break
            if not found:
                sys.exit(f'ERROR: cannot find LZMA stream near computed '
                         f'offset 0x{lzma_off:x}')
        hdr_off = off
        ft_old_len = ft_len
    else:
        result = find_opt_header(bl2)
        if result is None:
            sys.exit(
                'ERROR: could not locate the optimization header.\n'
                'This BL2 image may not embed the flash table as an LZMA '
                'stream (en7523 / en7562 are known to lack this layout).\n'
                'Try forcing --header-offset if you know the correct offset.\n')
        hdr_off, lzma_off, ft_old_len, uncomp_old, fields = result

    print(f'  Optimization header at 0x{hdr_off:05x}')
    print(f'  Flash table LZMA   at 0x{lzma_off:05x}')
    print(f'  Old ft_len=0x{ft_old_len:x} ({ft_old_len})')

    # ---- Sanity-check: LZMA stream must end at CRC (EOF-4) -----------------
    expected_crc_off = lzma_off + ft_old_len
    if expected_crc_off != len(bl2) - 4:
        print(f'  WARNING: LZMA end (0x{expected_crc_off:x}) != CRC position '
              f'(0x{len(bl2) - 4:x}) -- layout may differ from expected')

    # ---- Replace flash table -----------------------------------------------
    # Assemble: [prefix up to LZMA] + [new LZMA stream] + [new CRC]
    prefix = bl2[:lzma_off]
    new_bl2 = bytearray(prefix)
    new_bl2.extend(compressed)

    # Update flash_table_length in the optimization header
    struct.pack_into('<I', new_bl2, hdr_off + 8, new_ft_len)

    # Compute and append CRC32 (without final XOR)
    crc = crc32_no_xor(new_bl2)
    new_bl2.extend(struct.pack('<I', crc))

    print(f'  New ft_len=0x{new_ft_len:x} ({new_ft_len})')
    print(f'  New CRC=0x{crc:08x}')
    print(f'  Output size: {len(new_bl2)} bytes (0x{len(new_bl2):x})')

    # ---- Write output ------------------------------------------------------
    out_path = args.output if args.output else args.bl2_path
    if out_path == args.bl2_path:
        bak = backup_file(args.bl2_path)
        print(f'  Backed up original to {bak}')
    with open(out_path, 'wb') as fh:
        fh.write(new_bl2)
    print(f'Wrote {out_path}')


if __name__ == '__main__':
    main()
