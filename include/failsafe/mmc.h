/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * MMC storage backend and its inline helpers.
 *
 * Two layers live behind this header, both built on the standard U-Boot
 * MMC / block / partition API only, so they work on any board that enables
 * CONFIG_MMC:
 *
 *   - the inline helpers (device lookup, partition lookup, block aligned
 *     read / write / erase) used by the backends and by the flash page;
 *   - the interface of the MMC backend, failsafe/bootimg/mmc.c:
 *     partitions by GPT / MBR name, raw regions of a hardware partition
 *     (user area / boot0 / boot1) and the partition table itself.
 *
 * Whether a partition or region belongs to the preloader, the FIP or the
 * system image is decided by the board code
 * (board/{airoha,mediatek}/common/failsafe.c).
 *
 * The vendor string beautifier (failsafe_mmc_vendor_pretty) is declared
 * in <failsafe/helpers.h> and implemented in failsafe/modules/helpers.c.
 */

#ifndef _FAILSAFE_MMC_H_
#define _FAILSAFE_MMC_H_

#include <errno.h>
#include <malloc.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>

/*
 * MMC device used for the failsafe storage targets.  Boards may override
 * it at build time (mmc 0 by default).
 */
#ifndef FAILSAFE_MMC_DEV_NUM
#define FAILSAFE_MMC_DEV_NUM	0
#endif

#if IS_ENABLED(CONFIG_MMC)

#include <mmc.h>
#include <part.h>

/**
 * failsafe_mmc_get_dev() - get and initialize the failsafe MMC device
 *
 * Returns the initialized device, or NULL when it is not available.
 */
static inline struct mmc *failsafe_mmc_get_dev(void)
{
	struct mmc *mmc = find_mmc_device(FAILSAFE_MMC_DEV_NUM);

	if (!mmc)
		return NULL;
	if (!mmc->has_init && mmc_init(mmc))
		return NULL;

	return mmc;
}

/**
 * failsafe_mmc_blk_desc() - block descriptor of an MMC device
 * @mmc: MMC device (may be NULL)
 */
static inline struct blk_desc *failsafe_mmc_blk_desc(struct mmc *mmc)
{
	return mmc ? mmc_get_blk_desc(mmc) : NULL;
}

/**
 * failsafe_mmc_present() - whether the failsafe MMC device is usable
 */
static inline bool failsafe_mmc_present(void)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(failsafe_mmc_get_dev());

	return bd && bd->type != DEV_TYPE_UNKNOWN;
}

/**
 * failsafe_mmc_find_part() - look up an MMC partition by name
 * @mmc:   MMC device
 * @name:  partition name (GPT / MBR label)
 * @dpart: receives the partition description
 *
 * Returns 0 on success, -ENODEV when the partition does not exist.
 */
static inline int failsafe_mmc_find_part(struct mmc *mmc, const char *name,
					 struct disk_partition *dpart)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);

	if (!bd || !name || !name[0] || !dpart)
		return -EINVAL;

	part_init(bd);

	return part_get_info_by_name(bd, name, dpart) >= 0 ? 0 : -ENODEV;
}

/**
 * failsafe_mmc_read() - read a byte range from an MMC device
 * @mmc:    MMC device
 * @offset: byte offset (need not be block aligned)
 * @buf:    destination buffer
 * @len:    number of bytes to read
 *
 * The block device is addressed in whole blocks, so the request is
 * rounded outwards to block boundaries and the surplus bytes are dropped
 * afterwards.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
static inline int failsafe_mmc_read(struct mmc *mmc, u64 offset, void *buf,
				    size_t len)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	lbaint_t blocks;
	u64 blksz, start, end;
	u8 *blkbuf;

	if (!bd || !bd->blksz || !buf || !len)
		return -EINVAL;

	blksz = bd->blksz;
	start = offset & ~(blksz - 1);
	end = (offset + len + blksz - 1) & ~(blksz - 1);
	blocks = (end - start) / blksz;

	blkbuf = malloc(end - start);
	if (!blkbuf)
		return -ENOMEM;

	if (blk_dread(bd, start / blksz, blocks, blkbuf) != blocks) {
		free(blkbuf);
		return -EIO;
	}

	memcpy(buf, blkbuf + (offset - start), len);
	free(blkbuf);

	return 0;
}

/**
 * failsafe_mmc_write() - write a byte range to an MMC device
 * @mmc:    MMC device
 * @offset: byte offset (need not be block aligned)
 * @buf:    source buffer
 * @len:    number of bytes to write
 *
 * The request is written in up to three parts: the unaligned head and
 * tail are merged into their block with a read-modify-write, so the bytes
 * outside the requested range are preserved (the same read-modify-write
 * behaviour as the MTD path), while the whole blocks in between go to the
 * block layer in one call.  A multi-MiB firmware image would otherwise
 * need one blk_dwrite() per block.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
static inline int failsafe_mmc_write(struct mmc *mmc, u64 offset,
				     const void *buf, size_t len)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	size_t head_len, bulk_len, tail_len;
	u64 blksz, pos;
	lbaint_t blk;
	u8 *blkbuf;
	int ret = 0;

	if (!bd || !bd->blksz || !buf || !len)
		return -EINVAL;

	blksz = bd->blksz;

	head_len = offset & (blksz - 1) ?
		   (size_t)(blksz - (offset & (blksz - 1))) : 0;
	if (head_len > len)
		head_len = len;

	bulk_len = (len - head_len) / blksz * blksz;
	tail_len = len - head_len - bulk_len;

	blkbuf = malloc(blksz);
	if (!blkbuf)
		return -ENOMEM;

	/* Unaligned head: preserve the bytes in front of the request. */
	if (head_len) {
		blk = offset / blksz;
		if (blk_dread(bd, blk, 1, blkbuf) != 1) {
			ret = -EIO;
			goto out;
		}

		memcpy(blkbuf + (offset - (u64)blk * blksz), buf, head_len);

		if (blk_dwrite(bd, blk, 1, blkbuf) != 1) {
			ret = -EIO;
			goto out;
		}
	}

	/* Whole blocks: a single call for the bulk of the image. */
	if (bulk_len) {
		blk = (offset + head_len) / blksz;
		if (blk_dwrite(bd, blk, bulk_len / blksz,
			       (const u8 *)buf + head_len) != bulk_len / blksz) {
			ret = -EIO;
			goto out;
		}
	}

	/* Unaligned tail: preserve the bytes behind the request.  The
	 * offset is block aligned here (head_len rounds it up, bulk_len is
	 * a multiple of the block size), so the tail sits at the start of
	 * its block.
	 */
	if (tail_len) {
		pos = offset + head_len + bulk_len;
		blk = pos / blksz;

		if (blk_dread(bd, blk, 1, blkbuf) != 1) {
			ret = -EIO;
			goto out;
		}

		memcpy(blkbuf, (const u8 *)buf + head_len + bulk_len,
		       tail_len);

		if (blk_dwrite(bd, blk, 1, blkbuf) != 1) {
			ret = -EIO;
			goto out;
		}
	}

out:
	free(blkbuf);
	return ret;
}

/**
 * failsafe_mmc_erase() - erase a byte range of an MMC device
 * @mmc:    MMC device
 * @offset: byte offset
 * @len:    number of bytes to erase
 *
 * The range is rounded outwards to whole blocks (erase is block based).
 *
 * Returns 0 on success, a negative errno otherwise.
 */
static inline int failsafe_mmc_erase(struct mmc *mmc, u64 offset, size_t len)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	u64 blksz, start, end;
	lbaint_t blocks;

	if (!bd || !bd->blksz || !len)
		return -EINVAL;

	blksz = bd->blksz;
	start = offset & ~(blksz - 1);
	end = (offset + len + blksz - 1) & ~(blksz - 1);

	if (end > (u64)bd->lba * blksz)
		return -EINVAL;

	blocks = (end - start) / blksz;

	return blk_derase(bd, start / blksz, blocks) == blocks ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/*  MMC partitions and the partition table (failsafe/bootimg/mmc.c)    */
/* ------------------------------------------------------------------ */

/**
 * failsafe_mmc_part_size() - size of an MMC partition
 * @name: partition name (GPT / MBR label)
 * @size: receives the partition size in bytes (may be NULL)
 *
 * Returns 0 on success, -ENODEV when the partition does not exist.
 */
int failsafe_mmc_part_size(const char *name, u64 *size);

/**
 * failsafe_mmc_write_part() - write an image to the start of a partition
 * @name: partition name
 * @data: image contents
 * @size: image size in bytes
 *
 * The image must fit inside the partition; the bytes behind it keep
 * their content.  Returns 0 on success, a negative errno otherwise.
 */
int failsafe_mmc_write_part(const char *name, const void *data, size_t size);

/**
 * failsafe_mmc_read_part() - read inside a partition
 * @name: partition name
 * @offset: byte offset inside the partition
 * @buf: destination buffer
 * @max_len: capacity of @buf
 * @read_len: receives the number of bytes read (may be NULL)
 *
 * At most @max_len bytes are read, clamped to the partition size.
 * Returns 0 on success, a negative errno otherwise.
 */
int failsafe_mmc_read_part(const char *name, u64 offset, void *buf,
			   size_t max_len, size_t *read_len);

/**
 * failsafe_mmc_erase_part() - erase a range inside a partition
 * @name: partition name
 * @offset: byte offset inside the partition
 * @size: number of bytes to erase (0 = to the end of the partition)
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int failsafe_mmc_erase_part(const char *name, u64 offset, u64 size);

/**
 * failsafe_mmc_write_gpt() - install a GPT (partition table) on the MMC
 * @data: primary partition table image (MBR + GPT header + entries)
 * @size: image size in bytes (at most FAILSAFE_STORAGE_GPT_MAX_SIZE)
 *
 * The GPT header is adjusted to the real device size (alternate_lba,
 * last_usable_lba and the header CRC are recomputed), the primary table
 * is written to LBA 0 and the secondary table is generated at the end of
 * the device.  Returns 0 on success, a negative errno otherwise.
 */
int failsafe_mmc_write_gpt(const void *data, size_t size);

#if IS_ENABLED(CONFIG_MMC)
/*
 * Hardware partitions of an MMC device: the user data area (SD cards only
 * have this one) and the two boot partitions of an eMMC.  Which one holds
 * what - e.g. the preloader, a FIP - is decided by the platform, not here:
 * the MediaTek tree puts the preloader into boot0, the Airoha one keeps it
 * at a fixed offset in the user area.
 */
#define FAILSAFE_MMC_HWPART_USER	0
#define FAILSAFE_MMC_HWPART_BOOT0	1
#define FAILSAFE_MMC_HWPART_BOOT1	2

/**
 * failsafe_mmc_is_sd() - SD card or eMMC?
 *
 * Returns 1 for an SD card (no hardware partitions, no boot options),
 * 0 for an eMMC and -ENODEV when there is no MMC device.
 */
int failsafe_mmc_is_sd(void);

/**
 * failsafe_mmc_region_capacity() - capacity check of a raw region
 * @hwpart: hardware partition (FAILSAFE_MMC_HWPART_*)
 * @off:    byte offset inside the hardware partition
 * @size:   number of bytes that have to fit
 *
 * The region must be inside the device; an area reserved for a specific
 * purpose (the 1 MiB preloader area of boot0, ...) is the platform's own
 * policy and is checked there.
 *
 * Returns 0 when it fits, -ENODEV / -ENOTSUPP / -EFBIG otherwise.
 */
int failsafe_mmc_region_capacity(int hwpart, u64 off, size_t size);

/**
 * failsafe_mmc_write_region() - write a raw region of a hardware partition
 * @hwpart: hardware partition (FAILSAFE_MMC_HWPART_*)
 * @off:    byte offset inside the hardware partition
 * @data:   image contents
 * @size:   image size in bytes
 *
 * Nothing is erased first (an eMMC overwrites in place) and no partition
 * table is needed, so this is what platform code uses for the preloader
 * area and for a FIP that lives outside a GPT partition.
 *
 * Returns 0 on success, -ENOTSUPP when the hardware partition does not
 * exist on this device (an SD card has none), a negative errno otherwise.
 */
int failsafe_mmc_write_region(int hwpart, u64 off, const void *data,
			      size_t size);

/**
 * failsafe_mmc_read_region() - read a raw region of a hardware partition
 * @hwpart: hardware partition (FAILSAFE_MMC_HWPART_*)
 * @off:    byte offset inside the hardware partition
 * @buf:    destination buffer
 * @max_len: capacity of @buf
 * @read_len: receives the number of bytes read (may be NULL)
 */
int failsafe_mmc_read_region(int hwpart, u64 off, void *buf, size_t max_len,
			     size_t *read_len);

#endif /* CONFIG_MMC */

#endif /* IS_ENABLED(CONFIG_MMC) */
#endif /* _FAILSAFE_MMC_H_ */
