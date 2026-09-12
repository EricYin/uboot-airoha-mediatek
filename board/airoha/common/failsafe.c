// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Airoha failsafe board hooks (Web recovery / httpd upload).
 *
 * Covers the __weak hooks required by failsafe/failsafe_core.c and used
 * by failsafe/modules/upgrade.c:
 *   - httpd_get_upload_buffer_ptr()  — return a safe DRAM staging buffer
 *   - failsafe_validate_image()      — basic size check plus the generic
 *                                      storage capacity checks, then the
 *                                      structural image validation
 *   - failsafe_write_image()         — flash the staged image to the
 *                                      target partition (MTD) or UBI volume
 *   - boot_from_mem()                — boot an uploaded image from DRAM
 *   - failsafe_bl2_version_info()    — extract the preloader banner
 *
 * Only the Airoha specific policy lives here:
 *   - the uploaded DRAM staging buffer and the RAM-boot fallback address,
 *   - the firmware-type → storage-target mapping and the BL2 write
 *     offset (0x800, or 0 when the image already carries the BL1 head),
 *   - the BL2 banner extraction, which on Airoha has to locate the
 *     "tb-fw" payload inside the boot image FIP first.
 *
 * The generic FIP / BL2 / image / storage helpers live in
 * failsafe/bootimg/ and are shared with the MediaTek board code.
 *
 * Target selection is driven by the failsafe_fw_t firmware type that the
 * Web UI derives from the uploaded form field:
 *   firmware    → "fit"   (OpenWrt kernel/rootfs FIT, UBI dynamic volume)
 *   bl2         → "bl2"   (MTD partition, bootrom expects image at 0x800,
 *                          modern FIP devices only)
 *   chainloader → "chainloader" (MTD partition, either an OpenWrt-style
 *                          second-stage U-Boot FIT or a shim-based
 *                          legacy uImage, modern FIP devices only)
 *   uboot       → "u-boot" (MTD partition, legacy-image devices; the
 *                          512 KiB boot image, with or without BL1)
 *   fip         → "fip"   (UBI static volume, modern FIP devices only)
 *   initramfs   → no flash target, RAM boot via boot_from_mem()
 */

#include <command.h>
#include <env.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <failsafe/fw_type.h>
#include <failsafe/internal.h>
#include <failsafe/bl2.h>
#include <failsafe/image.h>
#include <failsafe/storage.h>

#include "failsafe_validate.h"

DECLARE_GLOBAL_DATA_PTR;

/* Staging buffer: 32 MiB past ram_base, safely above U-Boot's working
 * set on all current Airoha SoCs (AN7563, AN7581, EN7523 family, etc.).
 */
#define FAILSAFE_UPLOAD_OFFSET	0x02000000

/* Fallback RAM address used to stage an uploaded RAM-boot image (a FIT
 * initramfs booted via bootm, or a raw image started with the "go"
 * command) when the "loadaddr" environment variable is not set.  It is
 * safely above the failsafe upload staging buffer (ram_base + 32 MiB) and
 * the kernel / dtb / initramfs load addresses on all current Airoha SoCs.
 */
#define FAILSAFE_INITRAMFS_LOAD_FALLBACK	0x89400000

/* Airoha BL2 (preloader) write offset.
 *
 * By default the Airoha bootrom expects the BL2 image 0x800 (2 KiB) into
 * the "bl2" MTD partition, with the leading bytes left at 0xFF.  The
 * default environment writes it exactly that way:
 *
 *     mw.b $loadaddr 0xff 0x800
 *     setexpr loadaddr_bl2 $loadaddr + 0x800
 *     tftpboot $loadaddr_bl2 $bootfile_bl2 && mtd write bl2 $loadaddr
 *
 * "mtd erase bl2" already leaves the partition at 0xFF, so the Web
 * failsafe only needs to start writing at this offset.
 *
 * With CONFIG_AIROHA_PRELOADER_BL1 the flashed artifact is
 * bl1-preloader.bin, which already carries the 2 KiB BL1 region in front
 * of the BL2 FIP.  Writing that image at 0x800 would push the FIP to
 * 0x1000, so it has to be written from offset 0 instead.
 */
#ifdef CONFIG_AIROHA_PRELOADER_BL1
#define FAILSAFE_BL2_WRITE_OFFSET	0
#else
#define FAILSAFE_BL2_WRITE_OFFSET	0x800
#endif

/* Offset of the internal FIP in the legacy 512 KiB boot image: the 2 KiB
 * BL1 (or zero) prefix always precedes it (see tools/airoha_pack_boot.sh).
 */
#define FAILSAFE_LEGACY_FIP_OFFSET	0x800

/* FIP offsets probed for the BL2 "tb-fw" payload, in order: the split
 * preloader.bin keeps the FIP at 0, the legacy image at 0x800.
 */
static const size_t airoha_bl2_fip_offsets[] = {
	0,
	FAILSAFE_LEGACY_FIP_OFFSET,
};

/*
 * Map a failsafe firmware type to the physical storage target name.
 *
 * The two bootloader types are mutually exclusive and are selected by the
 * build mode (see failsafe/Kconfig):
 *   - devices built with the legacy 512 KiB image expose "uboot" →
 *     "u-boot" MTD partition (with BL1: BL1 + internal FIP @0x800;
 *     without it - AN7563 - the same self-contained FIP behind a 2 KiB
 *     zero prefix)
 *   - modern FIP devices expose "bl2", "chainloader" and "fip" → "bl2"
 *     MTD partition, "chainloader" MTD partition (OpenWrt-style second
 *     stage U-Boot FIT) and "fip" UBI static volume
 *
 * Returns NULL for the RAM-boot initramfs case (no flash target).
 */
static const char *fw_to_target(failsafe_fw_t fw)
{
	switch (fw) {
	case FW_TYPE_FW:
		return FAILSAFE_STORAGE_FIT_TARGET;
	case FW_TYPE_BL2:
		return "bl2";
	case FW_TYPE_CHAINLOADER:
		return "chainloader";
	case FW_TYPE_UBOOT:
		return "u-boot";
	case FW_TYPE_FIP:
		return FAILSAFE_STORAGE_STATIC_TARGET;
	case FW_TYPE_INITRD:
		return NULL;	/* RAM boot, no flash target */
	default:
		return NULL;
	}
}

/*
 * Return the offset (in bytes) within the MTD partition where the image
 * must be written.  Most partitions are written from offset 0; the Airoha
 * "bl2" partition is the exception (see FAILSAFE_BL2_WRITE_OFFSET).
 */
static u64 failsafe_mtd_write_offset(const char *target)
{
	if (!strcmp(target, "bl2"))
		return FAILSAFE_BL2_WRITE_OFFSET;

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Public hooks – called by failsafe/failsafe_core.c                 */
/* ------------------------------------------------------------------ */

void *httpd_get_upload_buffer_ptr(size_t size)
{
	return (void *)(uintptr_t)(gd->ram_base + FAILSAFE_UPLOAD_OFFSET);
}

int failsafe_validate_image(const void *data, size_t size, failsafe_fw_t fw)
{
	const char *target = fw_to_target(fw);
	int ret;

	if (!size) {
		printf("Failsafe: empty image\n");
		return -EINVAL;
	}

	/* RAM boot (initramfs FIT or raw "go" image): no flash partition,
	 * so only the empty-image check above applies.
	 */
	if (!target)
		return 0;

	/* Generic storage capacity checks - always performed (basic
	 * storage safety), regardless of CONFIG_AIROHA_FAILSAFE_VALIDATE.
	 * For MTD partitions the image, including the write offset, must
	 * fit inside the partition; this is the single size gate for
	 * bl2 / u-boot / chainloader.
	 */
	ret = failsafe_check_capacity(target,
				      failsafe_mtd_write_offset(target), size);
	if (ret)
		return ret;

	/* Structural validation - the standalone validation module
	 * (failsafe_validate.c, no-op stub when the master switch is off)
	 * can additionally be turned off at runtime by setting the
	 * "failsafe_validate" environment variable to 0/no; the generic
	 * storage capacity checks above are always performed regardless of
	 * this switch.  When the variable is unset, validation stays
	 * enabled (default).
	 */
#if IS_ENABLED(CONFIG_AIROHA_FAILSAFE_VALIDATE)
	if (env_get_yesno("failsafe_validate") == 0) {
		printf("Failsafe: structural image validation disabled by "
		       "the 'failsafe_validate' environment variable\n");
		return 0;
	}
#endif

	return failsafe_validate_image_content(data, size, fw);
}

int failsafe_write_image(const void *data, size_t size, failsafe_fw_t fw)
{
	const char *target = fw_to_target(fw);

	/* RAM boot (initramfs FIT or raw "go" image): nothing to flash */
	if (!target) {
		printf("Failsafe: no flash target for firmware type %d\n", fw);
		return -EINVAL;
	}

	return failsafe_storage_write(target, failsafe_mtd_write_offset(target),
				      data, size);
}

/*
 * Boot an image previously uploaded by the failsafe Web UI directly from
 * DRAM (RAM boot).  The uploaded image is classified by its header: FIT /
 * legacy uImage images (an initramfs) are booted with bootm, any other
 * image is treated as a raw binary and executed with the "go" command at
 * the same address.
 */
int boot_from_mem(ulong data_load_addr)
{
	return failsafe_boot_image_from_mem(data_load_addr, upload_size,
					    FAILSAFE_INITRAMFS_LOAD_FALLBACK);
}

int failsafe_bl2_version_info(const void *data, size_t size,
			      failsafe_fw_t fw, struct failsafe_bl2_info *info)
{
	const void *bl2;
	size_t bl2_size;

	if (!data || !size || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	/*
	 * Only images that really carry a preloader: a direct BL2 upload
	 * and the legacy 512 KiB U-Boot image (BL2 + BL31 + U-Boot packed
	 * into one internal FIP).  The modern split FIP
	 * (bl31-uboot.fip) holds no BL2 at all.
	 */
	if (fw != FW_TYPE_BL2 && fw != FW_TYPE_UBOOT)
		return -ENOENT;

	/* Restrict the banner scan to the FIP "tb-fw" entry so the BL31 /
	 * U-Boot strings do not leak into the result.
	 */
	failsafe_bl2_locate(data, size, airoha_bl2_fip_offsets,
			    ARRAY_SIZE(airoha_bl2_fip_offsets), &bl2,
			    &bl2_size);
	if (!failsafe_bl2_parse_banner(bl2, bl2_size, info))
		return -ENOENT;

	return 0;
}
