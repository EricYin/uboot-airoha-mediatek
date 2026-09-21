/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 * Simple TAR extractor
 *
 * Ported from board/mediatek/common/untar.h (uboot-mtk-20250711) and moved
 * to the generic library tree (lib/tar) so that any board can parse a TAR
 * archive.  The failsafe Web UI uses it to split an OpenWrt sysupgrade
 * image (a TAR holding "sysupgrade/kernel" and "sysupgrade/root") into the
 * two parts it flashes to separate storage areas.
 */

#ifndef _UNTAR_H_
#define _UNTAR_H_

#include <linux/types.h>

/*
 * A TAR archive is a sequence of 512 byte blocks: every file starts with
 * a 512 byte header block, followed by its data padded to the next block.
 * The first bytes of such a block are what failsafe_image_is_tar() probes.
 */
#define TAR_BLOCK_SIZE		512

enum tar_file_type {
	TAR_FT_UNKNOWN = 0,
	TAR_FT_REGULAR,
	TAR_FT_LINK,
	TAR_FT_CHAR,
	TAR_FT_BLOCK,
	TAR_FT_DIRECTORY,
	TAR_FT_FIFO
};

struct tar_parse_ctx {
	const char *head;
	size_t offset;
	size_t size;
	int eof;
};

struct tar_file_record {
	const char *name;
	const void *data;
	size_t size;
	unsigned int mode;
	enum tar_file_type type;
	unsigned int uid;
	unsigned int gid;
	const char *uname;
	const char *gname;
	const char *linkname;
};

/**
 * tar_ctx_init() - initialize a TAR parse context
 * @ctx: context to initialize
 * @buff: archive contents
 * @size: archive size in bytes
 */
int tar_ctx_init(struct tar_parse_ctx *ctx, const void *buff, size_t size);

/**
 * tar_ctx_next_file() - advance to the next member of the archive
 * @ctx: context (updated in place)
 * @file_record: receives the member description; every pointer points into
 *		 @buff, so the archive must stay valid while it is used
 *
 * Returns 0 when a member was found, -ENODATA at the end of the archive,
 * a negative errno on a malformed archive.
 */
int tar_ctx_next_file(struct tar_parse_ctx *ctx,
		      struct tar_file_record *file_record);

/**
 * tar_header_checksum() - check the header block checksum of a TAR member
 * @buff: at least TAR_BLOCK_SIZE readable bytes
 *
 * Returns 0 when @buff is a valid (non-blank) TAR header block, -EINVAL
 * otherwise.  This is the probe used to tell a TAR archive from any other
 * image format.
 */
int tar_header_checksum(const void *buff);

/**
 * parse_tar_image() - locate the kernel and the rootfs of an OpenWrt image
 * @data: archive contents
 * @size: archive size in bytes
 * @kernel_data: receives a pointer to the kernel payload
 * @kernel_size: receives the kernel size in bytes
 * @rootfs_data: receives a pointer to the rootfs payload
 * @rootfs_size: receives the rootfs size in bytes
 *
 * Searches the archive for the "sysupgrade/kernel" and "sysupgrade/root"
 * members of an OpenWrt sysupgrade image.
 *
 * Returns 0 when both were found, -1 otherwise.
 */
int parse_tar_image(const void *data, size_t size,
		    const void **kernel_data, size_t *kernel_size,
		    const void **rootfs_data, size_t *rootfs_size);

#endif /* _UNTAR_H_ */
