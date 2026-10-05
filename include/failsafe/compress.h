/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe boot-payload decompression helper.
 *
 * The Airoha / MediaTek FIP stores its payloads compressed on some
 * builds (BL31 always, U-Boot whenever the build compresses it), in one
 * of two containers:
 *   - Airoha    : LZMA-Alone stream (props 0x5D + uncompressed size),
 *                 exactly what tools/airoha_info_bl31.py decodes.
 *   - MediaTek  : an .xz container (FIP compression), magic
 *                 FD 37 7A 58 5A 00.
 * Both the BL31 banner extraction and the U-Boot board-model check
 * expand such a payload through this helper before looking at it.
 */

#ifndef _FAILSAFE_COMPRESS_H_
#define _FAILSAFE_COMPRESS_H_

#include <linux/types.h>

/*
 * Scratch space for expanding one compressed boot payload.  The Airoha
 * bl31.lzma expands to roughly 100 KiB, the MediaTek bl31.xz to about
 * 41 KiB, and a compressed U-Boot.bin to roughly 1 MiB; the rest is
 * headroom.
 */
#define FAILSAFE_COMPRESS_SCRATCH_SIZE	0x200000

/**
 * failsafe_payload_container() - name of the container a payload uses
 * @data: payload as stored in the FIP
 * @size: payload size in bytes
 *
 * Returns "XZ" or "LZMA" for a recognised compressed container, NULL when
 * the payload is not one.  The detection is independent of the matching
 * decoder being built in (CONFIG_XZ / CONFIG_LZMA), so a caller can tell
 * "not compressed" from "compressed, but this build cannot expand it" and
 * say so in its diagnostics.
 */
const char *failsafe_payload_container(const void *data, size_t size);

/**
 * failsafe_payload_expand_size() - scratch size for expanding a payload
 * @data: payload as stored in the FIP
 * @size: payload size in bytes
 *
 * Returns the number of bytes to allocate for the expansion of @data.
 * An LZMA-Alone stream declares its uncompressed size, so the allocation
 * can be as small as the payload really needs - which matters where the
 * heap is small (1 MiB on the MediaTek MIPS boards).  Streams that do not
 * declare a size (XZ, or an LZMA-Alone header that says "unknown") get
 * FAILSAFE_COMPRESS_SCRATCH_SIZE.  Uses the same detection as
 * failsafe_payload_decompress(), so the two never disagree.
 */
size_t failsafe_payload_expand_size(const void *data, size_t size);

/**
 * failsafe_payload_decompress() - expand a compressed boot payload
 * @data: payload as stored in the FIP
 * @size: payload size in bytes
 * @out: scratch buffer receiving the expanded payload
 * @out_size: size of @out
 * @out_len: receives the size of the expanded payload
 *
 * A payload that is not a container this build can expand is left alone
 * and reported as such, so the caller can use @data as it is.
 *
 * Returns 0 when @data was expanded into @out, -EOPNOTSUPP when it is not
 * a container this build expands (not compressed at all, or no decoder
 * for it is built in - see failsafe_payload_container()), a negative
 * errno when it is one but could not be expanded (corrupt stream, @out
 * too small or out of memory).
 */
int failsafe_payload_decompress(const void *data, size_t size, void *out,
				size_t out_size, size_t *out_len);

#endif /* _FAILSAFE_COMPRESS_H_ */
