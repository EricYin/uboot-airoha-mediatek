#!/usr/bin/env python3
"""Batch convert exported .der certificates to .pem files, and print as much readable information as possible.

By default, it reads the *.der files in the `certificates_der/` directory and outputs them to the `certificates_pem/` directory.

If the environment has `cryptography` installed, the script will additionally print:
- Subject
- Issuer
- Serial Number
- Validity

Otherwise, the script can still complete the DER -> PEM conversion.
"""

from __future__ import annotations

import argparse
import base64
from datetime import datetime
from pathlib import Path
from typing import Iterable


def der_to_pem(der_bytes: bytes) -> bytes:
    """Wrap a DER-encoded certificate into PEM format."""
    b64 = base64.encodebytes(der_bytes).replace(b"\n", b"")
    lines = [b"-----BEGIN CERTIFICATE-----"]
    for i in range(0, len(b64), 64):
        lines.append(b64[i:i + 64])
    lines.append(b"-----END CERTIFICATE-----")
    lines.append(b"")
    return b"\n".join(lines)


def iter_der_files(input_path: Path) -> Iterable[Path]:
    if input_path.is_file():
        if input_path.suffix.lower() == ".der":
            yield input_path
        return

    if input_path.is_dir():
        yield from sorted(input_path.glob("*.der"))


def try_parse_cert(der_bytes: bytes):
    """Attempt to parse the certificate information; return None if unsuccessful."""
    try:
        from cryptography import x509
    except Exception:
        return None

    try:
        return x509.load_der_x509_certificate(der_bytes)
    except Exception:
        return None


def format_dt(dt: datetime) -> str:
    if dt.tzinfo is None:
        return dt.isoformat(sep=" ")
    return dt.astimezone().isoformat(sep=" ")


def get_cert_validity(cert) -> tuple[datetime, datetime]:
    """Compatible with the validity period fields in both new and old versions of the `cryptography` library."""
    not_before = getattr(cert, "not_valid_before_utc", None)
    if not_before is None:
        not_before = cert.not_valid_before

    not_after = getattr(cert, "not_valid_after_utc", None)
    if not_after is None:
        not_after = cert.not_valid_after

    return not_before, not_after


def process_certificates(input_path: Path, output_dir: Path) -> int:
    cert_files = list(iter_der_files(input_path))
    if not cert_files:
        raise SystemExit(f".der certificate not found: {input_path}")

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Input: {input_path}")
    print(f"Output: {output_dir}")
    print(f"Number of certificates: {len(cert_files)}")
    print()

    for idx, der_path in enumerate(cert_files, 1):
        der_bytes = der_path.read_bytes()
        pem_bytes = der_to_pem(der_bytes)

        out_path = output_dir / (der_path.stem + ".pem")
        out_path.write_bytes(pem_bytes)

        print(f"[{idx:02d}] {der_path.name} -> {out_path.name}")
        print(f"     Size: {len(der_bytes)} bytes")

        cert = try_parse_cert(der_bytes)
        if cert is not None:
            try:
                subject = cert.subject.rfc4514_string()
            except Exception:
                subject = str(cert.subject)
            try:
                issuer = cert.issuer.rfc4514_string()
            except Exception:
                issuer = str(cert.issuer)

            print(f"     Subject: {subject}")
            print(f"     Issuer : {issuer}")
            print(f"     Serial : {cert.serial_number}")
            not_before, not_after = get_cert_validity(cert)
            print(f"     Valid  : {format_dt(not_before)}  ->  {format_dt(not_after)}")
        else:
            print("     [i] 'cryptography' is not installed; skipping certificate field parsing.")

        print()

    return len(cert_files)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Batch convert .der certificates to .pem and print the subject/issuer information whenever possible."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="certificates_der",
        help="Input .der file or directory (default: certificates_der)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="certificates_pem",
        help="Output directory (default: certificates_pem)",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output)

    if not input_path.exists():
        raise SystemExit(f"Input path not found: {input_path}")

    process_certificates(input_path, output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
