// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Mediatek failsafe image validation (Web recovery / httpd upload).
 *
 * The whole module is controlled by the master switch
 * CONFIG_MTK_FAILSAFE_VALIDATE ("Failsafe image validation" menu in
 * board/mediatek/Kconfig).  Per-image-type checks can be enabled or
 * disabled individually with:
 *   bl2               CONFIG_MTK_FAILSAFE_VALIDATE_BL2
 *   fip               CONFIG_MTK_FAILSAFE_VALIDATE_FIP
 *   firmware          CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE
 *
 * The layout decides which checks apply:
 *   - FIP mode: BL2 lives in its own partition / image (preloader.bin),
 *     so CONFIG_MTK_FAILSAFE_VALIDATE_BL2 always includes the raw
 *     preloader header check (SF_BOOT / SPINAND! / NANDCFG! /
 *     EMMC_BOOT / SDMMC_BOOT) and, when the preloader is delivered as a
 *     FIP container ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin),
 *     a FIP ToC check for the BL2 payload.
 *   - 'fip' is a UBI static volume holding a FIP (BL31 + U-Boot, or the
 *     legacy BL2+BL31+U-Boot single-FIP), validated as a FIP ToC.
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

/* ------------------------------------------------------------------ */
/*  FIP ToC parsing                                                    */
/* ------------------------------------------------------------------ */

/* FIP ToC header: magic(le32) + serial_number(le32) + flags(le64) = 16 bytes;
 * each ToC entry: uuid(16) + offset(le64) + size(le64) + flags(le64) = 40. */
#define MTK_FAILSAFE_FIP_HDR_SIZE		16
#define MTK_FAILSAFE_FIP_TOC_ENTRY_SIZE	40
#define MTK_FAILSAFE_FIP_MAGIC			0xAA640001

/*
 * FIP ToC UUID of the BL2 payload.  The 'bl2' image flashed by the
 * failsafe UI is preloader.bin, produced by:
 *
 *   $(FIPTOOL) create --tb-fw $(BL2_BIN) preloader.bin
 *
 * i.e. a FIP container whose single entry carries the raw preloader.
 * UUID_TRUSTED_BOOT_FIRMWARE_BL2 is 5ff9ec0b-4d22-3e4d-a544-c39d81c73f0a.
 * In the FIP ToC the uuid is stored in the canonical text byte order:
 *   5f f9 ec 0b | 4d 22 | 3e 4d | a5 44 | c3 9d 81 c7 3f 0a
 */
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
static const u8 mtk_fip_uuid_bl2[16] = {
	0x5f, 0xf9, 0xec, 0x0b, 0x4d, 0x22, 0x3e, 0x4d,
	0xa5, 0x44, 0xc3, 0x9d, 0x81, 0xc7, 0x3f, 0x0a,
};
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_BL2 */

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
static int mtk_validate_fip_toc(const void *data, size_t size,
				size_t fip_off, const u8 *want_uuid,
				const char *what)
{
	const u8 *base = (const u8 *)data + fip_off;
	u32 magic;
	size_t avail, pos;
	int found = 0;

	if (fip_off > size ||
	    size - fip_off < MTK_FAILSAFE_FIP_HDR_SIZE +
			     MTK_FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		printf("Failsafe: '%s' image too small for a FIP\n", what);
		return -EINVAL;
	}

	magic = le32_to_cpu(*(const __le32 *)base);
	if (magic != MTK_FAILSAFE_FIP_MAGIC) {
		printf("Failsafe: '%s' image has no FIP ToC (magic 0x%08x)\n",
		       what, magic);
		return -EINVAL;
	}

	avail = size - fip_off - MTK_FAILSAFE_FIP_HDR_SIZE;
	for (pos = 0; pos + MTK_FAILSAFE_FIP_TOC_ENTRY_SIZE <= avail;
	     pos += MTK_FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		const u8 *e = base + MTK_FAILSAFE_FIP_HDR_SIZE + pos;
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

/* ------------------------------------------------------------------ */
/*  BL2 (preloader) header detection                                   */
/* ------------------------------------------------------------------ */

/* The raw Mediatek preloader (boot ROM image) starts with one of these
 * magic headers at offset 0 (mirrors the reference bl2_helper.h).  Only
 * the first 8 bytes are compared, as the tail varies across preloader
 * versions. */
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
#define MTK_BL2_HDR_SIZE		8

static const u8 mtk_bl2_hdr_sf_nor[8] = {
	0x53, 0x46, 0x5f, 0x42, 0x4f, 0x4f, 0x54, 0x00,	/* "SF_BOOT\0" */
};
static const u8 mtk_bl2_hdr_spi_nand[8] = {
	0x53, 0x50, 0x49, 0x4e, 0x41, 0x4e, 0x44, 0x21,	/* "SPINAND!" */
};
static const u8 mtk_bl2_hdr_snfi_nand[8] = {
	0x4e, 0x41, 0x4e, 0x44, 0x43, 0x46, 0x47, 0x21,	/* "NANDCFG!" */
};
static const u8 mtk_bl2_hdr_emmc[8] = {
	0x45, 0x4d, 0x4d, 0x43, 0x5f, 0x42, 0x4f, 0x4f,	/* "EMMC_BOOT" */
};
static const u8 mtk_bl2_hdr_sd[8] = {
	0x53, 0x44, 0x4d, 0x4d, 0x43, 0x5f, 0x42, 0x4f,	/* "SDMMC_BOOT" */
};

/*
 * Return the name of the storage type a raw preloader is built for, or
 * NULL if 'data' does not carry any known Mediatek preloader magic.
 */
static const char *mtk_bl2_storage_name(const void *data, size_t size)
{
	static const u8 *const magics[] = {
		mtk_bl2_hdr_sf_nor, mtk_bl2_hdr_spi_nand,
		mtk_bl2_hdr_snfi_nand, mtk_bl2_hdr_emmc, mtk_bl2_hdr_sd,
	};
	static const char *const names[] = {
		"spim-nor", "spim-nand", "snfi-nand", "emmc", "sd",
	};
	int i;

	if (size < MTK_BL2_HDR_SIZE)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(magics); i++)
		if (!memcmp(data, magics[i], MTK_BL2_HDR_SIZE))
			return names[i];

	return NULL;
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_BL2 */

/* ------------------------------------------------------------------ */
/*  Per-type image validators                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
static int failsafe_validate_bl2(const void *data, size_t size)
{
	u32 magic;

	if (size < MTK_FAILSAFE_FIP_HDR_SIZE) {
		printf("Failsafe: 'bl2' image too small (%zu)\n", size);
		return -EINVAL;
	}

	magic = le32_to_cpu(*(const __le32 *)data);

	/* preloader.bin delivered as a FIP container wrapping the raw
	 * preloader ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin).
	 * Validate the ToC and that the BL2 payload lies inside the image. */
	if (magic == MTK_FAILSAFE_FIP_MAGIC)
		return mtk_validate_fip_toc(data, size, 0,
					    mtk_fip_uuid_bl2, "bl2");

	/* A corrupted preloader.bin with a bad header magic still carries
	 * the BL2 ToC entry at offset 16; a genuine raw preloader would
	 * never match the 128-bit UUID there by chance. */
	if (size >= MTK_FAILSAFE_FIP_HDR_SIZE +
		   MTK_FAILSAFE_FIP_TOC_ENTRY_SIZE &&
	    !memcmp((const u8 *)data + MTK_FAILSAFE_FIP_HDR_SIZE,
		    mtk_fip_uuid_bl2, 16)) {
		printf("Failsafe: 'bl2' image looks like a FIP with a bad "
		       "header (BL2 ToC entry present, magic 0x%08x)\n",
		       magic);
		return -EINVAL;
	}

	/* Bare preloader: must carry one of the known Mediatek storage
	 * magic headers. */
	if (!mtk_bl2_storage_name(data, size)) {
		printf("Failsafe: 'bl2' image has no known preloader "
		       "header (magic 0x%08x)\n", magic);
		return -EINVAL;
	}

	return 0;
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIP)
static int failsafe_validate_fip(const void *data, size_t size)
{
	/* 'fip' = UBI volume with a FIP (BL31+U-Boot, or the legacy
	 * BL2+BL31+U-Boot single-FIP).  Any non-empty ToC is accepted.
	 */
	return mtk_validate_fip_toc(data, size, 0, NULL, "fip");
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_FIP */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE)
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

static int failsafe_validate_firmware(const void *data, size_t size)
{
	return failsafe_validate_fit(data, size, "firmware");
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE */

/* ------------------------------------------------------------------ */
/*  Public entry point                                                */
/* ------------------------------------------------------------------ */

int failsafe_validate_image_content(const void *data, size_t size,
				    failsafe_fw_t fw)
{
	int ret;

	/* Per-type structural validation.  Each validator is gated by its
	 * own Kconfig toggle (board/mediatek/Kconfig, "Failsafe image
	 * validation").  The generic storage capacity checks are done by
	 * failsafe_validate_image() in failsafe.c, always enabled.
	 */
	switch (fw) {
	case FW_TYPE_BL2:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
		ret = failsafe_validate_bl2(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_FIP:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIP)
		ret = failsafe_validate_fip(data, size);
		if (ret)
			return ret;
#endif
		break;
	case FW_TYPE_FW:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE)
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
