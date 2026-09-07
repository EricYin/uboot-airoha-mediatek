// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Mediatek failsafe image writer (Web recovery / httpd upload).
 *
 * Covers the __weak hooks required by failsafe/failsafe_core.c and used
 * by failsafe/modules/upgrade.c:
 *   - httpd_get_upload_buffer_ptr()  — return a safe DRAM staging buffer
 *   - failsafe_write_image()         — flash the staged image to the
 *                                      target partition (MTD) or UBI volume
 *   - boot_from_mem()                — boot an uploaded image from DRAM:
 *                                      FIT / legacy images via bootm, raw
 *                                      images via the "go" command
 *
 * Target selection is driven by the failsafe_fw_t firmware type that the
 * Web UI derives from the uploaded form field:
 *   firmware    → "fit"   (OpenWrt kernel/rootfs FIT, UBI dynamic volume)
 *   bl2         → "bl2"   (MTD partition, FIP-mode devices only)
 *   fip         → "fip"   (UBI static volume, FIP-mode devices only)
 *   initramfs   → no flash target, RAM boot via boot_from_mem()
 *
 * The target is automatically probed:
 *   • If an MTD partition with that name exists → MTD erase + write.
 *   • Otherwise → treated as a UBI volume: the "rootfs_data" volume is
 *     removed first to free space (mirroring "run ubi_remove_rootfs" in
 *     the console upgrade commands), the old volume is removed (if
 *     present), a new volume is created, and the image is written with
 *     "ubi write".
 */

#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <image.h>
#include <linux/string.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <failsafe/fw_type.h>

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

/* Kernel / rootfs upgrade (OpenWrt UBI layout): kernel / rootfs resides
 * in a dynamic UBI volume named "fit" (FIT image containing kernel + dtb
 * + initramfs).
 */
#define FAILSAFE_FW_TARGET	"fit"

/*
 * Storage target shared by the failsafe write path (failsafe.c) and the
 * validation module (failsafe_validate.c): the "fip" volume is a UBI
 * static volume created at the fixed size FAILSAFE_FIP_VOL_SIZE,
 * so the capacity check in failsafe.c must know both the target name
 * and the reserved size.
 */
#define FAILSAFE_STATIC_TARGET	"fip"
#define FAILSAFE_FIP_VOL_SIZE	0x100000

/* Size of the uploaded initramfs image, tracked by failsafe/modules/
 * upgrade.c.  Used by boot_from_mem() to stage the image at loadaddr.
 */
extern size_t upload_size;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/*
 * Probe whether 'name' refers to a known MTD partition.
 *
 * This is used to decide between the MTD and UBI write path: if an MTD
 * partition with that name exists we take the mtd erase + mtd write
 * path; otherwise we treat it as a UBI volume.
 */
static bool is_mtd_partition(const char *name)
{
	struct mtd_info *mtd;

	mtd_probe_devices();

	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return false;

	put_mtd_device(mtd);
	return true;
}

/*
 * Generic MTD partition capacity check: the uploaded image must fit entirely
 * inside the target MTD partition.
 *
 * This is the single size gate for the MTD-backed bootloader targets
 * (bl2 / u-boot / chainloader): the image size must not exceed the MTD
 * partition size.  There are no fixed image size limits.
 *
 * Defined here, in the failsafe write path, because it is a basic
 * storage-safety check; the structural validation module
 * (failsafe_validate.c) calls it via failsafe_validate.h for the
 * MTD-backed targets.
 */
int failsafe_check_mtd_capacity(const char *target, u64 off, size_t size)
{
	struct mtd_info *mtd;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(target);
	if (IS_ERR_OR_NULL(mtd)) {
		printf("Failsafe: MTD partition '%s' not found\n", target);
		return -ENODEV;
	}

	if (off + (u64)size > mtd->size) {
		printf("Failsafe: image (%zu) exceeds partition '%s' "
		       "(%llu), write offset 0x%llx\n",
		       size, target, (unsigned long long)mtd->size,
		       (unsigned long long)off);
		put_mtd_device(mtd);
		return -EFBIG;
	}

	put_mtd_device(mtd);
	return 0;
}

/*
 * Map a failsafe firmware type to the physical storage target name.
 *
 * The two bootloader types are mutually exclusive and are selected by the
 * build mode (see failsafe/Kconfig):
 *   - devices built with the legacy 512 KiB image expose "uboot" →
 *     "u-boot" MTD partition (boards built without BL1 - AN7563 - use the
 *     same target with the self-contained BL2+BL31+U-Boot FIP at 0x800)
 *   - modern FIP devices expose "bl2", "chainloader" and "fip" → "bl2"
 *     MTD partition, "chainloader" MTD partition (OpenWrt-style second
 *     stage U-Boot FIT) and "fip" UBI static volume
 *
 * Returns NULL for the RAM-boot case (initramfs FIT or raw "go" image;
 * no flash target).
 */
static const char *fw_to_target(failsafe_fw_t fw)
{
	switch (fw) {
	case FW_TYPE_FW:
		return FAILSAFE_FW_TARGET;
	case FW_TYPE_BL2:
		return "bl2";
	case FW_TYPE_FIP:
		return FAILSAFE_STATIC_TARGET;
	case FW_TYPE_INITRD:
		return NULL;	/* RAM boot, no flash target */
	default:
		return NULL;
	}
}

/*
 * Ensure UBI is attached to the UBI MTD partition (usually "ubi").
 * Safe to call even if already attached.
 */
static int ubi_ensure_attached(void)
{
	return run_command("ubi part ubi", 0);
}

/*
 * Recreate the OpenWrt "rootfs_data" UBI volume during a FIT upgrade.
 *
 * The rootfs_data volume holds the writable overlay (user config).  After
 * a FIT upgrade we always rebuild it from scratch:
 *   - detect whether it already exists ("ubi check"),
 *   - if it exists, remove it first ("ubi remove"),
 *   - then create a fresh dynamic volume spanning all remaining space
 *     ("-" = maximum available size).
 */
static int failsafe_recreate_rootfs_data(void)
{
	char cmd[256];
	int ret;

	/* Detect: does rootfs_data already exist? */
	snprintf(cmd, sizeof(cmd), "ubi check rootfs_data");
	ret = run_command(cmd, 0);
	if (!ret) {
		/* Exists -> remove it first */
		snprintf(cmd, sizeof(cmd), "ubi remove rootfs_data");
		ret = run_command(cmd, 0);
		if (ret) {
			printf("Failsafe: remove 'rootfs_data' failed "
			       "(ret=%d)\n", ret);
			return -EIO;
		}
	}

	/* Create a fresh dynamic volume with the maximum available size */
	snprintf(cmd, sizeof(cmd), "ubi create rootfs_data - dynamic");
	ret = run_command(cmd, 0);
	if (ret) {
		printf("Failsafe: 'ubi create rootfs_data' failed (ret=%d)\n",
		       ret);
		return -EIO;
	}

	printf("Failsafe: 'rootfs_data' recreated\n");
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
	 * storage safety), regardless of CONFIG_MTK_FAILSAFE_VALIDATE.
	 * For MTD partitions the image must fit inside the partition;
	 * this is the single size gate for bl2 / u-boot / chainloader.
	 */
	if (is_mtd_partition(target)) {
		ret = failsafe_check_mtd_capacity(target, 0, size);
		if (ret)
			return ret;
	} else if (!strcmp(target, FAILSAFE_STATIC_TARGET) &&
		   size > FAILSAFE_FIP_VOL_SIZE) {
		/* The "fip" static volume is created at the fixed
		 * FAILSAFE_FIP_VOL_SIZE (see failsafe_write_image),
		 * so reject images that would not fit - otherwise
		 * "ubi write" fails after the volume has already been
		 * recreated empty.
		 */
		printf("Failsafe: '%s' image too big (%zu > 0x%zx)\n",
		       target, size, (size_t)FAILSAFE_FIP_VOL_SIZE);
		return -EFBIG;
	}

	/* Structural validation - delegated to the standalone validation
	 * module (failsafe_validate.c).  It can be turned off at runtime
	 * by setting the "failsafe_validate" environment variable to 0/no;
	 * the generic storage capacity checks above are always performed
	 * regardless of this switch.  When the variable is unset,
	 * validation stays enabled (default).
	 */
	if (env_get_yesno("failsafe_validate") == 0)
		return 0;

#ifdef MTK_FAILSAFE_VALIDATE
	return failsafe_validate_image_content(data, size, fw);
#else
	return 0;
#endif
}

int failsafe_write_image(const void *data, size_t size, failsafe_fw_t fw)
{
	const char *target = fw_to_target(fw);
	char cmd[256];
	int ret;

	/* RAM boot (initramfs FIT or raw "go" image): nothing to flash */
	if (!target) {
		printf("Failsafe: no flash target for firmware type %d\n", fw);
		return -EINVAL;
	}

	printf("\n*** Failsafe upgrade: %zu (0x%zx) bytes -> '%s' ***\n\n",
	       size, size, target);

	if (is_mtd_partition(target)) {
		/* ----- MTD partition ----- */
		snprintf(cmd, sizeof(cmd), "mtd erase %s", target);
		ret = run_command(cmd, 0);
		if (ret) {
			printf("Failsafe: erase '%s' failed (ret=%d)\n",
			       target, ret);
			return -EIO;
		}

		snprintf(cmd, sizeof(cmd), "mtd write %s 0x%lx 0x0 0x%lx",
			 target, (unsigned long)(uintptr_t)data,
			 (unsigned long)size);
		ret = run_command(cmd, 0);
		if (ret) {
			printf("Failsafe: write '%s' failed (ret=%d)\n",
			       target, ret);
			return -EIO;
		}

	} else {
		/* ----- UBI volume ----- */
		ret = ubi_ensure_attached();
		if (ret) {
			printf("Failsafe: cannot attach UBI (ret=%d)\n", ret);
			return -EIO;
		}

		/* Remove old volume if it exists (best-effort). */
		snprintf(cmd, sizeof(cmd),
			 "ubi check %s && ubi remove %s",
			 target, target);
		run_command(cmd, 0);

		/*
		 * Create the new volume.  "fip" is a static volume created
		 * at the fixed FAILSAFE_FIP_VOL_SIZE (0x100000),
		 * exactly like "ubi create fip 0x100000 static" from the
		 * console "ubi_write_fip"; "ubi write" later records the
		 * actual image size as used_bytes.  All other UBI targets
		 * are dynamic volumes sized to the uploaded image
		 * ("ubi create fit $filesize dynamic").
		 */
		if (!strcmp(target, FAILSAFE_STATIC_TARGET))
			snprintf(cmd, sizeof(cmd),
				 "ubi create %s 0x%zx static", target,
				 (size_t)FAILSAFE_FIP_VOL_SIZE);
		else
			snprintf(cmd, sizeof(cmd),
				 "ubi create %s 0x%zx dynamic", target, size);
		ret = run_command(cmd, 0);
		if (ret) {
			printf("Failsafe: 'ubi create %s' failed (ret=%d)\n",
			       target, ret);
			return -EIO;
		}

		snprintf(cmd, sizeof(cmd),
			 "ubi write 0x%lx %s 0x%lx",
			 (unsigned long)(uintptr_t)data,
			 target, (unsigned long)size);
		ret = run_command(cmd, 0);
		if (ret) {
			printf("Failsafe: 'ubi write %s' failed (ret=%d)\n",
			       target, ret);
			return -EIO;
		}

		/*
		 * FIT upgrade: always rebuild the OpenWrt "rootfs_data"
		 * volume (remove if present, then create) so the device
		 * boots with a fresh overlay.
		 */
		if (!strcmp(target, FAILSAFE_FW_TARGET)) {
			ret = failsafe_recreate_rootfs_data();
			if (ret)
				return ret;
		}
	}

	printf("\n*** Failsafe upgrade completed ('%s') ***\n\n", target);
	return 0;
}

/*
 * Boot an image previously uploaded by the failsafe Web UI directly from
 * DRAM (RAM boot).  The uploaded image is classified by its header:
 *
 *   - FIT / legacy uImage images (an initramfs or another image understood
 *     by bootm) are booted with bootm;
 *   - any other image is treated as a raw binary and executed with the
 *     "go" command at the same address.
 *
 * The image is staged at the address from the "loadaddr" environment
 * variable (FAILSAFE_INITRAMFS_LOAD_FALLBACK when it is not set), so a
 * raw image must be position-independent or linked to run at that address.
 */
int boot_from_mem(ulong data_load_addr)
{
	const char *loadaddr = env_get("loadaddr");
	const char *bootconf = env_get("bootconf");
	ulong load_addr;
	char cmd[96];
	int ret;

	if (loadaddr && loadaddr[0])
		load_addr = simple_strtoul(loadaddr, NULL, 0);
	else
		load_addr = FAILSAFE_INITRAMFS_LOAD_FALLBACK;

	if (load_addr != data_load_addr)
		memcpy((void *)load_addr, (const void *)data_load_addr,
		       upload_size);

	switch (genimg_get_format((const void *)load_addr)) {
	case IMAGE_FORMAT_FIT:
	case IMAGE_FORMAT_LEGACY:
		/* Parseable boot image: use bootm (the initramfs path). */
		if (bootconf && bootconf[0])
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx#%s",
				 load_addr, bootconf);
		else
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx", load_addr);

		return run_command(cmd, 0);

	default:
		/* Raw binary: jump straight to it with "go".  The image was
		 * copied with memcpy() while caches were active, so make sure
		 * it actually reached RAM and is not served from a stale
		 * i-cache line before executing.
		 */
		flush_cache(load_addr, upload_size);
		invalidate_icache_all();
		printf("\n*** Failsafe: raw image - 'go 0x%lx' ***\n\n",
		       load_addr);
		snprintf(cmd, sizeof(cmd), "go 0x%lx", load_addr);
		ret = run_command(cmd, 0);
		if (ret)
			printf("Failsafe: 'go 0x%lx' failed (ret=%d)\n",
			       load_addr, ret);
		return ret;
	}
}
