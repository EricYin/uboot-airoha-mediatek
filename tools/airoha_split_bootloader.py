#!/usr/bin/env python3
"""
split_bootloader.py - Split ECNT/Airoha Airoha bootloader image.

The mtd0-bootloader.img (a.k.a. bl1-bl2-bl31-uboot.bin) is a vendor-packed
512 KiB image containing
multiple boot stages compressed and laid out at fixed offsets:

  SoC BootROM
    -> BL1  (0x00000)  ARMv7 first-stage loader, verified by BootROM
    -> BL2  (0x00C00)  Pre-assembled bl2.bin: DDR/SPI-NAND init, LZMA decompress,
                       secure-boot certificate handling, BL31/U-Boot dispatch
    -> Cert (0x20000)  X.509 Trusted-Boot / Non-Trusted-Firmware certificates
    -> BL31 (0x21000)  LZMA-compressed ARMv8-A BL31 runtime firmware
    -> U-Boot (0x24800) LZMA-compressed U-Boot 2014.04-rc1 (BL33)
    -> Env  (0x7C000)  U-Boot environment variables

This script extracts each component, decompresses the LZMA payloads, and
optionally compares against reference binaries in the en7523/ directory.

Usage:
  python3 split_bootloader.py [mtd0-bootloader.img] [-o OUTPUT_DIR] [--ref-dir en7523]

Dependencies: Python 3.10+ (uses lzma, struct, pathlib, argparse)
"""

from __future__ import annotations

import argparse
import lzma
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Bootloader layout constants
# ---------------------------------------------------------------------------

BOOTLOADER_SIZE = 0x80000  # 512 KiB

# Component offsets and sizes within the bootloader image.
# Offsets are absolute from the start of the image.
BL1_OFFSET = 0x00000
BL1_MAX_SIZE = 0x01000  # BL1 occupies at most the first 4 KiB page

KEYAREA_OFFSET = 0x007E0  # Secure-boot key/hash table (between BL1 and BL2)
KEYAREA_END = 0x00C00

BL2_OFFSET = 0x00C00
BL2_REGION_END = 0x20000  # BL2 data extends up to the certificate region

CERT_OFFSET = 0x20000
CERT_END = 0x21000

BL31_LZMA_OFFSET = 0x21000
BL31_LZMA_END = 0x24800  # U-Boot LZMA starts here

UBOOT_LZMA_OFFSET = 0x24800
UBOOT_LZMA_END = 0x7C000  # U-Boot env starts here

ENV_OFFSET = 0x7C000
ENV_END = 0x80000  # End of image

LZMA_HEADER_SIZE = 13  # 1 byte props + 4 bytes dict_size + 8 bytes uncomp_size

# LZMA stream signature: props=0x5D, dict_size=0x00800000 (8 MiB) in little-endian
LZMA_SIGNATURE = b"\x5d\x00\x00\x80"


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class LzmaInfo:
    """Parsed LZMA stream header."""
    props: int
    dict_size: int
    uncomp_size: int
    compressed_size: int
    header_size: int = LZMA_HEADER_SIZE

    @property
    def total_size(self) -> int:
        return self.header_size + self.compressed_size


@dataclass
class Component:
    """A single extracted component."""
    name: str
    offset: int
    size: int
    data: bytes
    description: str = ""
    extra: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# LZMA helpers
# ---------------------------------------------------------------------------

def parse_lzma_header(data: bytes, offset: int = 0) -> tuple[int, int, int]:
    """Parse a raw LZMA1 header (props + dict_size + uncomp_size)."""
    props = data[offset]
    dict_size = struct.unpack_from("<I", data, offset + 1)[0]
    uncomp_size = struct.unpack_from("<Q", data, offset + 5)[0]
    return props, dict_size, uncomp_size


def decompress_lzma_raw(
    data: bytes,
    uncomp_size: int,
    dict_size: int,
    max_input: Optional[int] = None,
) -> tuple[bytes, int]:
    """
    Decompress a raw LZMA1 stream.

    Returns (decompressed_bytes, consumed_compressed_bytes).
    ``max_input`` limits how many compressed bytes to consider.
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
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)

    compressed = data[LZMA_HEADER_SIZE:]
    if max_input is not None:
        compressed = compressed[:max_input]

    result = dec.decompress(compressed, max_length=uncomp_size)
    unused = dec.unused_data
    consumed = len(compressed) - len(unused)

    return result, consumed


# ---------------------------------------------------------------------------
# CRC32 (matching the trx tool's implementation)
# ---------------------------------------------------------------------------

_CRC32_TABLE = [
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECFB0, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
]


def crc32_trx(data: bytes) -> int:
    """CRC32 matching the trx tool's crc32buf (poly 0xEDB88320, init 0xFFFFFFFF)."""
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC32_TABLE[((crc ^ b) & 0xFF)] ^ (crc >> 8)
    return crc


# ---------------------------------------------------------------------------
# String extraction
# ---------------------------------------------------------------------------

def extract_strings(data: bytes, min_len: int = 6, max_results: int = 50) -> list[tuple[int, str]]:
    """Extract ASCII strings from binary data."""
    results = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
        results.append((m.start(), m.group().decode("ascii", errors="replace")))
        if len(results) >= max_results:
            break
    return results


def find_strings_with_keywords(
    data: bytes, keywords: list[str], min_len: int = 6
) -> list[tuple[int, str]]:
    """Find ASCII strings containing any of the specified keywords (case-insensitive)."""
    results = []
    for off, text in extract_strings(data, min_len=min_len, max_results=500):
        if any(kw in text.lower() for kw in keywords):
            results.append((off, text))
    return results


# ---------------------------------------------------------------------------
# Bootloader parser
# ---------------------------------------------------------------------------

class BootloaderParser:
    """Parse and split an ECNT/Airoha EN7562 bootloader image."""

    def __init__(self, image_data: bytes):
        self.data = image_data
        self.size = len(image_data)
        self.components: list[Component] = []
        self.lzma_info: dict[str, LzmaInfo] = {}
        # Layout — may be overridden by auto-detection
        self.layout = {
            "bl2_region_end": BL2_REGION_END,
            "cert_offset": CERT_OFFSET,
            "cert_end": CERT_END,
            "bl31_lzma_offset": BL31_LZMA_OFFSET,
            "bl31_lzma_end": BL31_LZMA_END,
            "uboot_lzma_offset": UBOOT_LZMA_OFFSET,
            "uboot_lzma_end": UBOOT_LZMA_END,
        }

    def parse(self) -> None:
        """Run the full parsing pipeline."""
        self._detect_layout()
        self._extract_bl1()
        self._extract_key_area()
        self._extract_bl2()
        self._extract_certificates()
        self._extract_bl31()
        self._extract_uboot()
        self._extract_env()

    # -- Layout detection ---------------------------------------------------

    def _detect_layout(self) -> None:
        """
        Auto-detect the bootloader layout by scanning for LZMA streams
        and X.509 certificate patterns. This handles different firmware
        variants (e.g. sdg320 with 4 certificates and different offsets).
        """
        # Scan for LZMA streams (props=0x5D, dict=8MB) between BL2 end and env
        lzma_streams = []
        scan_start = 0x1F000  # Certificates/LZMA region starts around here
        scan_end = ENV_OFFSET

        for i in range(scan_start, scan_end - LZMA_HEADER_SIZE, 0x100):
            if self.data[i:i + 4] == LZMA_SIGNATURE:
                props = self.data[i]
                dict_size = struct.unpack_from("<I", self.data, i + 1)[0]
                uncomp_size = struct.unpack_from("<Q", self.data, i + 5)[0]
                # Validate: dict_size should be a reasonable power-of-2
                # and uncomp_size should be plausible (1KB - 4MB)
                if dict_size in (0x800000, 0x100000, 0x200000, 0x400000,
                                 0x10000, 0x1000000) and \
                   0x1000 <= uncomp_size <= 0x400000:
                    lzma_streams.append(i)

        if len(lzma_streams) >= 2:
            bl31_off = lzma_streams[0]
            uboot_off = lzma_streams[1]
            self.layout["bl31_lzma_offset"] = bl31_off
            self.layout["bl31_lzma_end"] = uboot_off
            self.layout["uboot_lzma_offset"] = uboot_off
            self.layout["uboot_lzma_end"] = ENV_OFFSET
            # BL2 extends up to the certificate region or first LZMA stream
            self.layout["bl2_region_end"] = bl31_off

        # Scan for X.509 DER certificates (0x30 0x82) to find cert region.
        # Note: certificate chain starts differ per SoC/firmware batch:
        #   EN7523/EN7562  -> 0x20000 (after the BL2 region)
        #   AN7581/AN7583  -> 0x1D000 (first certs embedded in "BL2 region")
        #   AN7563 (be5000)-> 0x1BC00 (TB_FW_CERT even earlier, right after
        #                             the BL2 code + padding)
        # Scanning from 0x1B000 catches all of the above, and the continuity
        # check below (a valid DER cert following right after) rejects false
        # positives inside BL2 code.
        # First collect every plausible DER-cert candidate, then walk the
        # chain: the cert region starts at the first candidate that is
        # followed by another valid cert, and every cert after it is taken
        # by jumping with the DER length (this keeps the final cert too,
        # which has no successor).
        candidates = []
        for i in range(0x1B000, scan_end - 4, 0x100):
            if self.data[i] == 0x30 and self.data[i + 1] == 0x82:
                cert_len = struct.unpack_from(">H", self.data, i + 2)[0]
                if 0x100 < cert_len < 0x1000:  # Reasonable cert size
                    candidates.append(i)

        cert_positions = []
        # Certificate slots in the flash image are aligned to 0x400 (same as
        # the FIP data alignment used by airoha_pack_bootext.py).
        for start in candidates:
            total = start + 4 + struct.unpack_from(">H", self.data, start + 2)[0]
            nxt = (total + 0x3FF) & ~0x3FF
            # Is this the start of a certificate chain (another cert follows)?
            if any(nxt <= j < nxt + 0x800 for j in candidates):
                # Walk the whole chain from the start
                pos = start
                while True:
                    cert_positions.append(pos)
                    total = pos + 4 + struct.unpack_from(">H", self.data, pos + 2)[0]
                    npos = (total + 0x3FF) & ~0x3FF
                    if npos in candidates:
                        pos = npos
                    else:
                        break
                break

        if cert_positions:
            cert_start = cert_positions[0]
            # Certificate region end is the last cert + its length, rounded up
            last_cert = cert_positions[-1]
            last_cert_len = struct.unpack_from(">H", self.data, last_cert + 2)[0] + 4
            cert_end = last_cert + last_cert_len
            # Round up to next 256-byte boundary
            cert_end = (cert_end + 0xFF) & ~0xFF
            self.layout["cert_offset"] = cert_start
            self.layout["cert_end"] = cert_end
            # BL2 region ends at the first certificate (certificates are
            # separate from BL2 code/data, placed at fixed flash offsets)
            if cert_start < self.layout["bl2_region_end"]:
                self.layout["bl2_region_end"] = cert_start

    # -- BL1 ---------------------------------------------------------------

    def _extract_bl1(self) -> None:
        """BL1 is the BootROM-verified first-stage loader at offset 0."""
        # BL1 size is determined by comparing with reference binaries.
        # EN7523 BL1 is 0x7E0 (2016) bytes; the actual image may differ slightly.
        # We scan for the BL2 start pattern to find the BL1 boundary.
        bl2_pattern = bytes([0x00, 0x90, 0xA0, 0xE1, 0x01, 0xA0, 0xA0, 0xE1])
        bl1_end = KEYAREA_OFFSET

        # Verify BL1 starts with ARMv7 NOP sled (0x00F020E3 = NOP)
        if self.data[:4] == b"\x00\xf0\x20\xe3":
            # BL1 confirmed. Find its actual size by looking for the gap
            # between BL1 code and the key area (zeros at 0x7E0).
            for end in range(0x600, 0x1000, 4):
                if all(b == 0 for b in self.data[end:end + 32]):
                    bl1_end = end
                    break

        bl1_data = self.data[BL1_OFFSET:bl1_end]
        self.components.append(Component(
            name="bl1",
            offset=BL1_OFFSET,
            size=len(bl1_data),
            data=bl1_data,
            description="BL1: BootROM-verified ARMv7 first-stage loader",
        ))

    # -- Key/Hash area ------------------------------------------------------

    def _extract_key_area(self) -> None:
        """Secure-boot key hash table between BL1 and BL2."""
        key_data = self.data[KEYAREA_OFFSET:KEYAREA_END]
        if all(b == 0 for b in key_data):
            return  # Skip if empty

        self.components.append(Component(
            name="key_area",
            offset=KEYAREA_OFFSET,
            size=len(key_data),
            data=key_data,
            description="Secure-boot key/hash table (between BL1 and BL2)",
        ))

    # -- BL2 ----------------------------------------------------------------

    def _extract_bl2(self) -> None:
        """BL2 is the pre-assembled bootloader firmware at offset 0xC00."""
        bl2_region_end = self.layout["bl2_region_end"]
        bl2_data = self.data[BL2_OFFSET:bl2_region_end]

        # Trim trailing zeros to get the actual BL2 binary size
        last_nonzero = len(bl2_data) - 1
        while last_nonzero >= 0 and bl2_data[last_nonzero] == 0:
            last_nonzero -= 1

        bl2_trimmed = bl2_data[:last_nonzero + 1]

        # Collect diagnostic strings
        strings = find_strings_with_keywords(
            bl2_trimmed,
            ["bl2", "bl31", "bl32", "exit", "boot", "lzma", "ddr", "spi",
             "nand", "secure", "ecnt", "en75", "cert", "trust"],
        )

        self.components.append(Component(
            name="bl2",
            offset=BL2_OFFSET,
            size=len(bl2_trimmed),
            data=bl2_trimmed,
            description="BL2: Pre-assembled bl2.bin (DDR init, SPI-NAND, LZMA, secure boot)",
            extra={
                "region_end": bl2_region_end,
                "padding": bl2_region_end - BL2_OFFSET - len(bl2_trimmed),
                "key_strings": strings[:10],
            },
        ))

    # -- Certificates -------------------------------------------------------

    def _extract_certificates(self) -> None:
        """X.509 certificates for trusted/non-trusted firmware verification."""
        cert_offset = self.layout["cert_offset"]
        cert_end = self.layout["cert_end"]
        cert_data = self.data[cert_offset:cert_end]

        # Find actual certificate data (DER-encoded X.509 starts with 0x30 0x82)
        certs = []
        pos = 0
        while pos < len(cert_data) - 4:
            if cert_data[pos] == 0x30 and cert_data[pos + 1] == 0x82:
                cert_len = struct.unpack_from(">H", cert_data, pos + 2)[0]
                cert_total = pos + 4 + cert_len
                if cert_total <= len(cert_data):
                    cert_bytes = cert_data[pos:cert_total]
                    # Try to identify the certificate type from strings
                    cert_type = "unknown"
                    for m in re.finditer(rb"[\x20-\x7e]{8,}", cert_bytes):
                        s = m.group().decode("ascii", errors="replace")
                        if "Certificate" in s:
                            cert_type = s.strip().rstrip("0")
                            break
                    certs.append((pos, cert_bytes, cert_type))
                    pos = cert_total
                    continue
            pos += 1

        cert_types = [c[2] for c in certs]
        cert_desc = " / ".join(cert_types) if cert_types else \
            "Trusted Boot FW / Non-Trusted Firmware Content Cert"

        self.components.append(Component(
            name="certificates",
            offset=cert_offset,
            size=len(cert_data),
            data=cert_data,
            description=f"X.509 certificates ({len(certs)} found): {cert_desc}",
            extra={"cert_count": len(certs), "cert_offsets": [c[0] for c in certs]},
        ))

    # -- BL31 (LZMA) --------------------------------------------------------

    def _extract_bl31(self) -> None:
        """BL31 runtime firmware, LZMA-compressed."""
        bl31_off = self.layout["bl31_lzma_offset"]
        bl31_end = self.layout["bl31_lzma_end"]
        region = self.data[bl31_off:bl31_end]

        props, dict_size, uncomp_size = parse_lzma_header(region)
        decompressed, consumed = decompress_lzma_raw(
            region, uncomp_size, dict_size,
            max_input=bl31_end - bl31_off - LZMA_HEADER_SIZE,
        )

        info = LzmaInfo(
            props=props,
            dict_size=dict_size,
            uncomp_size=uncomp_size,
            compressed_size=consumed,
        )
        self.lzma_info["bl31"] = info

        # LZMA compressed data (header + compressed payload)
        lzma_data = region[:LZMA_HEADER_SIZE + consumed]

        self.components.append(Component(
            name="bl31.lzma",
            offset=bl31_off,
            size=len(lzma_data),
            data=lzma_data,
            description=f"BL31 LZMA compressed (0x{consumed:x} -> 0x{uncomp_size:x} bytes)",
            extra={"lzma_info": info},
        ))

        self.components.append(Component(
            name="bl31",
            offset=bl31_off,
            size=len(decompressed),
            data=decompressed,
            description="BL31: ARMv8-A runtime firmware (decompressed)",
            extra={
                "is_decompressed": True,
                "key_strings": find_strings_with_keywords(
                    decompressed, ["bl31", "ecnt", "sip", "plat", "avs"]
                )[:5],
            },
        ))

    # -- U-Boot (LZMA) ------------------------------------------------------

    def _extract_uboot(self) -> None:
        """U-Boot (BL33), LZMA-compressed."""
        uboot_off = self.layout["uboot_lzma_offset"]
        uboot_end = self.layout["uboot_lzma_end"]
        region = self.data[uboot_off:uboot_end]

        props, dict_size, uncomp_size = parse_lzma_header(region)
        decompressed, consumed = decompress_lzma_raw(
            region, uncomp_size, dict_size,
            max_input=uboot_end - uboot_off - LZMA_HEADER_SIZE,
        )

        info = LzmaInfo(
            props=props,
            dict_size=dict_size,
            uncomp_size=uncomp_size,
            compressed_size=consumed,
        )
        self.lzma_info["uboot"] = info

        lzma_data = region[:LZMA_HEADER_SIZE + consumed]

        # Extract version string
        version = ""
        idx = decompressed.find(b"U-Boot 20")
        if idx >= 0:
            end = decompressed.find(b"\x00", idx)
            version = decompressed[idx:end].decode("ascii", errors="replace")

        self.components.append(Component(
            name="uboot.lzma",
            offset=uboot_off,
            size=len(lzma_data),
            data=lzma_data,
            description=f"U-Boot LZMA compressed (0x{consumed:x} -> 0x{uncomp_size:x} bytes)",
            extra={"lzma_info": info},
        ))

        self.components.append(Component(
            name="uboot",
            offset=uboot_off,
            size=len(decompressed),
            data=decompressed,
            description=f"U-Boot: {version}" if version else "U-Boot (decompressed)",
            extra={
                "is_decompressed": True,
                "version": version,
                "key_strings": find_strings_with_keywords(
                    decompressed, ["u-boot", "ecnt", "soc=", "arch=", "uboot_filename"]
                )[:5],
            },
        ))

    # -- U-Boot environment -------------------------------------------------

    def _extract_env(self) -> None:
        """U-Boot environment variables at offset 0x7C000."""
        env_raw = self.data[ENV_OFFSET:ENV_END]

        # Some images have a 4-byte CRC32 prefix before the env data.
        # Detect this by checking if the first 4 bytes are non-ASCII
        # followed by valid env key=value pairs.
        env_data = env_raw
        crc_prefix = None

        # Check for CRC prefix: first 4 bytes not starting with valid ASCII env
        if env_raw[:4] != b"\x00\x00\x00\x00":
            # Try parsing from offset 4 (skip potential CRC)
            env_from_4 = env_raw[4:]
            # Check if offset 4 looks like start of env (e.g., "arch=" or other key=)
            if re.match(rb"^[a-zA-Z_][a-zA-Z0-9_]*=", env_from_4[:32]):
                crc_prefix = env_raw[:4]
                env_data = env_from_4

        # Parse env as key=value pairs separated by NUL
        env_pairs = {}
        for s in env_data.split(b"\x00"):
            if b"=" in s:
                try:
                    key, val = s.decode("ascii").split("=", 1)
                    env_pairs[key] = val
                except (UnicodeDecodeError, ValueError):
                    pass

        desc = f"U-Boot environment ({len(env_pairs)} variables)"
        if crc_prefix is not None:
            desc += f", CRC32=0x{struct.unpack('<I', crc_prefix)[0]:08X}"

        self.components.append(Component(
            name="uboot.env",
            offset=ENV_OFFSET,
            size=len(env_raw),
            data=env_raw,
            description=desc,
            extra={"env_pairs": env_pairs, "crc_prefix": crc_prefix},
        ))

    # -- Reporting ----------------------------------------------------------

    def print_report(self) -> None:
        """Print a detailed analysis report."""
        print("=" * 72)
        print("  Airoha Bootloader Image Analysis")
        print("=" * 72)
        print(f"  Image size: {self.size} bytes (0x{self.size:x})")
        print()

        print("  Boot chain:")
        print("    SoC BootROM")
        print("      -> BL1 (ARMv7 first-stage, BootROM-verified)")
        print("         -> BL2 (DDR/SPI-NAND init, LZMA, secure boot)")
        print("            -> BL31 (ARMv8-A runtime firmware, LZMA)")
        print("               -> U-Boot / BL33 (LZMA)")
        print("                  -> Linux kernel (from tclinux)")
        print()

        print("  Component layout:")
        print(f"  {'Name':<16s} {'Offset':>10s} {'Size':>10s}  Description")
        print(f"  {'-'*16} {'-'*10} {'-'*10}  {'-'*40}")

        for comp in self.components:
            print(
                f"  {comp.name:<16s} 0x{comp.offset:08x} "
                f"0x{comp.size:08x}  {comp.description}"
            )

        print()

        # LZMA details
        if self.lzma_info:
            print("  LZMA compression details:")
            for name, info in self.lzma_info.items():
                ratio = info.uncomp_size / info.compressed_size if info.compressed_size else 0
                print(
                    f"    {name:8s}: props=0x{info.props:02x} "
                    f"dict={info.dict_size // 1024 // 1024}MB "
                    f"comp=0x{info.compressed_size:x} "
                    f"uncomp=0x{info.uncomp_size:x} "
                    f"ratio={ratio:.2f}:1"
                )
            print()

        # Key strings
        for comp in self.components:
            strings = comp.extra.get("key_strings", [])
            if strings:
                print(f"  Key strings in {comp.name}:")
                for off, text in strings:
                    print(f"    0x{off:06x}: {text[:65]}")
                print()

        # U-Boot env highlights
        for comp in self.components:
            if comp.name == "uboot.env":
                env = comp.extra.get("env_pairs", {})
                highlights = [
                    "arch", "cpu", "soc", "board", "vendor", "board_name",
                    "bootfile", "uboot_filename", "product_name", "loadaddr",
                ]
                print("  U-Boot environment highlights:")
                for key in highlights:
                    if key in env:
                        print(f"    {key} = {env[key]}")
                print()

        # CRC32 values
        print("  CRC32 checksums:")
        for comp in self.components:
            crc = crc32_trx(comp.data)
            print(f"    {comp.name:<16s}: 0x{crc:08x}")
        # Also CRC of the full image
        full_crc = crc32_trx(self.data)
        print(f"    {'full image':<16s}: 0x{full_crc:08x}")
        print()

    def save(self, output_dir: Path) -> None:
        """Save all components to the output directory."""
        output_dir.mkdir(parents=True, exist_ok=True)

        for comp in self.components:
            path = output_dir / f"{comp.name}.bin"
            path.write_bytes(comp.data)
            print(f"  saved: {path.name:20s}  "
                  f"0x{comp.offset:08x}  {len(comp.data):>8d} bytes")

        # Save a layout map
        layout_path = output_dir / "layout.txt"
        with layout_path.open("w") as f:
            f.write("Airoha Bootloader Component Layout\n")
            f.write("=" * 60 + "\n\n")
            for comp in self.components:
                f.write(f"{comp.name}.bin\n")
                f.write(f"  offset: 0x{comp.offset:08x}\n")
                f.write(f"  size:   0x{comp.size:08x} ({comp.size} bytes)\n")
                f.write(f"  desc:   {comp.description}\n")
                if "lzma_info" in comp.extra:
                    info = comp.extra["lzma_info"]
                    f.write(f"  lzma:   comp=0x{info.compressed_size:x} "
                            f"uncomp=0x{info.uncomp_size:x}\n")
                f.write("\n")

        print(f"\n  layout map: {layout_path.name}")


# ---------------------------------------------------------------------------
# Reference comparison
# ---------------------------------------------------------------------------

def compare_with_reference(
    components: list[Component],
    ref_dir: Path,
) -> None:
    """Compare extracted components with reference binaries."""
    ref_map = {
        "bl1": "en7523-bl1.bin",
        "bl2": "en7523-bl2.bin",
        "bl31": "en7523-bl31.bin",
    }

    comp_dict = {c.name: c for c in components}
    print("\n  Reference comparison (en7523/):")
    print(f"  {'Component':<12s} {'Extracted':>10s} {'Reference':>10s} "
          f"{'Match':>6s}  Note")
    print(f"  {'-'*12} {'-'*10} {'-'*10} {'-'*6}  {'-'*30}")

    for name, ref_file in ref_map.items():
        ref_path = ref_dir / ref_file
        if not ref_path.exists():
            print(f"  {name:<12s} {'?':>10s} {'missing':>10s} {'N/A':>6s}  "
                  f"reference file not found")
            continue

        ref_data = ref_path.read_bytes()
        comp = comp_dict.get(name)
        if comp is None:
            print(f"  {name:<12s} {'missing':>10s} {len(ref_data):>10d} {'N/A':>6s}")
            continue

        # Compare first N bytes
        compare_len = min(len(comp.data), len(ref_data))
        prefix_match = comp.data[:compare_len] == ref_data[:compare_len]
        exact_match = len(comp.data) == len(ref_data) and prefix_match

        if exact_match:
            note = "exact match"
        elif prefix_match:
            note = f"prefix matches, size differs (ref={len(ref_data)})"
        else:
            # Find first difference
            first_diff = 0
            for i in range(compare_len):
                if comp.data[i] != ref_data[i]:
                    first_diff = i
                    break
            note = f"differs at 0x{first_diff:x}"

        match_str = "YES" if exact_match else ("PREFIX" if prefix_match else "NO")
        print(f"  {name:<12s} {len(comp.data):>10d} {len(ref_data):>10d} "
              f"{match_str:>6s}  {note}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Split Airoha bootloader image into "
                    "BL1, BL2, BL31, U-Boot, certificates, and env.",
    )
    parser.add_argument(
        "image",
        type=Path,
        nargs="?",
        default=Path("mtd0-bootloader.img"),
        help="bootloader image file (default: mtd0-bootloader.img)",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=Path("split_bootloader"),
        help="output directory for extracted components (default: split_bootloader)",
    )
    parser.add_argument(
        "--ref-dir",
        type=Path,
        default=Path("en7523"),
        help="reference binary directory for comparison (default: en7523)",
    )
    parser.add_argument(
        "--no-save",
        action="store_true",
        help="do not save extracted files, only print analysis",
    )
    args = parser.parse_args()

    # Read image
    image_path = args.image.resolve()
    if not image_path.exists():
        print(f"error: image file not found: {image_path}", file=sys.stderr)
        sys.exit(1)

    image_data = image_path.read_bytes()
    if len(image_data) != BOOTLOADER_SIZE:
        print(f"warning: image size is {len(image_data)} (0x{len(image_data):x}), "
              f"expected {BOOTLOADER_SIZE} (0x{BOOTLOADER_SIZE:x})",
              file=sys.stderr)

    # Parse
    bp = BootloaderParser(image_data)
    bp.parse()

    # Report
    bp.print_report()

    # Compare with reference
    if args.ref_dir.exists():
        compare_with_reference(bp.components, args.ref_dir)
    else:
        print(f"\n  (reference directory {args.ref_dir} not found, skipping comparison)")

    # Save
    if not args.no_save:
        print(f"\n  Saving components to: {args.output}")
        bp.save(args.output)

    print()


if __name__ == "__main__":
    main()
