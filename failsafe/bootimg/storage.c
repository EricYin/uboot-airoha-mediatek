// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage helper: the generic MTD / UBI write path shared by
 * the Airoha and MediaTek board code.
 *
 * Target selection is automatic: if an MTD partition with the requested
 * name exists it is erased and written; otherwise the target is treated
 * as a UBI volume ("rootfs_data" is removed first to free space, the old
 * volume is removed, a new volume is created and the image is written
 * with "ubi write").
 */

#include <command.h>
#include <errno.h>
#include <linux/string.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <vsprintf.h>

#include <failsafe/storage.h>

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
 * Generic MTD partition capacity check: the uploaded image, including
 * the write offset (Airoha BL2 is written 0x800 bytes into the "bl2"
 * partition), must fit entirely inside the target MTD partition.
 *
 * This is the single size gate for the MTD-backed bootloader targets
 * (bl2 / u-boot / chainloader): the image size must not exceed the MTD
 * partition size.  There are no fixed image size limits.
 */
static int check_mtd_capacity(const char *target, u64 off, size_t size)
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

int failsafe_check_capacity(const char *target, u64 mtd_off, size_t size)
{
	if (is_mtd_partition(target))
		return check_mtd_capacity(target, mtd_off, size);

	if (!strcmp(target, FAILSAFE_STORAGE_STATIC_TARGET) &&
	    size > FAILSAFE_STORAGE_STATIC_SIZE) {
		/* The "fip" static volume is created at the fixed
		 * FAILSAFE_STORAGE_STATIC_SIZE (see failsafe_storage_write),
		 * so reject images that would not fit - otherwise
		 * "ubi write" fails after the volume has already been
		 * recreated empty.
		 */
		printf("Failsafe: '%s' image too big (%zu > 0x%zx)\n",
		       target, size, (size_t)FAILSAFE_STORAGE_STATIC_SIZE);
		return -EFBIG;
	}

	return 0;
}

int failsafe_storage_write(const char *target, u64 mtd_off,
			   const void *data, size_t size)
{
	char cmd[256];
	int ret;

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

		snprintf(cmd, sizeof(cmd), "mtd write %s 0x%lx 0x%llx 0x%lx",
			 target, (unsigned long)(uintptr_t)data,
			 (unsigned long long)mtd_off, (unsigned long)size);
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
		 * at the fixed FAILSAFE_STORAGE_STATIC_SIZE (0x100000),
		 * exactly like "ubi create fip 0x100000 static" from the
		 * console "ubi_write_fip"; "ubi write" later records the
		 * actual image size as used_bytes.  All other UBI targets
		 * are dynamic volumes sized to the uploaded image
		 * ("ubi create fit $filesize dynamic").
		 */
		if (!strcmp(target, FAILSAFE_STORAGE_STATIC_TARGET))
			snprintf(cmd, sizeof(cmd),
				 "ubi create %s 0x%zx static", target,
				 (size_t)FAILSAFE_STORAGE_STATIC_SIZE);
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
		if (!strcmp(target, FAILSAFE_STORAGE_FIT_TARGET)) {
			ret = failsafe_recreate_rootfs_data();
			if (ret)
				return ret;
		}
	}

	printf("\n*** Failsafe upgrade completed ('%s') ***\n\n", target);
	return 0;
}
