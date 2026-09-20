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
 * with "ubi write").  Bootloader targets (bl2 / chainloader / u-boot)
 * only ever exist as MTD partitions, so a missing one is reported as an
 * error instead of being rerouted to the UBI path.
 */

#include <command.h>
#include <errno.h>
#include <linux/string.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <vsprintf.h>

#include <failsafe/storage.h>
#include <failsafe/cprint.h>

#if IS_ENABLED(CONFIG_CMD_UBI)
#include <ubi_uboot.h>
#endif

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
 * Whether 'name' is a bootloader target, i.e. one that only ever exists
 * as a raw MTD partition (preloader / chainloader / the legacy U-Boot
 * image) and can never be a UBI volume.
 *
 * Without this check a missing partition - a typo, or a board built with
 * the wrong device tree - made failsafe_storage_write() silently take the
 * UBI branch and run "ubi create bl2", which fails with a misleading
 * "not enough PEBs, only 0 available" (or, worse, succeeds and creates a
 * bogus volume next to the real one).
 */
static bool is_mtd_only_target(const char *name)
{
	return !strcmp(name, FAILSAFE_STORAGE_BL2_TARGET) ||
	       !strcmp(name, FAILSAFE_STORAGE_CHAINLOADER_TARGET) ||
	       !strcmp(name, FAILSAFE_STORAGE_UBOOT_TARGET);
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
		cprintln(ERROR, "Failsafe: MTD partition '%s' not found",
			 target);
		return -ENODEV;
	}

	if (off + (u64)size > mtd->size) {
		cprintln(ERROR, "Failsafe: image (%zu) exceeds partition "
			 "'%s' (%llu), write offset 0x%llx", size, target,
			 (unsigned long long)mtd->size,
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
 * Remove the OpenWrt "rootfs_data" overlay volume, if present.
 *
 * "rootfs_data" is created as a dynamic volume spanning the maximum
 * available size ("ubi create rootfs_data - dynamic"), so it owns every
 * free PEB of the UBI device.  The "fit" volume is created from that very
 * same pool, so the overlay has to be dropped *before* the new "fit"
 * volume is created - otherwise "ubi create fit <size> dynamic" fails
 * with "not enough PEBs, only N available" as soon as the device already
 * carries an overlay, which is the normal case after the first boot.
 *
 * A missing volume is not an error: "ubi check" then returns non-zero and
 * there is nothing to free.
 */
static int failsafe_remove_rootfs_data(void)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "ubi check rootfs_data");
	if (run_command(cmd, 0))
		return 0;

	snprintf(cmd, sizeof(cmd), "ubi remove rootfs_data");
	ret = run_command(cmd, 0);
	if (ret) {
		cprintln(ERROR, "Failsafe: remove 'rootfs_data' failed "
			 "(ret=%d)", ret);
		return -EIO;
	}

	return 0;
}

/*
 * Rebuild the OpenWrt "rootfs_data" overlay volume after a FIT upgrade.
 *
 * A FIT upgrade deliberately starts from a fresh overlay: the volume is
 * recreated as a dynamic volume spanning all space left over by the new
 * "fit" volume ("-" = maximum available size).
 */
static int failsafe_recreate_rootfs_data(void)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "ubi create rootfs_data - dynamic");
	ret = run_command(cmd, 0);
	if (ret) {
		cprintln(ERROR, "Failsafe: 'ubi create rootfs_data' failed "
			 "(ret=%d)", ret);
		return -EIO;
	}

	cprintln(SUCCESS, "Failsafe: 'rootfs_data' recreated");
	return 0;
}

int failsafe_check_capacity(const char *target, u64 mtd_off, size_t size)
{
	if (is_mtd_partition(target))
		return check_mtd_capacity(target, mtd_off, size);

	/* A bootloader target without its MTD partition is a hard error:
	 * never report "fits" for something that cannot be written.
	 */
	if (is_mtd_only_target(target)) {
		cprintln(ERROR, "Failsafe: MTD partition '%s' not found",
			 target);
		return -ENODEV;
	}

	if (!strcmp(target, FAILSAFE_STORAGE_STATIC_TARGET) &&
	    size > FAILSAFE_STORAGE_STATIC_SIZE) {
		/* The "fip" static volume is created at the fixed
		 * FAILSAFE_STORAGE_STATIC_SIZE (see failsafe_storage_write),
		 * so reject images that would not fit - otherwise
		 * "ubi write" fails after the volume has already been
		 * recreated empty.
		 */
		cprintln(ERROR, "Failsafe: '%s' image too big (%zu > 0x%zx)",
			 target, size,
			 (size_t)FAILSAFE_STORAGE_STATIC_SIZE);
		return -EFBIG;
	}

	return 0;
}

int failsafe_storage_write(const char *target, u64 mtd_off,
			   const void *data, size_t size)
{
	char cmd[256];
	int ret;

	cprintln(NORMAL, "\n*** Failsafe upgrade: %zu (0x%zx) bytes -> "
		 "'%s' ***\n", size, size, target);

	if (is_mtd_partition(target)) {
		/* ----- MTD partition ----- */
		snprintf(cmd, sizeof(cmd), "mtd erase %s", target);
		ret = run_command(cmd, 0);
		if (ret) {
			cprintln(ERROR, "Failsafe: erase '%s' failed (ret=%d)",
				 target, ret);
			return -EIO;
		}

		snprintf(cmd, sizeof(cmd), "mtd write %s 0x%lx 0x%llx 0x%lx",
			 target, (unsigned long)(uintptr_t)data,
			 (unsigned long long)mtd_off, (unsigned long)size);
		ret = run_command(cmd, 0);
		if (ret) {
			cprintln(ERROR, "Failsafe: write '%s' failed (ret=%d)",
				 target, ret);
			return -EIO;
		}
	} else if (is_mtd_only_target(target)) {
		/* A bootloader stage is always an MTD partition; falling
		 * back to UBI here would create a bogus volume and hide the
		 * real problem (missing partition / wrong device tree).
		 */
		cprintln(ERROR, "Failsafe: MTD partition '%s' not found, "
			 "refusing to fall back to a UBI volume", target);
		return -ENODEV;
	} else {
		/* ----- UBI volume ----- */
		ret = ubi_ensure_attached();
		if (ret) {
			cprintln(ERROR, "Failsafe: cannot attach UBI (ret=%d)",
				 ret);
			return -EIO;
		}

		/*
		 * "fit" shares the UBI device with the OpenWrt overlay
		 * volume, which is created with the maximum available
		 * size: the overlay has to be dropped first, otherwise no
		 * PEBs are left for the new "fit" volume.  It is rebuilt
		 * once the FIT image has been written (see below).
		 */
		if (!strcmp(target, FAILSAFE_STORAGE_FIT_TARGET)) {
			ret = failsafe_remove_rootfs_data();
			if (ret)
				return ret;
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
			cprintln(ERROR, "Failsafe: 'ubi create %s' failed "
				 "(ret=%d)", target, ret);
			return -EIO;
		}

		snprintf(cmd, sizeof(cmd),
			 "ubi write 0x%lx %s 0x%lx",
			 (unsigned long)(uintptr_t)data,
			 target, (unsigned long)size);
		ret = run_command(cmd, 0);
		if (ret) {
			cprintln(ERROR, "Failsafe: 'ubi write %s' failed "
				 "(ret=%d)", target, ret);
			return -EIO;
		}

		/*
		 * FIT upgrade: rebuild the OpenWrt "rootfs_data" volume
		 * that was removed above, so the device boots with a fresh
		 * overlay spanning the space left by the new "fit" volume.
		 */
		if (!strcmp(target, FAILSAFE_STORAGE_FIT_TARGET)) {
			ret = failsafe_recreate_rootfs_data();
			if (ret)
				return ret;
		}
	}

	cprintln(SUCCESS, "\n*** Failsafe upgrade completed ('%s') ***\n",
		 target);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Read path (image inspection / debugging)                           */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MTD
static int read_mtd(const char *target, u64 offset, void *buf, size_t max_len,
		    size_t *read_len)
{
	struct mtd_info *mtd;
	size_t len, retlen = 0;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(target);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	if (offset >= mtd->size) {
		put_mtd_device(mtd);
		return -EINVAL;
	}

	len = mtd->size - offset;
	if (len > max_len)
		len = max_len;

	ret = mtd_read(mtd, offset, len, &retlen, buf);
	put_mtd_device(mtd);

	/* -EUCLEAN only reports corrected bit flips: the data is good. */
	if (ret && ret != -EUCLEAN)
		return -EIO;

	if (read_len)
		*read_len = retlen;

	return 0;
}
#endif /* CONFIG_MTD */

#if IS_ENABLED(CONFIG_CMD_UBI)
static int read_ubi(const char *target, u64 offset, void *buf, size_t max_len,
		    size_t *read_len)
{
	struct ubi_volume *vol;
	size_t len;
	int ret;

	/*
	 * Attach only when UBI is not up yet: re-attaching would tear down
	 * and rebuild the volume structures, invalidating an in-flight
	 * session (e.g. a streamed volume backup).
	 */
	if (!ubi_devices[0]) {
		ret = ubi_ensure_attached();
		if (ret)
			return -EIO;
	}

	vol = ubi_find_volume(target);
	if (!vol)
		return -ENODEV;

	if (offset >= (u64)vol->used_bytes)
		return -EINVAL;

	len = (u64)vol->used_bytes - offset;
	if (len > max_len)
		len = max_len;

	ret = ubi_volume_read(target, buf, offset, len);
	if (ret)
		return -EIO;

	if (read_len)
		*read_len = len;

	return 0;
}
#endif /* CONFIG_CMD_UBI */

int failsafe_storage_read(const char *target, u64 offset, void *buf,
			  size_t max_len, size_t *read_len)
{
	if (!target || !buf || !max_len)
		return -EINVAL;

	if (is_mtd_partition(target)) {
#ifdef CONFIG_MTD
		return read_mtd(target, offset, buf, max_len, read_len);
#else
		return -ENODEV;
#endif
	}

#if IS_ENABLED(CONFIG_CMD_UBI)
	return read_ubi(target, offset, buf, max_len, read_len);
#else
	return -ENODEV;
#endif
}
