#!/usr/bin/env python3
"""pack_tcboot.py - Rebuild ECNT/Airoha tcboot images from split components.

This script reverses the layout used by ``split_bootloader.py`` and writes a
512 KiB ``tcboot`` / ``mtd0-bootloader.img`` style image.

It accepts the split files produced by the splitter:
  - bl1.bin
  - bl2.bin
  - bl31.bin
  - u-boot.bin (or uboot.bin)
  - certificates.bin
  - key_area.bin

Optional exact payload inputs can also be supplied:
  - bl31.lzma.bin
  - uboot.lzma.bin

If the LZMA payloads are not provided, the script will recompress the raw
``bl31.bin`` and ``u-boot.bin`` data into a valid LZMA-Alone stream so the
resulting image is bootable. If you want a byte-for-byte match with an original
image, pass the original ``*.lzma.bin`` payloads.

Typical usage:
  python pack_tcboot.py \
      --bl1 bl1.bin --key-area key_area.bin --bl2 bl2.bin \
      --certificates certificates.bin --bl31 bl31.bin --uboot u-boot.bin \
      -o tcboot.bin

  # Exact payload reuse when available
  python pack_tcboot.py \
      --bl1 bl1.bin --key-area key_area.bin --bl2 bl2.bin \
      --certificates certificates.bin --bl31-lzma bl31.lzma.bin \
      --uboot-lzma uboot.lzma.bin -o tcboot.bin

Layouts supported:
  - standard:  EN7523 DRAMC 0.2 style offsets
  - sdkax3000:    EN7523 DRAMC 0.6, larger cert region, BL31 and U-Boot start later
  - auto:      infer from certificate size

The final image is always 0x80000 bytes and is zero-filled outside the provided
segments.
"""

from __future__ import annotations

import argparse
import lzma
import struct
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


IMAGE_SIZE = 0x80000
BL1_OFFSET = 0x00000
KEY_AREA_OFFSET = 0x007E0
BL2_OFFSET = 0x00C00
CERT_OFFSET = 0x1E000
ENV_OFFSET = 0x7C000
ENV_END = 0x80000

# FIP structure: the original tcboot is a FIP image at 0x800 whose ToC
# (table of contents) declares the exact size of each component.  BL2
# parses this ToC (by UUID) and reads exactly the declared number of
# bytes, so the size fields MUST match the payloads actually placed.
FIP_BASE = 0x800
FIP_MAGIC = 0xAA640001
FIP_TOC_ENTRY_SIZE = 40  # uuid(16) + offset(8) + size(8) + flags(8)

# UUIDs from the ECNT platform (see ref bootloader ATF platform).
UUID_BL2 = "5ff9ec0b-4d22-3e4d-a544-c39d81c73f0a"
UUID_BL31 = "47d4086d-4cfe-9846-9b95-2950cbbd5a00"
UUID_BL33 = "d6d0eea7-fcea-d54b-9782-9934f234b6e4"


@dataclass(frozen=True)
class Layout:
    """Known tcboot layout."""

    name: str
    cert_offset: int
    bl31_offset: int
    uboot_offset: int
    env_offset: int = ENV_OFFSET
    image_size: int = IMAGE_SIZE

    @property
    def bl2_end(self) -> int:
        return self.cert_offset

    @property
    def cert_end(self) -> int:
        return self.bl31_offset

    @property
    def uboot_end(self) -> int:
        return self.env_offset


STANDARD_LAYOUT = Layout(
    name="en7523",
    cert_offset=0x1E000,
    bl31_offset=0x21000,
    uboot_offset=0x24800,
)

SDKAX3000_LAYOUT = Layout(
    name="en7562",
    cert_offset=0x1E000,
    bl31_offset=0x21400,
    uboot_offset=0x2B000,
)

LAYOUTS = {
    "standard": STANDARD_LAYOUT,
    "sdkax3000": SDKAX3000_LAYOUT,
}


def read_file(path: Optional[Path]) -> Optional[bytes]:
    if path is None:
        return None
    return path.read_bytes()


def lzma_stream_length(data: bytes) -> int:
    """Return the exact LZMA stream length (13-byte header + compressed data).

    The FIP ToC size field must match the actual stream length so that BL2
    reads exactly the right number of bytes.  Trailing padding is excluded
    by decompressing with FORMAT_ALONE and checking ``unused_data``.
    """
    try:
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        dec.decompress(data)
        return len(data) - len(dec.unused_data)
    except lzma.LZMAError:
        return len(data)


def patch_fip_toc_sizes(image: bytes, sizes: dict) -> bytes:
    """Update FIP ToC size fields to match the actual payload sizes.

    The tcboot is a FIP image: BL2 parses the FIP ToC (at 0x800) and loads
    each image by UUID, reading exactly the number of bytes declared in the
    ToC entry.  If the declared size is smaller than the real payload, the
    LZMA stream is truncated and decompression fails with
    SZ_ERROR_INPUT_EOF (the 'LZMA: res 6 state 3' error seen on hardware).

    ``sizes`` maps UUID strings (e.g. UUID_BL31) to the actual payload size.
    """
    buf = bytearray(image)

    if len(buf) < FIP_BASE + 16:
        raise ValueError("image too small for a FIP header")
    magic = struct.unpack_from("<I", buf, FIP_BASE)[0]
    if magic != FIP_MAGIC:
        raise ValueError(
            f"no FIP header at 0x{FIP_BASE:x} (magic=0x{magic:08x}); "
            "cannot patch ToC sizes — is key_area.bin the original FIP?"
        )

    pos = FIP_BASE + 16  # FIP header is 16 bytes: magic + version + flags
    updated = []
    while pos + FIP_TOC_ENTRY_SIZE <= len(buf):
        entry = buf[pos:pos + FIP_TOC_ENTRY_SIZE]
        uuid_bytes = bytes(entry[:16])
        if uuid_bytes == b"\x00" * 16:
            break  # terminator entry
        uuid_str = str(uuid.UUID(bytes=uuid_bytes))
        if uuid_str in sizes:
            old_size = struct.unpack_from("<Q", buf, pos + 24)[0]
            # Never shrink the declared size: BL2 reads exactly the declared
            # number of bytes, so a smaller value would truncate the payload.
            # Keeping the original size when the payload fits preserves
            # byte-for-byte reproduction of factory images.
            new_size = max(old_size, sizes[uuid_str])
            if new_size != old_size:
                struct.pack_into("<Q", buf, pos + 24, new_size)
                updated.append((uuid_str, old_size, new_size))
        pos += FIP_TOC_ENTRY_SIZE

    if updated:
        print("FIP ToC sizes updated (uuid: old -> new):")
        for u, old, new in updated:
            print(f"  {u}: 0x{old:x} -> 0x{new:x} ({new} bytes)")
    return bytes(buf)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def lzma_header_has_unknown_size(data: bytes) -> bool:
    """True when the LZMA-Alone header declares the -1 (unknown) size.

    Vendor LZMA streams (en7523/en7562/an7563) already carry the real
    uncompressed size, so they are used as-is without any probing.
    """
    return len(data) >= 13 and struct.unpack_from("<Q", data, 5)[0] == 0xFFFFFFFFFFFFFFFF


def fix_lzma_uncomp_size(data: bytes, known_uncomp_size: Optional[int] = None) -> bytes:
    """Patch the LZMA-Alone header to contain the actual uncompressed size.

    Modern ``xz`` / ``lzma`` tools set the uncompressed-size field in the
    13-byte LZMA-Alone header to 0xFFFFFFFFFFFFFFFF (-1) to indicate
    "unknown size".  However, some boot-ROM decompressors (e.g. the
    EN7523/EN7562 BL2 stage) require the exact size and will fail with
    "Failed to decompress image (err=1)" when they see -1.

    This function replaces bytes 5–12 with the correct little-endian size.
    If *known_uncomp_size* is ``None`` the current value is left untouched
    (no-op) — only the sentinel -1 triggers a rewrite.
    """
    if len(data) < 13:
        return data
    current = struct.unpack_from("<Q", data, 5)[0]
    if current != 0xFFFFFFFFFFFFFFFF:
        return data  # already has a real size
    if known_uncomp_size is None:
        return data  # caller didn't supply a known size – leave unchanged
    result = bytearray(data)
    struct.pack_into("<Q", result, 5, known_uncomp_size)
    return bytes(result)


def decompress_and_get_size(lzma_data: bytes) -> int:
    """Decompress a raw LZMA1 stream to find the uncompressed size.

    Used as a fallback when the LZMA-Alone header has -1 in the
    uncompressed-size field.  The caller passes the full 13-byte
    header + compressed data; we skip the header and decompress.
    """
    props = lzma_data[0]
    dict_size = struct.unpack_from("<I", lzma_data, 1)[0]
    filters = [
        {
            "id": lzma.FILTER_LZMA1,
            "dict_size": dict_size,
            "lc": props % 9,
            "lp": (props // 9) % 5,
            "pb": props // 45,
        }
    ]
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    result = dec.decompress(lzma_data[13:])
    return len(result)


def compress_lzma_alone(raw: bytes, dict_size: int = 0x800000) -> bytes:
    """Compress raw data using LZMA-Alone format (13-byte header).

    The uncompressed-size field in the header is patched to the real value
    so that BL2-stage decompressors that rely on it see the correct size.
    """
    filters = [
        {
            "id": lzma.FILTER_LZMA1,
            "dict_size": dict_size,
            "lc": 3,
            "lp": 0,
            "pb": 2,
        }
    ]
    result = lzma.compress(raw, format=lzma.FORMAT_ALONE, filters=filters)
    return fix_lzma_uncomp_size(result, known_uncomp_size=len(raw))


def ensure_exact_fit(name: str, data: bytes, offset: int, end: int) -> None:
    if offset > end:
        raise ValueError(f"{name} offset 0x{offset:x} is beyond the region end 0x{end:x}")
    if offset + len(data) > end:
        raise ValueError(
            f"{name} does not fit: size=0x{len(data):x}, region=0x{end - offset:x}, "
            f"offset=0x{offset:x}, end=0x{end:x}"
        )


def infer_layout(cert_size: int, explicit: Optional[str]) -> Layout:
    if explicit and explicit != "auto":
        try:
            return LAYOUTS[explicit]
        except KeyError as exc:
            raise ValueError(f"unknown layout: {explicit}") from exc

    # Heuristic based on observed certificate region sizes.
    # Standard images keep BL31 at 0x21000, while SDG320 pushes it to 0x21400.
    if cert_size >= 0x3300:
        return SDKAX3000_LAYOUT
    return STANDARD_LAYOUT


def build_image(
    bl1: bytes,
    key_area: bytes,
    bl2: bytes,
    certificates: bytes,
    bl31_payload: bytes,
    uboot_payload: bytes,
    env: Optional[bytes],
    layout: Layout,
) -> bytes:
    buf = bytearray(b"\x00" * layout.image_size)

    # Fixed regions.
    regions = [
        ("bl1", bl1, BL1_OFFSET, KEY_AREA_OFFSET),
        ("key_area", key_area, KEY_AREA_OFFSET, BL2_OFFSET),
        ("bl2", bl2, BL2_OFFSET, layout.cert_offset),
        ("certificates", certificates, layout.cert_offset, layout.cert_end),
        ("bl31", bl31_payload, layout.bl31_offset, layout.uboot_offset),
        ("uboot", uboot_payload, layout.uboot_offset, layout.env_offset),
    ]

    for name, data, start, end in regions:
        ensure_exact_fit(name, data, start, end)
        buf[start:start + len(data)] = data

    if env is not None:
        ensure_exact_fit("env", env, layout.env_offset, layout.image_size)
        buf[layout.env_offset:layout.env_offset + len(env)] = env

    return bytes(buf)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Rebuild tcboot / mtd0-bootloader.img from split components.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Rebuild from decompressed binaries
  python pack_tcboot.py --bl1 bl1.bin --key-area key_area.bin --bl2 bl2.bin \\
      --certificates certificates.bin --bl31 bl31.bin --uboot u-boot.bin \\
      -o tcboot.bin

  # Rebuild with original LZMA payloads for byte-preserving output
  python pack_tcboot.py --bl1 bl1.bin --key-area key_area.bin --bl2 bl2.bin \\
      --certificates certificates.bin --bl31-lzma bl31.lzma.bin --uboot-lzma uboot.lzma.bin \\
      -o tcboot.bin

  # Force SDG320 layout
  python pack_tcboot.py ... --layout sdkax3000
        """,
    )

    parser.add_argument("--bl1", type=Path, required=True, help="bl1.bin path")
    parser.add_argument("--fip", type=Path, help="prebuilt FIP (fiptool output); use with --bl1 for simple bl1+FIP+env assembly")
    parser.add_argument("--key-area", type=Path, help="key_area.bin path (required unless --fip)")
    parser.add_argument("--bl2", type=Path, help="bl2.bin path (required unless --fip)")
    parser.add_argument("--certificates", type=Path, help="certificates.bin path (required unless --fip)")

    parser.add_argument("--bl31", type=Path, help="decompressed bl31.bin path")
    parser.add_argument("--uboot", type=Path, help="decompressed u-boot.bin / uboot.bin path")

    parser.add_argument("--bl31-lzma", type=Path, help="original bl31.lzma.bin path")
    parser.add_argument("--uboot-lzma", type=Path, help="original uboot.lzma.bin path")
    parser.add_argument("--env", type=Path, help="optional uboot.env.bin path")

    parser.add_argument(
        "--layout",
        choices=["auto", "standard", "sdkax3000"],
        default="auto",
        help="layout preset (default: auto)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("tcboot.bin"),
        help="output tcboot image path (default: tcboot.bin)",
    )
    parser.add_argument(
        "--dict-size",
        type=lambda x: int(x, 0),
        default=0x800000,
        help="LZMA dictionary size used when recompressing (default: 0x800000)",
    )
    parser.add_argument(
        "--no-compress",
        action="store_true",
        help="do not recompress missing LZMA payloads; require --bl31-lzma and --uboot-lzma",
    )
    parser.add_argument(
        "--keep-unused-env-bytes",
        action="store_true",
        help="if --env is supplied, keep only its raw bytes; otherwise env area remains zero-filled",
    )
    parser.add_argument(
        "--no-env",
        action="store_true",
        help="exclude the env tail region (output ends at 0x7c000 instead of 0x80000)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    # ---- FIP assembly mode (bl1 + prebuilt FIP + optional env) ------
    if args.fip is not None:
        bl1 = read_file(args.bl1)
        fip = read_file(args.fip)
        env = read_file(args.env)
        if bl1 is None:
            raise SystemExit("--bl1 is required in --fip mode")
        if fip is None:
            raise SystemExit("--fip file not found")

        image_size = ENV_OFFSET if args.no_env else IMAGE_SIZE
        buf = bytearray(b"\x00" * image_size)
        # bl1 at offset 0, truncated if it exceeds 0x800
        bl1_len = min(len(bl1), FIP_BASE)
        buf[0:bl1_len] = bl1[:bl1_len]
        # FIP at offset 0x800
        buf[FIP_BASE:FIP_BASE + len(fip)] = fip
        # env at 0x7C000 (if provided and not no-env)
        if not args.no_env and env is not None:
            buf[ENV_OFFSET:ENV_OFFSET + len(env)] = env

        args.output.write_bytes(bytes(buf))
        print(f"output:      {args.output} ({len(buf)} bytes, 0x{len(buf):x})")
        print(f"bl1:         0x{0:05x}  0x{bl1_len:x}")
        print(f"fip:         0x{FIP_BASE:05x}  0x{len(fip):x}")
        if not args.no_env and env:
            print(f"env:         0x{ENV_OFFSET:05x}  0x{len(env):x}")
        elif not args.no_env:
            print(f"env:         0x{ENV_OFFSET:05x}  (zero-filled)")
        print("done")
        return

    # ---- Legacy mode: individual components at fixed offsets --------
    bl1 = read_file(args.bl1)
    key_area = read_file(args.key_area)
    bl2 = read_file(args.bl2)
    certificates = read_file(args.certificates)

    if bl1 is None or key_area is None or bl2 is None or certificates is None:
        raise SystemExit("missing required input files")

    if args.layout == "auto":
        layout = infer_layout(len(certificates), None)
    else:
        layout = infer_layout(len(certificates), args.layout)

    if args.no_env:
        layout.image_size = ENV_OFFSET

    # Raw payloads used only when original compressed streams are not supplied.
    bl31_raw = read_file(args.bl31)
    uboot_raw = read_file(args.uboot)
    env = None if args.no_env else read_file(args.env)

    if args.bl31_lzma:
        bl31_payload = read_file(args.bl31_lzma)
        assert bl31_payload is not None
        # Vendor streams already carry the real size; only fix -1 headers.
        if lzma_header_has_unknown_size(bl31_payload):
            if bl31_raw is not None:
                bl31_payload = fix_lzma_uncomp_size(bl31_payload, known_uncomp_size=len(bl31_raw))
            else:
                bl31_payload = fix_lzma_uncomp_size(
                    bl31_payload, known_uncomp_size=decompress_and_get_size(bl31_payload)
                )
    else:
        if bl31_raw is None:
            raise SystemExit("either --bl31 or --bl31-lzma must be provided")
        if args.no_compress:
            raise SystemExit("--no-compress set but --bl31-lzma was not provided")
        bl31_payload = compress_lzma_alone(bl31_raw, dict_size=args.dict_size)

    if args.uboot_lzma:
        uboot_payload = read_file(args.uboot_lzma)
        assert uboot_payload is not None
        # Only fix -1 headers; vendor streams are used as-is.
        if lzma_header_has_unknown_size(uboot_payload):
            if uboot_raw is not None:
                uboot_payload = fix_lzma_uncomp_size(uboot_payload, known_uncomp_size=len(uboot_raw))
            else:
                uboot_payload = fix_lzma_uncomp_size(
                    uboot_payload, known_uncomp_size=decompress_and_get_size(uboot_payload)
                )
    else:
        if uboot_raw is None:
            raise SystemExit("either --uboot or --uboot-lzma must be provided")
        if args.no_compress:
            raise SystemExit("--no-compress set but --uboot-lzma was not provided")
        uboot_payload = compress_lzma_alone(uboot_raw, dict_size=args.dict_size)

    # Validate that recompressed payloads fit into the reserved regions.
    ensure_exact_fit("bl31 payload", bl31_payload, layout.bl31_offset, layout.uboot_offset)
    ensure_exact_fit("uboot payload", uboot_payload, layout.uboot_offset, layout.env_offset)

    image = build_image(
        bl1=bl1,
        key_area=key_area,
        bl2=bl2,
        certificates=certificates,
        bl31_payload=bl31_payload,
        uboot_payload=uboot_payload,
        env=env,
        layout=layout,
    )

    # The tcboot is a FIP image: BL2 reads each component by UUID using the
    # size declared in the FIP ToC.  The ToC is carried in key_area.bin and
    # declares the ORIGINAL component sizes; patch it to the sizes actually
    # placed so BL2 never reads a truncated LZMA stream.
    image = patch_fip_toc_sizes(
        image,
        {
            UUID_BL2: len(bl2),
            UUID_BL31: lzma_stream_length(bl31_payload),
            UUID_BL33: lzma_stream_length(uboot_payload),
        },
    )

    args.output.write_bytes(image)

    print(f"layout:      {layout.name}")
    print(f"output:      {args.output} ({len(image)} bytes, 0x{len(image):x})")
    print(f"bl1:         0x{BL1_OFFSET:05x}  0x{len(bl1):x}")
    print(f"key_area:    0x{KEY_AREA_OFFSET:05x}  0x{len(key_area):x}")
    print(f"bl2:         0x{BL2_OFFSET:05x}  0x{len(bl2):x}")
    print(f"certs:       0x{layout.cert_offset:05x}  0x{len(certificates):x}")
    print(f"bl31:        0x{layout.bl31_offset:05x}  0x{len(bl31_payload):x}")
    print(f"uboot:       0x{layout.uboot_offset:05x}  0x{len(uboot_payload):x}")
    if env is not None:
        print(f"env:         0x{layout.env_offset:05x}  0x{len(env):x}")
    else:
        print(f"env:         0x{layout.env_offset:05x}  (zero-filled)")
    print("done")


if __name__ == "__main__":
    main()
