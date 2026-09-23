/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe storage helpers used by the Airoha and
 * MediaTek board code: the target -> backend resolution and the matching
 * capacity checks.
 *
 * The actual work is done by one of three backends, each in its own file
 * (the same split the MediaTek tree uses between mmc_helper.c and
 * mtd_helper.c):
 *
 *   failsafe/bootimg/mtd.c   raw MTD partitions
 *   failsafe/bootimg/mmc.c   MMC (GPT / MBR) partitions, the GPT itself,
 *			      the eMMC boot0/boot1 hardware partitions and
 *			      the split / single firmware upgrade
 *   failsafe/bootimg/ubi.c   UBI volumes
 */

#ifndef _FAILSAFE_STORAGE_H_
#define _FAILSAFE_STORAGE_H_

#include <linux/types.h>

/* UBI static volume holding a FIP, always created at a fixed size. */
#define FAILSAFE_STORAGE_STATIC_TARGET	"fip"
#define FAILSAFE_STORAGE_STATIC_SIZE	0x100000

/* UBI volume whose OpenWrt overlay ("rootfs_data") is rebuilt after a write.
 *
 * The same name is also the first partition tried for a single (ITB / FIT)
 * firmware image on an MMC device; see failsafe_storage_write_firmware().
 */
#define FAILSAFE_STORAGE_FIT_TARGET	"fit"

/* The partition names of the standard single-image (ITB / FIT) layout of an
 * MMC system image, tried in this order when the board's own target is not
 * a partition of the device (see failsafe_storage_write_firmware()). */
#define FAILSAFE_STORAGE_FIRMWARE_TARGET	"firmware"
#define FAILSAFE_STORAGE_PRODUCTION_TARGET	"production"

/* Bootloader targets.  Where each one lives is platform policy (an MTD
 * partition, a fixed offset in the user area of an eMMC, the boot0
 * hardware partition, a GPT partition, ...), so the board's failsafe code
 * writes them with the backend primitives directly and the generic path
 * below only knows the names.
 */
#define FAILSAFE_STORAGE_BL2_TARGET		"bl2"
#define FAILSAFE_STORAGE_CHAINLOADER_TARGET	"chainloader"
#define FAILSAFE_STORAGE_UBOOT_TARGET		"u-boot"

/* MMC partition table (GPT) target.
 *
 * The uploaded image is the primary table area of a GPT disk: the
 * protective MBR (LBA 0), the GPT header (LBA 1) and the partition entry
 * array (LBA 2 and following), i.e. GPT_MAX_SIZE sectors.  It is written
 * to the start of the MMC device; the secondary (backup) table is
 * generated from it at the end of the device, and the GPT header is
 * adjusted to the real device size (see failsafe_mmc_write_gpt()).
 *
 * Like the bootloader targets this only exists on one kind of storage
 * (MMC), so a missing MMC device is reported as an error instead of
 * being routed to the MTD / UBI path.
 */
#define FAILSAFE_STORAGE_GPT_TARGET	"gpt"
#define FAILSAFE_STORAGE_GPT_MAX_SIZE	(34 * 512)

/* ------------------------------------------------------------------ */
/*  Public entry points                                                */
/* ------------------------------------------------------------------ */

/**
 * failsafe_check_capacity() - storage capacity check of a target
 * @target: MTD partition, MMC partition or UBI volume name
 * @mtd_off: write offset inside an MTD partition (0 for the other backends)
 * @size: image size in bytes
 *
 * Resolved like failsafe_storage_write() (first match wins): for MTD
 * partitions the image, including @mtd_off, must fit inside the partition;
 * for MMC partitions it must fit inside the GPT / MBR partition; the "gpt"
 * target is limited to the fixed table area
 * (FAILSAFE_STORAGE_GPT_MAX_SIZE) and the "fip" static UBI volume to its
 * fixed creation size.
 *
 * The bootloader stages are checked by the platform code that writes them
 * (it is the only place that knows where they live).
 *
 * Returns 0 when the image fits, -ENODEV when the target does not exist,
 * -EFBIG when it is too small.
 */
int failsafe_check_capacity(const char *target, u64 mtd_off, size_t size);

/**
 * failsafe_storage_write() - write an image to its storage target
 * @target: MTD partition, MMC partition / hardware partition or UBI
 *	    volume name
 * @mtd_off: write offset inside an MTD partition (0 for the other backends)
 * @data: image contents
 * @size: image size in bytes
 *
 * The target is resolved in this order (the first match wins):
 *
 *   1. an MTD partition of that name - erased and written through the
 *      MTD API;
 *   2. on MMC devices, FAILSAFE_STORAGE_GPT_TARGET - the uploaded primary
 *      partition table is written to LBA 0 after it has been adjusted to
 *      the real device size, and the secondary table is generated at the
 *      end of the device;
 *   3. on MMC devices, an MMC (GPT / MBR) partition of that name - the
 *      image is written at the partition start, the rest of the
 *      partition keeps its content;
 *   4. otherwise a UBI volume: for FAILSAFE_STORAGE_FIT_TARGET the OpenWrt
 *      "rootfs_data" overlay volume is removed *first* to free the PEBs it
 *      holds (it is created with the maximum available size), the old
 *      volume is removed, a new volume is created ("fip" as a static volume
 *      of FAILSAFE_STORAGE_STATIC_SIZE, everything else dynamic with the
 *      image size) and the image is written with "ubi write".  After a
 *      FAILSAFE_STORAGE_FIT_TARGET write the OpenWrt overlay volume is
 *      recreated, so the device boots with a fresh "rootfs_data".
 *
 * The bootloader targets (FAILSAFE_STORAGE_{BL2,CHAINLOADER,UBOOT}_TARGET)
 * and FAILSAFE_STORAGE_GPT_TARGET only exist on one kind of storage: when
 * the partition / device is missing the write fails with -ENODEV rather
 * than being rerouted to another backend.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int failsafe_storage_write(const char *target, u64 mtd_off,
			   const void *data, size_t size);

/**
 * failsafe_storage_write_firmware() - write an uploaded firmware (system)
 *				       image
 * @target: the board's firmware target (FAILSAFE_STORAGE_FIT_TARGET, i.e.
 *	    "fit", for both boards)
 * @data: image contents (one ITB / FIT image)
 * @size: image size in bytes
 *
 * A system image is one single boot image holding everything; the boards
 * supported today do not use a split layout with separate kernel / rootfs
 * partitions.
 *
 * On an MMC device it goes into the first partition of the standard
 * single-image layout that exists: @target ("fit"), then "firmware" and
 * "production" (see FAILSAFE_STORAGE_{FIRMWARE,PRODUCTION}_TARGET).
 * Everywhere else the image keeps going to @target through the generic
 * path - the "fit" UBI volume on NAND / NOR devices.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int failsafe_storage_write_firmware(const char *target, const void *data,
				    size_t size);


/**
 * failsafe_storage_read() - read raw bytes from a storage target
 * @target: MTD partition, MMC partition / hardware partition or UBI
 *	    volume name
 * @offset: byte offset inside the target
 * @buf: destination buffer
 * @max_len: maximum number of bytes to read
 * @read_len: receives the number of bytes actually read (may be NULL)
 *
 * The target is resolved like failsafe_storage_write() (MTD partition,
 * then MMC partition, then UBI volume); "ubi part ubi" is attached first
 * when the target turns out to be a UBI volume.  At most @max_len bytes
 * are read, clamped to the partition / volume size.
 *
 * Returns 0 on success, -ENODEV when the target does not exist, -EINVAL
 * when @offset is past the end, -EIO on a read error.
 */
int failsafe_storage_read(const char *target, u64 offset, void *buf,
			  size_t max_len, size_t *read_len);

/* ------------------------------------------------------------------ */
/*  Backend: raw MTD partitions (failsafe/bootimg/mtd.c)               */
/* ------------------------------------------------------------------ */

/** failsafe_mtd_exists() - is @name an MTD partition? */
bool failsafe_mtd_exists(const char *name);

/** failsafe_mtd_capacity() - capacity check of an MTD partition */
int failsafe_mtd_capacity(const char *name, u64 off, size_t size);

/** failsafe_mtd_write() - erase and write an MTD partition */
int failsafe_mtd_write(const char *name, u64 off, const void *data,
		       size_t size);

/** failsafe_mtd_read() - read inside an MTD partition */
int failsafe_mtd_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len);

/* ------------------------------------------------------------------ */
/*  Backend: UBI volumes (failsafe/bootimg/ubi.c)                      */
/* ------------------------------------------------------------------ */

/** failsafe_ubi_attach() - "ubi part ubi" (idempotent) */
int failsafe_ubi_attach(void);

/** failsafe_ubi_capacity() - capacity check of a UBI volume */
int failsafe_ubi_capacity(const char *name, size_t size);

/** failsafe_ubi_write() - (re)create and write a UBI volume */
int failsafe_ubi_write(const char *name, const void *data, size_t size);

/** failsafe_ubi_read() - read inside a UBI volume */
int failsafe_ubi_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len);

#endif /* _FAILSAFE_STORAGE_H_ */
