#!/usr/bin/env python3
"""Export each certificate as a .der file from a concatenated X.509 DER certificate block.

This is suitable for files like `certificates.bin`, which typically contain multiple certificates concatenated together.

Each certificate begins with `0x30 0x82` followed by 2 bytes of big-endian length.

Example:
  python extract_certificates.py certificates.bin -o out_der
  python extract_certificates.py split_bootloader/certificates.bin --expected 6
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def find_der_certificates(blob: bytes) -> list[tuple[int, bytes]]:
    """Scan for consecutive DER data and return [(offset, cert_bytes), ...]."""
    results: list[tuple[int, bytes]] = []
    pos = 0

    while pos <= len(blob) - 4:
        if blob[pos] == 0x30 and blob[pos + 1] == 0x82:
            cert_len = struct.unpack_from(">H", blob, pos + 2)[0]
            total_len = 4 + cert_len

            if cert_len > 0 and pos + total_len <= len(blob):
                cert = blob[pos : pos + total_len]
                results.append((pos, cert))
                pos += total_len
                continue

        pos += 1

    return results


def extract_certificates(input_path: Path, output_dir: Path, expected: int | None = None) -> int:
    data = input_path.read_bytes()
    certs = find_der_certificates(data)

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Input file: {input_path}")
    print(f"Size: {len(data)} bytes")
    print(f"Find the certificate: {len(certs)}")

    if expected is not None and len(certs) != expected:
        print(f"[!] Expected {expected} certificates, but actually found {len(certs)}")

    for idx, (offset, cert) in enumerate(certs, 1):
        out_path = output_dir / f"cert_{idx:02d}.der"
        out_path.write_bytes(cert)
        print(f"  #{idx:02d} offset=0x{offset:05X} size={len(cert)} -> {out_path}")

    return len(certs)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Extract each certificate from the concatenated DER certificate blocks and save it as a .der file."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="split_bootloader/certificates.bin",
        help="Enter the path to certificates.bin (default: split_bootloader/certificates.bin)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="certificates_der",
        help="Output directory (default: certificates_der)",
    )
    parser.add_argument(
        "--expected",
        type=int,
        default=6,
        help="Expected number of exported certificates (default: 6)",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output)

    if not input_path.is_file():
        raise SystemExit(f"Input file not found: {input_path}")

    extract_certificates(input_path, output_dir, args.expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
