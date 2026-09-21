/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Generic MMC helpers for the failsafe flash module.
 *
 * These helpers therefore only use the standard U-Boot MMC / block / partition API,
 * so the flash page can offer MMC targets on any board that enables CONFIG_MMC.
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

#if CONFIG_IS_ENABLED(MMC)

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
 * Blocks only partially covered by the request are read back first, so
 * the bytes outside the requested range are preserved (the same
 * read-modify-write behaviour as the MTD path).
 *
 * Returns 0 on success, a negative errno otherwise.
 */
static inline int failsafe_mmc_write(struct mmc *mmc, u64 offset,
				     const void *buf, size_t len)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	u64 blksz, pos, start, end;
	u8 *blkbuf;
	int ret = 0;

	if (!bd || !bd->blksz || !buf || !len)
		return -EINVAL;

	blksz = bd->blksz;
	start = offset & ~(blksz - 1);
	end = (offset + len + blksz - 1) & ~(blksz - 1);

	blkbuf = malloc(blksz);
	if (!blkbuf)
		return -ENOMEM;

	for (pos = start; pos < end; pos += blksz) {
		u64 data_start = max(offset, pos);
		u64 data_end = min(offset + len, pos + blksz);
		size_t copy_len = (size_t)(data_end - data_start);

		if (copy_len != blksz) {
			if (blk_dread(bd, pos / blksz, 1, blkbuf) != 1) {
				ret = -EIO;
				break;
			}
		}

		memcpy(blkbuf + (data_start - pos),
		       (const u8 *)buf + (data_start - offset), copy_len);

		if (blk_dwrite(bd, pos / blksz, 1, blkbuf) != 1) {
			ret = -EIO;
			break;
		}
	}

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

#endif /* CONFIG_IS_ENABLED(MMC) */
#endif /* _FAILSAFE_MMC_H_ */
