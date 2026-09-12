// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Mediatek failsafe board hooks (Web recovery / httpd upload).
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
 * Only the Mediatek specific policy lives here: the uploaded DRAM
 * staging buffer / RAM-boot fallback address and the firmware-type →
 * storage-target mapping.  The generic FIP / BL2 / image / storage
 * helpers live in failsafe/bootimg/ and are shared with the Airoha
 * board code.
 *
 * Target selection is driven by the failsafe_fw_t firmware type that the
 * Web UI derives from the uploaded form field:
 *   firmware    → "fit"   (OpenWrt kernel/rootfs FIT, UBI dynamic volume)
 *   bl2         → "bl2"   (MTD partition, FIP-mode devices only)
 *   fip         → "fip"   (UBI static volume, FIP-mode devices only)
 *   initramfs   → no flash target, RAM boot via boot_from_mem()
 */

#include <command.h>
#include <env.h>
#include <errno.h>
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

/*
 * Staging buffer for received uploads.
 *
 * ram_base + 64 MiB keeps the buffer clear of the ARM Trusted Firmware
 * (BL31 / secmon) no-map reserved area that starts at ram_base + 48 MiB
 * (0x43000000 on 0x40000000-based SoCs).  A buffer at ram_base + 32 MiB
 * used to overlap that area once an upload grew past 16 MiB, and writes
 * into the BL31 region were clobbered (observed as a zeroed hole in the
 * received image).  The original MediaTek httpd moves the buffer to
 * ram_base + 96 MiB for the same reason; 64 MiB is enough here while
 * still leaving room below the FAILSAFE_INITRAMFS_LOAD_FALLBACK address.
 */
#define FAILSAFE_UPLOAD_OFFSET	0x04000000

/* Fallback RAM address used to stage an uploaded RAM-boot image (a FIT
 * initramfs booted via bootm, or a raw image started with the "go"
 * command) when the "loadaddr" environment variable is not set.  It is
 * safely above the failsafe upload staging buffer (ram_base + 64 MiB) and
 * the kernel / dtb / initramfs load addresses on all current Mediatek SoCs.
 */
#define FAILSAFE_INITRAMFS_LOAD_FALLBACK	0x46000000

/*
 * Map a failsafe firmware type to the physical storage target name.
 *
 * The bootloader types are selected by the build mode (see
 * failsafe/Kconfig):
 *   - modern FIP devices expose "bl2" and "fip" → "bl2" MTD partition
 *     and "fip" UBI static volume
 *
 * Returns NULL for the RAM-boot case (initramfs FIT or raw "go" image;
 * no flash target).
 */
static const char *fw_to_target(failsafe_fw_t fw)
{
	switch (fw) {
	case FW_TYPE_FW:
		return FAILSAFE_STORAGE_FIT_TARGET;
	case FW_TYPE_BL2:
		return "bl2";
	case FW_TYPE_FIP:
		return FAILSAFE_STORAGE_STATIC_TARGET;
	case FW_TYPE_INITRD:
		return NULL;	/* RAM boot, no flash target */
	default:
		return NULL;
	}
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
	 * storage safety), regardless of CONFIG_MTK_FAILSAFE_VALIDATE.
	 * For MTD partitions the image must fit inside the partition;
	 * this is the single size gate for bl2 / fip.
	 */
	ret = failsafe_check_capacity(target, 0, size);
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
	if (env_get_yesno("failsafe_validate") == 0)
		return 0;

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

	return failsafe_storage_write(target, 0, data, size);
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
	if (!data || !size || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	/*
	 * Only a direct BL2 upload carries a preloader banner.  The
	 * MediaTek U-Boot image is not a BL2 container, so nothing is
	 * reported for it.
	 */
	if (fw != FW_TYPE_BL2)
		return -ENOENT;

	if (!failsafe_bl2_parse_banner(data, size, info))
		return -ENOENT;

	return 0;
}
