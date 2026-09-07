#!/usr/bin/env python3
"""
pack_bootext.py - Pack/unpack bootext.ram FIP files for EN7523/EN7562.

FIP (Firmware Image Package) format used by Airoha/EN75xx BootROM:
  - TOC Header  (16 bytes): magic(4) + serial(4) + flags(8)
  - TOC Entry   (40 bytes): uuid(16) + offset(8) + size(8) + flags(8)
  - NULL term   (40 bytes): all zeros
  - Data section: entries at ALIGN-byte boundaries
  - CRC32       (4 bytes): trx.c algorithm (init=0xFFFFFFFF, no final XOR)

Standard TF-A UUIDs:
  UUID_TRUSTED_BOOT_FW      = 0becf95f-224d-4d3e-a544-c39d81c73f0a
  UUID_TRUSTED_BOOT_FW_CERT = ea69e2d6-635d-11e4-8d8c-9fbabe9956a5

Usage:
  Pack:   python pack_bootext.py pack bl2.bin [--cert cert.bin] [-o bootext.ram]
  Unpack: python pack_bootext.py unpack bootext.ram [-d output_dir]
  Info:   python pack_bootext.py info bootext.ram
"""

import argparse
import binascii
import os
import struct
import sys
import uuid

# ── Constants ──────────────────────────────────────────────────────────────

FIP_MAGIC = 0xAA640001
FIP_SERIAL_DEFAULT = 0x12345678
FIP_ALIGN_DEFAULT = 0x400  # 1024 bytes (observed in en7523_bootext.ram)

# Standard TF-A UUIDs (from include/tools_share/firmware_image_package.h)
UUID_TRUSTED_BOOT_FW = uuid.UUID("0becf95f-224d-4d3e-a544-c39d81c73f0a")
UUID_TRUSTED_BOOT_FW_CERT = uuid.UUID("ea69e2d6-635d-11e4-8d8c-9fbabe9956a5")
UUID_TRUSTED_KEY_CERT = uuid.UUID("90e87e82-60f8-11e4-a1b4-777a21b4f94c")
UUID_SOC_FW_KEY_CERT = uuid.UUID("ccbeb88a-60f9-11e4-9ad0-eb4822d8dcf8")
UUID_SOC_FW_CONTENT_CERT = uuid.UUID("2a83d58a-60fb-11e4-8aaf-df30bbc49859")
UUID_TRUSTED_OS_FW_KEY_CERT = uuid.UUID("200cb2e2-635e-11e4-9ce8-abccf92bb666")
UUID_TRUSTED_OS_FW_CONTENT_CERT = uuid.UUID("f3c1c48e-635d-11e4-a7a9-87ee40b23fa7")

# Ordered list of TF-A certificate UUIDs used when multiple certs are passed
TF_A_CERT_UUIDS = [
    UUID_TRUSTED_BOOT_FW_CERT,
    UUID_TRUSTED_KEY_CERT,
    UUID_SOC_FW_KEY_CERT,
    UUID_SOC_FW_CONTENT_CERT,
    UUID_TRUSTED_OS_FW_KEY_CERT,
    UUID_TRUSTED_OS_FW_CONTENT_CERT,
]

# Known FIP entry UUIDs for display
UUID_NAMES = {
    UUID_TRUSTED_BOOT_FW: "Trusted Boot FW (BL2)",
    UUID_TRUSTED_BOOT_FW_CERT: "Trusted Boot FW Certificate",
    UUID_TRUSTED_KEY_CERT: "Trusted Key Certificate",
    UUID_SOC_FW_KEY_CERT: "SOC FW Key Certificate",
    UUID_SOC_FW_CONTENT_CERT: "SOC FW Content Certificate",
    UUID_TRUSTED_OS_FW_KEY_CERT: "Trusted OS FW Key Certificate",
    UUID_TRUSTED_OS_FW_CONTENT_CERT: "Trusted OS FW Content Certificate",
    uuid.UUID("04625d26-9a42-4a9b-b21f-6eb2bb6977d4"): "SCP Firmware (BL2)",
    uuid.UUID("9d67a958-cddc-4e67-89df-3c1d3e8e8d50"): "EL3 Runtime FW (BL31)",
    uuid.UUID("d5731a9b-d0ee-4c9e-a4b5-5e5a8a9f1c2d"): "NT Firmware (BL33/U-Boot)",
}


# ── UUID encoding ──────────────────────────────────────────────────────────

def uuid_to_fip_bytes(u: uuid.UUID) -> bytes:
    """Encode UUID as FIP TOC entry bytes (mixed-endian RFC 4122).

    First 4 bytes (time_low): little-endian
    Next 2 bytes (time_mid):  little-endian
    Next 2 bytes (time_hi):   little-endian
    Last 8 bytes (clock_seq+node): big-endian (as-is)
    """
    fields = u.fields  # (time_low, time_mid, time_hi, clock_seq, node)
    return struct.pack("<IHH", fields[0], fields[1], fields[2]) + u.bytes[8:]


def fip_bytes_to_uuid(raw: bytes) -> uuid.UUID:
    """Decode FIP TOC entry UUID bytes back to uuid.UUID.

    FIP uses mixed-endian RFC 4122:
      bytes[0:4]  time_low  (LE uint32)
      bytes[4:6]  time_mid  (LE uint16)
      bytes[6:8]  time_hi   (LE uint16)
      bytes[8]    clock_seq_hi_variant
      bytes[9]    clock_seq_low
      bytes[10:16] node (big-endian 48-bit)
    """
    time_low = struct.unpack_from("<I", raw, 0)[0]
    time_mid = struct.unpack_from("<H", raw, 4)[0]
    time_hi = struct.unpack_from("<H", raw, 6)[0]
    node = int.from_bytes(raw[10:16], "big")
    return uuid.UUID(fields=(time_low, time_mid, time_hi, raw[8], raw[9], node))


# ── CRC (matching trx.c crc32buf) ──────────────────────────────────────────

def trx_crc32(data: bytes) -> int:
    """Calculate CRC32 matching trx.c algorithm.

    trx.c: init=0xFFFFFFFF, process bytes, return WITHOUT final XOR.
    Python binascii.crc32: init=0xFFFFFFFF, process, return WITH final XOR.
    So: trx_crc = python_crc ^ 0xFFFFFFFF
    """
    return (binascii.crc32(data) & 0xFFFFFFFF) ^ 0xFFFFFFFF


# ── Alignment helper ───────────────────────────────────────────────────────

def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


# ── FIP TOC structures ─────────────────────────────────────────────────────

def build_toc_header(serial: int = FIP_SERIAL_DEFAULT, flags: int = 0) -> bytes:
    """Build 16-byte FIP TOC header."""
    return struct.pack("<IIQ", FIP_MAGIC, serial, flags)


def build_toc_entry(u: uuid.UUID, offset: int, size: int, flags: int = 0) -> bytes:
    """Build 40-byte FIP TOC entry."""
    return uuid_to_fip_bytes(u) + struct.pack("<QQQ", offset, size, flags)


def build_null_entry(file_size: int = 0) -> bytes:
    """Build 40-byte NULL terminator.

    Airoha extension: the offset field stores the total image/file size.
    UUID is all zeros (marks end of TOC), size and flags are zero.
    """
    return b"\x00" * 16 + struct.pack("<QQQ", file_size, 0, 0)


# ── Pack ───────────────────────────────────────────────────────────────────

def pack_bootext(bl2_data: bytes,
                 certs: list = None,
                 serial: int = FIP_SERIAL_DEFAULT,
                 align: int = FIP_ALIGN_DEFAULT,
                 add_crc: bool = True) -> bytes:
    """Pack BL2 (and optional certificates) into FIP bootext.ram format.

    Args:
        bl2_data: Raw BL2 firmware binary
        certs:    List of (uuid, data) tuples for certificates.
                  When None or empty, only BL2 is packed.
                  When exactly one cert is given, UUID_TRUSTED_BOOT_FW_CERT
                  is used by default if the UUID is None.
        serial:   FIP serial number (default 0x12345678)
        align:    Data alignment in bytes (default 0x400)
        add_crc:  If True, append CRC32 (trx.c algorithm) at end

    Returns:
        Complete bootext.ram binary as bytes
    """
    # Build TOC entries list: (uuid, data)
    entries = [(UUID_TRUSTED_BOOT_FW, bl2_data)]
    if certs:
        # certs is list of (uuid, raw_bytes); if uuid is None auto-assign
        for idx, (u, data) in enumerate(certs):
            if u is None:
                if len(certs) == 1:
                    u = UUID_TRUSTED_BOOT_FW_CERT
                elif idx < len(TF_A_CERT_UUIDS):
                    u = TF_A_CERT_UUIDS[idx]
                else:
                    u = UUID_TRUSTED_BOOT_FW_CERT
            entries.append((u, data))

    # Calculate TOC size: header(16) + N entries(40 each) + null(40)
    toc_size = 16 + len(entries) * 40 + 40

    # First data offset: TOC aligned up to ALIGN boundary
    data_offset = align_up(toc_size, align)

    # Calculate offsets for each entry
    entry_layout = []  # [(uuid, offset, size, data), ...]
    current_offset = data_offset
    for u, data in entries:
        entry_layout.append((u, current_offset, len(data), data))
        current_offset = align_up(current_offset + len(data), align)

    # Total file size before CRC: align last entry end to ALIGN
    file_size_no_crc = current_offset  # already aligned

    # If adding CRC, file size increases by 4 bytes
    # But CRC goes at (file_size - 4), so file_size = file_size_no_crc
    # (the last 4 bytes of the aligned padding serve as CRC position)
    # Actually: trx.c does: *(uint32_t*)(buf + input_size - 4) = crc
    # So file_size = file_size_no_crc, and CRC at (file_size_no_crc - 4)
    file_size = file_size_no_crc

    # Build the file
    buf = bytearray(file_size)

    # Write TOC header
    offset = 0
    header = build_toc_header(serial)
    buf[offset:offset + 16] = header
    offset += 16

    # Write TOC entries
    for u, ent_offset, ent_size, _ in entry_layout:
        entry = build_toc_entry(u, ent_offset, ent_size)
        buf[offset:offset + 40] = entry
        offset += 40

    # Write NULL terminator (Airoha extension: offset field = file size)
    buf[offset:offset + 40] = build_null_entry(file_size)
    offset += 40

    # Write data
    for u, ent_offset, ent_size, ent_data in entry_layout:
        buf[ent_offset:ent_offset + ent_size] = ent_data

    # Add CRC if requested
    if add_crc:
        crc_offset = file_size - 4
        # CRC covers everything before the CRC field
        crc_val = trx_crc32(bytes(buf[:crc_offset]))
        struct.pack_into("<I", buf, crc_offset, crc_val)

    return bytes(buf)


# ── Unpack ─────────────────────────────────────────────────────────────────

def unpack_bootext(data: bytes, output_dir: str = None) -> dict:
    """Unpack a bootext.ram FIP file into its components.

    Args:
        data:       bootext.ram binary data
        output_dir: If specified, write extracted files to this directory

    Returns:
        Dict mapping component names to (uuid, offset, size, data) tuples
    """
    if len(data) < 16:
        raise ValueError("File too small to be a valid FIP")

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != FIP_MAGIC:
        raise ValueError(f"Invalid FIP magic: 0x{magic:08X} (expected 0x{FIP_MAGIC:08X})")

    serial = struct.unpack_from("<I", data, 4)[0]
    flags = struct.unpack_from("<Q", data, 8)[0]

    print(f"FIP TOC Header:")
    print(f"  Magic:  0x{magic:08X}")
    print(f"  Serial: 0x{serial:08X}")
    print(f"  Flags:  0x{flags:016X}")
    print()

    # Parse TOC entries
    offset = 16
    components = {}
    entry_num = 0

    while offset + 40 <= len(data):
        uuid_raw = data[offset:offset + 16]
        if uuid_raw == b"\x00" * 16:
            null_offset = struct.unpack_from("<Q", data, offset + 16)[0]
            print(f"  [NULL terminator at 0x{offset:x}]")
            if null_offset:
                print(f"    Image size: 0x{null_offset:x} ({null_offset})")
            break

        u = fip_bytes_to_uuid(uuid_raw)
        ent_offset = struct.unpack_from("<Q", data, offset + 16)[0]
        ent_size = struct.unpack_from("<Q", data, offset + 24)[0]
        ent_flags = struct.unpack_from("<Q", data, offset + 32)[0]

        name = UUID_NAMES.get(u, "Unknown")
        print(f"  Entry {entry_num}: {name}")
        print(f"    UUID:   {u}")
        print(f"    Offset: 0x{ent_offset:x} ({ent_offset})")
        print(f"    Size:   0x{ent_size:x} ({ent_size})")
        print(f"    Flags:  0x{ent_flags:x}")

        if ent_offset + ent_size > len(data):
            print(f"    WARNING: Entry extends beyond file end!")
            ent_data = data[ent_offset:len(data)]
        else:
            ent_data = data[ent_offset:ent_offset + ent_size]

        # Determine filename
        if u == UUID_TRUSTED_BOOT_FW:
            filename = "bl2.bin"
        elif u == UUID_TRUSTED_BOOT_FW_CERT:
            filename = "certificate.der"
        else:
            filename = f"entry{entry_num}_{u.hex}.bin"

        components[filename] = (u, ent_offset, ent_size, ent_data)

        if output_dir:
            os.makedirs(output_dir, exist_ok=True)
            filepath = os.path.join(output_dir, filename)
            with open(filepath, "wb") as f:
                f.write(ent_data)
            print(f"    Saved: {filepath}")

        print()
        offset += 40
        entry_num += 1

    # Check CRC
    crc_offset = len(data) - 4
    stored_crc = struct.unpack_from("<I", data, crc_offset)[0]
    calc_crc = trx_crc32(data[:crc_offset])
    print(f"CRC32:")
    print(f"  Stored:     0x{stored_crc:08X}")
    print(f"  Calculated: 0x{calc_crc:08X}")
    if stored_crc == 0:
        print(f"  Status:     Not calculated (zero)")
    elif stored_crc == calc_crc:
        print(f"  Status:     VALID")
    else:
        print(f"  Status:     MISMATCH")
    print(f"  File size:  0x{len(data):x} ({len(data)} bytes)")

    return components


# ── CLI ────────────────────────────────────────────────────────────────────

def cmd_pack(args):
    """Pack BL2 (+ optional certs) into bootext.ram"""
    # Read BL2
    with open(args.bl2, "rb") as f:
        bl2_data = f.read()
    print(f"BL2: {args.bl2} ({len(bl2_data)} bytes, 0x{len(bl2_data):x})")

    # Read certificates if provided (--cert can be repeated)
    certs = []
    if args.cert:
        for path in args.cert:
            with open(path, "rb") as f:
                data = f.read()
            print(f"Cert: {path} ({len(data)} bytes, 0x{len(data):x})")
            certs.append((None, data))  # UUID auto-assigned in pack_bootext

    # Pack
    result = pack_bootext(
        bl2_data,
        certs=certs if certs else None,
        serial=args.serial,
        align=args.align,
        add_crc=not args.no_crc,
    )

    # Write output
    output = args.output or "bootext.ram"
    with open(output, "wb") as f:
        f.write(result)

    crc_status = "no CRC" if args.no_crc else f"CRC=0x{trx_crc32(result[:-4]):08X}"
    print(f"Output: {output} ({len(result)} bytes, 0x{len(result):x}) [{crc_status}]")
    print(f"  Serial:    0x{args.serial:08X}")
    print(f"  Alignment: 0x{args.align:x}")
    nc = len(certs)
    print(f"  Entries:   BL2" + (f" + {nc} Certificate(s)" if nc else ""))


def cmd_unpack(args):
    """Unpack bootext.ram into components"""
    with open(args.input, "rb") as f:
        data = f.read()

    output_dir = args.output_dir or "bootext_extracted"
    print(f"Unpacking: {args.input} ({len(data)} bytes)")
    print(f"Output dir: {output_dir}")
    print()

    unpack_bootext(data, output_dir)


def cmd_info(args):
    """Show FIP structure info without extracting"""
    with open(args.input, "rb") as f:
        data = f.read()

    print(f"File: {args.input} ({len(data)} bytes, 0x{len(data):x})")
    print()
    unpack_bootext(data, None)


def main():
    parser = argparse.ArgumentParser(
        description="Pack/unpack bootext.ram FIP files for EN7523/EN7562",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Pack BL2 + 1 certificate into bootext.ram (en7523/an7563)
  python pack_bootext.py pack bl2.bin --cert cert.der -o bootext.ram

  # Pack BL2 + multiple certificates (an7581)
  python pack_bootext.py pack bl2.bin --cert c1.der --cert c2.der -o bootext.ram

  # Pack BL2 only (no certificate)
  python pack_bootext.py pack bl2.bin -o bootext.ram

  # Pack without CRC (match original file)
  python pack_bootext.py pack bl2.bin --cert cert.der --no-crc -o bootext.ram

  # Unpack bootext.ram
  python pack_bootext.py unpack bootext.ram -d output_dir/

  # Show FIP info
  python pack_bootext.py info bootext.ram
        """,
    )
    subparsers = parser.add_subparsers(dest="command", help="Command")

    # Pack
    p_pack = subparsers.add_parser("pack", help="Pack BL2 into bootext.ram")
    p_pack.add_argument("bl2", help="BL2 binary file")
    p_pack.add_argument("--cert", action='append',
                        help="Certificate file in DER format (can be repeated for multiple certs)")
    p_pack.add_argument("-o", "--output", default="bootext.ram", help="Output file (default: bootext.ram)")
    p_pack.add_argument("--serial", type=lambda x: int(x, 0), default=FIP_SERIAL_DEFAULT,
                        help=f"FIP serial number (default: 0x{FIP_SERIAL_DEFAULT:08X})")
    p_pack.add_argument("--align", type=lambda x: int(x, 0), default=FIP_ALIGN_DEFAULT,
                        help=f"Data alignment (default: 0x{FIP_ALIGN_DEFAULT:x})")
    p_pack.add_argument("--no-crc", action="store_true", help="Skip CRC (match original file)")
    p_pack.set_defaults(func=cmd_pack)

    # Unpack
    p_unpack = subparsers.add_parser("unpack", help="Unpack bootext.ram")
    p_unpack.add_argument("input", help="bootext.ram file")
    p_unpack.add_argument("-d", "--output-dir", default="bootext_extracted",
                          help="Output directory (default: bootext_extracted)")
    p_unpack.set_defaults(func=cmd_unpack)

    # Info
    p_info = subparsers.add_parser("info", help="Show FIP structure info")
    p_info.add_argument("input", help="bootext.ram file")
    p_info.set_defaults(func=cmd_info)

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()
