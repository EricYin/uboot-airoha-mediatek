/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe storage helpers: the generic MTD /
 * UBI write path and the matching capacity checks used by the Airoha
 * and MediaTek board code.
 */

#ifndef _FAILSAFE_STORAGE_H_
#define _FAILSAFE_STORAGE_H_

#include <linux/types.h>

/* UBI static volume holding a FIP, always created at a fixed size. */
#define FAILSAFE_STORAGE_STATIC_TARGET	"fip"
#define FAILSAFE_STORAGE_STATIC_SIZE	0x100000

/* UBI volume whose OpenWrt overlay ("rootfs_data") is rebuilt after a write. */
#define FAILSAFE_STORAGE_FIT_TARGET	"fit"

/* Bootloader targets, which always live in a raw MTD partition and can
 * never be a UBI volume: the preloader (BL2), the second stage U-Boot
 * (chainloader) and the self-contained legacy 512 KiB U-Boot image.
 */
#define FAILSAFE_STORAGE_BL2_TARGET		"bl2"
#define FAILSAFE_STORAGE_CHAINLOADER_TARGET	"chainloader"
#define FAILSAFE_STORAGE_UBOOT_TARGET		"u-boot"

/**
 * failsafe_check_capacity() - generic storage capacity check
 * @target: MTD partition or UBI volume name
 * @mtd_off: write offset inside an MTD partition (0 for UBI volumes)
 * @size: image size in bytes
 *
 * For MTD partitions the image, including @mtd_off, must fit inside the
 * partition.  The "fip" static UBI volume is created at a fixed size, so
 * images larger than it are rejected before the volume is recreated.
 *
 * Returns 0 when the image fits, -ENODEV / -EFBIG otherwise.
 */
int failsafe_check_capacity(const char *target, u64 mtd_off, size_t size);

/**
 * failsafe_storage_write() - write an image to its storage target
 * @target: MTD partition or UBI volume name
 * @mtd_off: write offset inside an MTD partition (0 for UBI volumes)
 * @data: image contents
 * @size: image size in bytes
 *
 * If an MTD partition named @target exists it is erased and written.  It
 * is otherwise treated as a UBI volume: "rootfs_data" is removed first to
 * free space, the old volume is removed, a new volume is created ("fip"
 * as a static volume of FAILSAFE_STORAGE_STATIC_SIZE, everything else
 * dynamic with the image size) and the image is written with
 * "ubi write".  After a FAILSAFE_STORAGE_FIT_TARGET write the OpenWrt
 * "rootfs_data" overlay volume is recreated.
 *
 * The bootloader targets (FAILSAFE_STORAGE_{BL2,CHAINLOADER,UBOOT}_TARGET)
 * are MTD partitions only: when the partition is missing the write fails
 * with -ENODEV rather than being rerouted to the UBI path.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int failsafe_storage_write(const char *target, u64 mtd_off,
			   const void *data, size_t size);

/**
 * failsafe_storage_read() - read raw bytes from a storage target
 * @target: MTD partition or UBI volume name
 * @offset: byte offset inside the target
 * @buf: destination buffer
 * @max_len: maximum number of bytes to read
 * @read_len: receives the number of bytes actually read (may be NULL)
 *
 * If an MTD partition named @target exists it is read through the MTD
 * API; otherwise @target is treated as a UBI volume ("ubi part ubi" is
 * attached first).  At most @max_len bytes are read, clamped to the
 * partition / volume size.
 *
 * Returns 0 on success, -ENODEV when the target does not exist, -EINVAL
 * when @offset is past the end, -EIO on a read error.
 */
int failsafe_storage_read(const char *target, u64 offset, void *buf,
			  size_t max_len, size_t *read_len);

#endif /* _FAILSAFE_STORAGE_H_ */
