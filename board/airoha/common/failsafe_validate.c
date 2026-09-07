// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Airoha failsafe image validation (Web recovery / httpd upload).
 *
 * Standalone validation module used by board/airoha/common/failsafe.c.
 * Every uploaded firmware image is structurally checked before it is
 * flashed.  The generic storage capacity checks (MTD partition size /
 * 'fip' static volume size) are done by failsafe.c
 * (failsafe_validate_image / failsafe_check_mtd_capacity) and are
 * always performed, independently of this module's master switch.
 *
 * The whole module is controlled by the master switch
 * CONFIG_AIROHA_FAILSAFE_VALIDATE ("Failsafe image validation" menu in
 * board/airoha/Kconfig).  Per-image-type checks can be enabled or
 * disabled individually with:
 *   u-boot (legacy image)      CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT
 *   bl2               CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2
 *   fip               CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP
 *   chainloader       CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER
 *   firmware          CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE
 *
 * The layout decides which checks apply:
 *   - legacy layout: 'u-boot' carries the whole boot chain (BL1 + BL2 +
 *     BL31 + U-Boot) inside one image, so its validator (FIP ToC @0x800)
 *     inherently covers BL2 too; there is no standalone 'bl2' check.
 *     Without BL1 (CONFIG_AIROHA_LEGACY_BL1 disabled, e.g. AN7563) the
 *     same self-contained FIP sits behind a 2 KiB zero prefix - the legacy
 *     image without BL1.  The prefix is only there to keep the FIP at the
 *     same offset as in the image with BL1, so both flavours are validated
 *     as a FIP ToC @0x800.
 *   - FIP mode: BL2 lives in its own partition / image (preloader.bin),
 *     so CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 always includes the deep
 *     LZMA-layout + trailing CRC32 check for bare preloaders.
 */

#include <errno.h>
#include <linux/string.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <asm/byteorder.h>
#include <image.h>
#include <u-boot/crc.h>
#include <failsafe/fw_type.h>

#include "failsafe_validate.h"

/* 'u-boot' layout: the internal FIP ToC is at 0x800 in both variants -
 * with BL1 (BL1 @0x0, FIP @0x800, env @0x7c000) and without BL1 (2 KiB
 * zero prefix, FIP @0x800).  See tools/airoha_pack_boot.sh. */
#define AIROHA_FAILSAFE_LEGACY_FIP_OFF		0x800

/* FIP ToC header: magic(le32) + version(le32) + flags(le64) = 16 bytes;
 * each ToC entry: uuid(16) + offset(le64) + size(le64) + flags(le64) = 40. */
#define AIROHA_FAILSAFE_FIP_HDR_SIZE		16
#define AIROHA_FAILSAFE_FIP_TOC_ENTRY_SIZE	40
#define AIROHA_FAILSAFE_FIP_MAGIC			0xAA640001

/*
 * FIP ToC UUID of the BL2 payload.  The 'bl2' image flashed by the
 * failsafe UI is preloader.bin, produced by:
 *
 *   $(FIPTOOL) create --tb-fw $(BL2_BIN) preloader.bin
 *
 * i.e. a FIP container whose single entry carries the raw preloader.
 * UUID 5ff9ec0b-4d22-3e4d-a544-c39d81c73f0a.  In the FIP ToC the uuid
 * is stored as a uuid_t (big-endian time_low/time_mid/time_hi fields,
 * node id bytes in order), i.e. the canonical text byte order:
 *   5f f9 ec 0b | 4d 22 | 3e 4d | a5 44 | c3 9d 81 c7 3f 0a
 */
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
static const u8 airoha_fip_uuid_bl2[16] = {
	0x5f, 0xf9, 0xec, 0x0b, 0x4d, 0x22, 0x3e, 0x4d,
	0xa5, 0x44, 0xc3, 0x9d, 0x81, 0xc7, 0x3f, 0x0a,
};
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 */

/* 'bl2' (preloader) LZMA layout (AN7563/AN7581/AN7583):
 *
 *   [stage-1 BL21 @0x0] [opt header @0x3800] [BL22 LZMA @0x3820]
 *   [BL23 LZMA] [flash-table LZMA] [CRC32 @ size-4]
 *
 * The optimization header (little-endian) holds the BL22/BL23/flash-table
 * compressed sizes in fields 0-2; all segments are back-to-back.  The
 * trailing CRC32 is the raw accumulator without the final one's
 * complement ("no final XOR" format).  Struct-layout boards (EN7523
 * family) embed the flash table at build time and have no opt header or
 * trailing CRC, so they are skipped automatically.
 */
#define AIROHA_FAILSAFE_BL2_MIN_SIZE		0x4000
#define AIROHA_FAILSAFE_BL2_OPT_HDR_OFF		0x3800
#define AIROHA_FAILSAFE_BL2_LZMA_DEF_OFF	0x3820

/* ------------------------------------------------------------------ */
/*  Per-type image validators                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT) || \
    defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2) || \
    defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP)
/*
 * Validate a FIP (Firmware Image Package) whose header starts at
 * data + fip_off:
 *   - header magic 0xAA640001,
 *   - ToC entries walkable within the image,
 *   - the wanted entry (want_uuid, FIP byte order) carries a payload
 *     fully contained in the image.
 * Pass want_uuid == NULL to accept the ToC as long as it holds at least
 * one non-terminal entry.  FIP payload offsets are relative to the FIP
 * start, hence fip_off + off + len must fit in the image.
 */
static int failsafe_validate_fip_toc(const void *data, size_t size,
				     size_t fip_off, const u8 *want_uuid,
				     const char *what)
{
	const u8 *base = (const u8 *)data + fip_off;
	u32 magic;
	size_t avail, pos;
	int found = 0;

	if (fip_off > size ||
	    size - fip_off < AIROHA_FAILSAFE_FIP_HDR_SIZE +
			     AIROHA_FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		printf("Failsafe: '%s' image too small for a FIP\n", what);
		return -EINVAL;
	}

	magic = le32_to_cpu(*(const __le32 *)base);
	if (magic != AIROHA_FAILSAFE_FIP_MAGIC) {
		printf("Failsafe: '%s' image has no FIP ToC (magic 0x%08x)\n",
		       what, magic);
		return -EINVAL;
	}

	avail = size - fip_off - AIROHA_FAILSAFE_FIP_HDR_SIZE;
	for (pos = 0; pos + AIROHA_FAILSAFE_FIP_TOC_ENTRY_SIZE <= avail;
	     pos += AIROHA_FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		const u8 *e = base + AIROHA_FAILSAFE_FIP_HDR_SIZE + pos;
		u64 off, len;
		int i, terminal = 1;

		/* A terminal entry has an all-zero UUID. */
		for (i = 0; i < 16; i++)
			if (e[i]) {
				terminal = 0;
				break;
			}
		if (terminal)
			break;

		if (!want_uuid || !memcmp(e, want_uuid, 16)) {
			off = le64_to_cpu(*(const __le64 *)(e + 16));
			len = le64_to_cpu(*(const __le64 *)(e + 24));
			found = 1;
			if (fip_off + off + len > size) {
				printf("Failsafe: '%s' payload (off 0x%llx, "
				       "len 0x%llx) exceeds image (%zu)\n",
				       what, (unsigned long long)off,
				       (unsigned long long)len, size);
				return -EINVAL;
			}
		}
	}

	if (want_uuid && !found) {
		printf("Failsafe: '%s' FIP has no expected image entry\n",
		       what);
		return -EINVAL;
	}

	return 0;
}
#endif

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
/*
 * Detect the AN7xxx LZMA BL2 layout from the optimization header: the
 * three compressed sizes (BL22, BL23, flash table) must be back-to-back
 * and end exactly at size - 4 (the trailing CRC32 slot).  On success the
 * stored CRC is returned in *crc.  Returns false for struct-layout BL2
 * images (EN7523 family), which have no opt header / trailing CRC.
 */
static bool bl2_lzma_layout_ok(const u8 *data, size_t size, u32 *crc)
{
	const u8 *h = data + AIROHA_FAILSAFE_BL2_OPT_HDR_OFF;
	u32 bl22, bl23, ft;
	u64 end;

	if (size < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF + 0x24 + 4)
		return false;

	bl22 = le32_to_cpu(*(const __le32 *)(h + 0));
	bl23 = le32_to_cpu(*(const __le32 *)(h + 4));
	ft   = le32_to_cpu(*(const __le32 *)(h + 8));

	/* Plausible size ranges (mirrors tools/airoha_info_preloader.py) */
	if (bl22 <= 0x1000 || bl22 >= 0x200000 ||
	    bl23 <= 0x1000 || bl23 >= 0x200000 ||
	    ft <= 0x100 || ft >= 0x20000)
		return false;

	end = (u64)AIROHA_FAILSAFE_BL2_LZMA_DEF_OFF + bl22 + bl23 + ft;
	if (end != size - 4)
		return false;

	*crc = le32_to_cpu(*(const __le32 *)(data + size - 4));
	return true;
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT)
static int failsafe_validate_uboot(const void *data, size_t size)
{
	/* The 'u-boot' MTD partition holds either the legacy image with BL1
	 * (BL1 @0x0 + internal FIP @0x800) or, when BL1 is left out
	 * (CONFIG_AIROHA_LEGACY_BL1 disabled, e.g. AN7563), the same
	 * internal FIP (BL2 + BL31 + U-Boot) behind a 2 KiB zero prefix
	 * instead of BL1.  tools/airoha_pack_boot.sh writes the FIP at
	 * 0x800 in both cases, so both are validated as a FIP ToC @0x800.
	 * Both carry the whole boot chain in one image, so this check
	 * inherently covers BL2 too.  Size is bounded by the generic
	 * MTD partition capacity check in
	 * failsafe_validate_image_content() (must fit the 'u-boot'
	 * partition), not by a fixed constant.
	 */
	return failsafe_validate_fip_toc(data, size,
					 AIROHA_FAILSAFE_LEGACY_FIP_OFF,
					 NULL, "u-boot");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
static int failsafe_validate_bl2(const void *data, size_t size)
{
	u32 magic;

	if (size < AIROHA_FAILSAFE_BL2_MIN_SIZE) {
		printf("Failsafe: 'bl2' image too small (%zu < 0x%zx)\n",
		       size, (size_t)AIROHA_FAILSAFE_BL2_MIN_SIZE);
		return -EINVAL;
	}

	magic = le32_to_cpu(*(const __le32 *)data);

	if (magic == AIROHA_FAILSAFE_FIP_MAGIC) {
		/* preloader.bin: FIP container wrapping the raw preloader
		 * ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin).  Validate
		 * the ToC and that the BL2 payload lies inside the image.
		 */
		return failsafe_validate_fip_toc(data, size, 0,
						 airoha_fip_uuid_bl2, "bl2");
	}

	/* A corrupted preloader.bin with a bad header magic still carries
	 * the BL2 ToC entry at offset 16; a genuine bare preloader would
	 * never match the 128-bit UUID there by chance. */
	if (size >= AIROHA_FAILSAFE_FIP_HDR_SIZE +
		   AIROHA_FAILSAFE_FIP_TOC_ENTRY_SIZE &&
	    !memcmp((const u8 *)data + AIROHA_FAILSAFE_FIP_HDR_SIZE,
		    airoha_fip_uuid_bl2, 16)) {
		printf("Failsafe: 'bl2' image looks like a FIP with a bad "
		       "header (BL2 ToC entry present, magic 0x%08x)\n",
		       magic);
		return -EINVAL;
	}

	{
		/* Bare preloader (legacy bootext images): LZMA layout
		 * deep check (AN7563/AN7581/AN7583).  Always performed
		 * when BL2 validation is enabled (FIP mode). */
		const u8 *s1 = data;
		u32 stored, calc;
		size_t i, nz = 0;

		if (bl2_lzma_layout_ok(data, size, &stored)) {
			/* stage-1 (BL21) region must not be blank. */
			for (i = 0; i < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF; i++)
				if (s1[i] != 0x00 && s1[i] != 0xFF)
					nz++;
			if (nz < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF / 8) {
				printf("Failsafe: 'bl2' stage-1 (BL21) "
				       "region blank\n");
				return -EINVAL;
			}

			/* Airoha stores the raw CRC accumulator without the
			 * final one's complement that crc32() adds, so XOR
			 * it back out for comparison. */
			calc = crc32(0, data, size - 4) ^ 0xFFFFFFFF;
			if (calc != stored) {
				printf("Failsafe: 'bl2' CRC32 mismatch "
				       "(stored 0x%08x, calc 0x%08x)\n",
				       stored, calc);
				return -EINVAL;
			}
		}
		/* Struct layout (EN7523 family): no opt header / trailing
		 * CRC; the size checks above are the only gate. */
	}

	return 0;
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP)
static int failsafe_validate_fip(const void *data, size_t size)
{
	/* 'fip' = UBI volume with a FIP (BL31+U-Boot, or the legacy
	 * BL2+BL31+U-Boot single-FIP).  Any non-empty ToC is accepted.
	 */
	return failsafe_validate_fip_toc(data, size, 0, NULL, "fip");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER) || \
    defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE)
/* Shared FIT checker for the FIT-based image types. */
static int failsafe_validate_fit(const void *data, size_t size,
				 const char *what)
{
	if (size < 4) {
		printf("Failsafe: '%s' image too small (%zu)\n", what, size);
		return -EINVAL;
	}

	if (fit_check_format(data, size)) {
		printf("Failsafe: '%s' image is not a valid FIT\n", what);
		return -EINVAL;
	}

	return 0;
}
#endif

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER)
/*
 * Validate a legacy uImage (shim-based chainloader):
 * 64-byte header CRC (ih_hcrc) plus payload CRC (ih_dcrc), after
 * verifying the declared payload size fits inside the image.
 */
static int failsafe_validate_legacy_uiimage(const void *data, size_t size,
					    const char *what)
{
	const struct legacy_img_hdr *hdr = data;
	size_t dsize;

	if (size < image_get_header_size()) {
		printf("Failsafe: '%s' legacy image too small (%zu)\n",
		       what, size);
		return -EINVAL;
	}
	if (!image_check_magic(hdr)) {
		printf("Failsafe: '%s' has no legacy uImage magic\n", what);
		return -EINVAL;
	}
	if (!image_check_hcrc(hdr)) {
		printf("Failsafe: '%s' legacy header CRC mismatch\n", what);
		return -EINVAL;
	}

	dsize = image_get_data_size(hdr);
	if (dsize > size - image_get_header_size()) {
		printf("Failsafe: '%s' legacy payload (0x%zx) exceeds "
		       "image (%zu)\n", what, dsize, size);
		return -EINVAL;
	}
	if (!image_check_dcrc(hdr)) {
		printf("Failsafe: '%s' legacy payload CRC mismatch\n", what);
		return -EINVAL;
	}

	return 0;
}

static int failsafe_validate_chainloader(const void *data, size_t size)
{
	const struct legacy_img_hdr *hdr = data;

	/* The chainloader partition may hold either a legacy uImage
	 * (shim-based packing) or an OpenWrt-style FIT;
	 * dispatch on the header magic. */
	if (size >= image_get_header_size() && image_check_magic(hdr))
		return failsafe_validate_legacy_uiimage(data, size,
							"chainloader");

	return failsafe_validate_fit(data, size, "chainloader");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE)
static int failsafe_validate_firmware(const void *data, size_t size)
{
	return failsafe_validate_fit(data, size, "firmware");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE */

/* ------------------------------------------------------------------ */
/*  Public entry point                                                */
/* ------------------------------------------------------------------ */

int failsafe_validate_image_content(const void *data, size_t size,
				    failsafe_fw_t fw)
{
	int ret;

	/* Per-type structural validation.  Each validator is gated by its
	 * own Kconfig toggle (board/airoha/Kconfig, "Failsafe image
	 * validation").  The generic storage capacity checks are done by
	 * failsafe_validate_image() in failsafe.c, always enabled.
	 */
	switch (fw) {
	case FW_TYPE_UBOOT:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT)
		ret = failsafe_validate_uboot(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_BL2:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
		ret = failsafe_validate_bl2(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_FIP:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP)
		ret = failsafe_validate_fip(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_CHAINLOADER:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER)
		ret = failsafe_validate_chainloader(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_FW:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE)
		ret = failsafe_validate_firmware(data, size);
		if (ret)
			return ret;
#endif
		break;
	default:
		break;
	}

	return 0;
}
