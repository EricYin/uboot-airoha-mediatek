// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage backend: raw MTD partitions.
 *
 * One of the three backends behind failsafe_storage_write() /
 * failsafe_storage_read() (see failsafe/bootimg/storage.c); the other two
 * are the MMC partitions (failsafe/bootimg/mmc.c) and the UBI volumes
 * (failsafe/bootimg/ubi.c).
 *
 * This is the backend the boards supported today use for the bootloader
 * stages ("bl2", "chainloader", "u-boot"): those live in a raw MTD
 * partition and nowhere else.
 *
 * Without CONFIG_MTD every entry point reports "not found" / -ENODEV, so
 * the dispatcher moves on to the next backend (an eMMC-only board does not
 * need MTD support at all).
 */

#include <command.h>
#include <errno.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <vsprintf.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>

#include <failsafe/storage.h>
#include <failsafe/cprint.h>

#if IS_ENABLED(CONFIG_MTD)

/*
 * Probe whether 'name' refers to a known MTD partition.
 *
 * This is also what decides between the MTD and the other write paths: if
 * an MTD partition with that name exists it is erased and written through
 * the MTD API, otherwise the target is treated as an MMC partition or a
 * UBI volume.
 */
bool failsafe_mtd_exists(const char *name)
{
	struct mtd_info *mtd;

	if (!name || !name[0])
		return false;

	mtd_probe_devices();

	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return false;

	put_mtd_device(mtd);
	return true;
}

/*
 * Capacity check: the image, including the write offset (the Airoha BL2 is
 * written 0x800 bytes into the "bl2" partition), must fit entirely inside
 * the target MTD partition.
 *
 * This is the single size gate for the MTD-backed bootloader targets
 * (bl2 / u-boot / chainloader): the image size must not exceed the MTD
 * partition size.  There are no fixed image size limits.
 */
int failsafe_mtd_capacity(const char *name, u64 off, size_t size)
{
	struct mtd_info *mtd;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd)) {
		cprintln(ERROR, "Failsafe: MTD partition '%s' not found",
			 name);
		return -ENODEV;
	}

	if (off + (u64)size > mtd->size) {
		cprintln(ERROR, "Failsafe: image (%zu) exceeds partition "
			 "'%s' (%llu), write offset 0x%llx", size, name,
			 (unsigned long long)mtd->size,
			 (unsigned long long)off);
		put_mtd_device(mtd);
		return -EFBIG;
	}

	put_mtd_device(mtd);
	return 0;
}

int failsafe_mtd_write(const char *name, u64 off, const void *data,
		       size_t size)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "mtd erase %s", name);
	ret = run_command(cmd, 0);
	if (ret) {
		cprintln(ERROR, "Failsafe: erase '%s' failed (ret=%d)",
			 name, ret);
		return -EIO;
	}

	snprintf(cmd, sizeof(cmd), "mtd write %s 0x%lx 0x%llx 0x%lx",
		 name, (unsigned long)(uintptr_t)data,
		 (unsigned long long)off, (unsigned long)size);
	ret = run_command(cmd, 0);
	if (ret) {
		cprintln(ERROR, "Failsafe: write '%s' failed (ret=%d)", name,
			 ret);
		return -EIO;
	}

	return 0;
}

int failsafe_mtd_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len)
{
	struct mtd_info *mtd;
	size_t len, retlen = 0;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	if (off >= mtd->size) {
		put_mtd_device(mtd);
		return -EINVAL;
	}

	len = mtd->size - off;
	if (len > max_len)
		len = max_len;

	ret = mtd_read(mtd, off, len, &retlen, buf);
	put_mtd_device(mtd);

	/* -EUCLEAN only reports corrected bit flips: the data is good. */
	if (ret && ret != -EUCLEAN)
		return -EIO;

	if (read_len)
		*read_len = retlen;

	return 0;
}

#else /* !CONFIG_MTD */

bool failsafe_mtd_exists(const char *name)
{
	(void)name;
	return false;
}

int failsafe_mtd_capacity(const char *name, u64 off, size_t size)
{
	(void)name;
	(void)off;
	(void)size;
	return -ENODEV;
}

int failsafe_mtd_write(const char *name, u64 off, const void *data,
		       size_t size)
{
	(void)name;
	(void)off;
	(void)data;
	(void)size;
	return -ENODEV;
}

int failsafe_mtd_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len)
{
	(void)name;
	(void)off;
	(void)buf;
	(void)max_len;
	(void)read_len;
	return -ENODEV;
}

#endif /* CONFIG_MTD */
