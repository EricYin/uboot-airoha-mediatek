/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe FIP (Firmware Image Package) helper.
 *
 * The helper is board independent: it parses the FIP container used by
 * both the Airoha and the MediaTek boot chains (BL2 / preloader, BL31,
 * U-Boot), so the board code and the image validators do not each carry
 * their own copy of the ToC walk.
 */

#ifndef _FAILSAFE_FIP_H_
#define _FAILSAFE_FIP_H_

#include <linux/types.h>

/* FIP container, see doc/board/airoha/boot-images.rst:
 *   header: magic u32 le (0xAA640001) + serial u32 le + flags u64 le
 *   ToC:    40-byte entries (uuid[16], offset u64 le, size u64 le, flags)
 *           terminated by an all-zero uuid entry.
 * Payload offsets are relative to the FIP start.
 */
#define FAILSAFE_FIP_MAGIC		0xAA640001
#define FAILSAFE_FIP_HEADER_SIZE	16
#define FAILSAFE_FIP_TOC_ENTRY_SIZE	40
#define FAILSAFE_FIP_TOC_MAX_ENTRIES	32

/*
 * FIP ToC UUID of the trusted boot firmware (BL2 / preloader) payload:
 * 5ff9ec0b-4d22-3e4d-a544-c39d81c73f0a, stored on disk in the canonical
 * text byte order.
 */
extern const u8 failsafe_fip_uuid_tb_fw[16];

/*
 * FIP ToC UUID of the EL3 runtime firmware (BL31) payload:
 * 47d4086d-4cfe-9846-9b95-2950cbbd5a00, stored on disk in the canonical
 * text byte order.
 */
extern const u8 failsafe_fip_uuid_soc_fw[16];

/**
 * failsafe_fip_check() - does a FIP header start at data + fip_off?
 * @data: image contents
 * @size: image size in bytes
 * @fip_off: offset of the FIP inside @data
 *
 * Returns true when the FIP magic is present at @fip_off.
 */
bool failsafe_fip_check(const void *data, size_t size, size_t fip_off);

/**
 * failsafe_fip_find() - find a ToC entry and return its payload
 * @data: image contents
 * @size: image size in bytes
 * @fip_off: offset of the FIP inside @data
 * @uuid: ToC entry UUID to look for (FIP byte order), or NULL for the
 *        first non-terminal entry
 * @payload: receives a pointer to the payload inside @data (NULL when
 *           the entry exists but its payload is out of range)
 * @payload_size: receives the payload size (the declared size on error)
 * @payload_off: optional, receives the payload offset relative to the
 *               FIP start (may be NULL)
 *
 * Returns 0 on success, -ENOENT when no matching entry exists (also for
 * a missing FIP header, so callers can fall back), -EINVAL when the
 * matched entry points outside the image.
 */
int failsafe_fip_find(const void *data, size_t size, size_t fip_off,
		      const u8 *uuid, const u8 **payload,
		      size_t *payload_size, u64 *payload_off);

/**
 * failsafe_fip_find_at() - find a ToC entry at the first matching FIP
 * @data: image contents
 * @size: image size in bytes
 * @fip_offsets: candidate FIP offsets, probed in order
 * @num_offsets: number of entries in @fip_offsets
 * @uuid: ToC entry UUID to look for, or NULL for the first entry
 * @payload: receives a pointer to the payload inside @data
 * @payload_size: receives the payload size
 *
 * Same as failsafe_fip_find() but probes several FIP offsets (a boot
 * image may carry its FIP at 0, or behind a BL1 prefix at 0x800).
 *
 * Returns 0 on success, -ENOENT when none of the offsets holds a FIP
 * with the requested entry.
 */
int failsafe_fip_find_at(const void *data, size_t size,
			 const size_t *fip_offsets, size_t num_offsets,
			 const u8 *uuid, const u8 **payload,
			 size_t *payload_size);

/**
 * failsafe_fip_validate() - validate a FIP container
 * @data: image contents
 * @size: image size in bytes
 * @fip_off: offset of the FIP inside @data
 * @want_uuid: ToC entry that must be present and contained in the image,
 *             or NULL to check the magic and every entry's payload
 * @what: image type name used in the diagnostics
 *
 * Returns 0 when valid, -EINVAL otherwise.
 */
int failsafe_fip_validate(const void *data, size_t size, size_t fip_off,
			  const u8 *want_uuid, const char *what);

#endif /* _FAILSAFE_FIP_H_ */
