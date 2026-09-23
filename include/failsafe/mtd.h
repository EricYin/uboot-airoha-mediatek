/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Fine-grained MTD range access.
 *
 * The storage layer (failsafe/storage.h) works by partition *name* and
 * writes a whole image at a time; the pages that let the user look after a
 * flash chip - the flash editor (modules/flash.c) and the whole-chip
 * restore (modules/simg.c) - need the layer below: read, erase and program
 * an arbitrary byte range of a device they picked themselves.
 *
 * That layer is here, written against the plain MTD API
 * (mtd_read / mtd_write / mtd_erase) and following what a flash chip
 * requires:
 *
 *   - erasing works on whole erase blocks, so a range that starts or ends
 *     inside a block is written with a read-modify-write cycle that keeps
 *     the bytes outside the range (failsafe_mtd_update_range(),
 *     failsafe_mtd_erase_range());
 *   - programming works on whole write units (pages), so a range is
 *     programmed one write unit at a time (failsafe_mtd_program_range());
 *   - a read can report corrected bit flips (-EUCLEAN), which is not a
 *     failure: the data is what was stored.
 *
 * Both pages used to carry their own copy of all of this.  Bad blocks are
 * *not* handled here - a failure is reported to the caller.  The one caller
 * that has to survive them (a whole-chip restore, where losing a block
 * beats losing the dump) drives the loop itself, see simg_write_range().
 *
 * Without CONFIG_MTD every call reports -ENODEV.
 */

#ifndef _FAILSAFE_MTD_H_
#define _FAILSAFE_MTD_H_

#include <linux/types.h>
#include <linux/mtd/mtd.h>

/**
 * failsafe_mtd_read_range() - read a byte range of an MTD device
 * @mtd: target device
 * @off: absolute flash offset
 * @len: number of bytes to read
 * @buf: destination buffer (@len bytes)
 * @out_len: receives the number of bytes actually read (may be NULL)
 *
 * -EUCLEAN is treated as success: it only reports bit flips the ECC engine
 * corrected.  Any other error is reported as -EIO.
 */
int failsafe_mtd_read_range(struct mtd_info *mtd, u64 off, size_t len,
			    u8 *buf, size_t *out_len);

/**
 * failsafe_mtd_program_range() - program a byte range of an erased area
 * @mtd: target device
 * @off: absolute flash offset
 * @data: payload
 * @len: payload length
 *
 * The range is programmed one write unit (page) at a time.  The caller is
 * responsible for having erased the target blocks beforehand - see
 * failsafe_mtd_erase_blocks(), failsafe_mtd_restore_range() and the
 * read-modify-write cycles in this file.
 */
int failsafe_mtd_program_range(struct mtd_info *mtd, u64 off,
			       const u8 *data, size_t len);

/**
 * failsafe_mtd_erase_blocks() - erase every block touched by a range
 * @mtd: target device
 * @start: absolute flash offset of the first byte
 * @len: number of bytes
 *
 * The range is rounded outwards to whole erase blocks, because that is what
 * the MTD erase API works on: exactly the blocks [start, start + @len)
 * touches are erased, nothing outside them.  Blocks that are only partially
 * covered are erased completely - use failsafe_mtd_erase_range() to keep
 * their remaining bytes.
 */
int failsafe_mtd_erase_blocks(struct mtd_info *mtd, u64 start, u64 len);

/**
 * failsafe_mtd_update_range() - write into a range, block by block
 * @mtd: target device
 * @start: absolute flash offset of the first byte
 * @data: payload
 * @len: payload length
 *
 * Every block the range touches is read back, patched with the part of
 * @data that falls into it, erased and programmed again, so the bytes
 * outside the range keep their content.  This is the one to use for a small
 * update in the middle of a partition (the flash editor's write); to write
 * a whole backup file use failsafe_mtd_restore_range().
 */
int failsafe_mtd_update_range(struct mtd_info *mtd, u64 start,
			      const u8 *data, size_t len);

/**
 * failsafe_mtd_restore_range() - erase a range and program it
 * @mtd: target device
 * @start: absolute flash offset of the first byte
 * @data: payload
 * @len: payload length
 *
 * The blocks the range touches are erased as a whole, then @len bytes are
 * programmed at @start.  Meant for restoring a backup that covers the range
 * (its start is normally block aligned): the bytes between the end of the
 * payload and the end of its last block read back as erased, and a @start
 * inside a block also loses the bytes in front of it.
 */
int failsafe_mtd_restore_range(struct mtd_info *mtd, u64 start,
			       const u8 *data, size_t len);

/**
 * failsafe_mtd_erase_range() - erase a range, keeping the bytes around it
 * @mtd: target device
 * @start: absolute flash offset of the first byte
 * @len: number of bytes
 *
 * Blocks fully covered by the range are erased directly; the partially
 * covered head / tail blocks go through a read-modify-write cycle that
 * fills only the requested bytes with 0xff, so everything outside the range
 * survives.
 */
int failsafe_mtd_erase_range(struct mtd_info *mtd, u64 start, u64 len);

#endif /* _FAILSAFE_MTD_H_ */
