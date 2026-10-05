// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: expansion of a compressed FIP payload
 * (BL31 / U-Boot), shared by the Airoha / MediaTek board code.
 *
 * Each decoder is optional: a build without CONFIG_LZMA simply does not
 * recognise an LZMA-Alone stream, one without CONFIG_XZ does not
 * recognise an .xz container, and the payload is reported as-is.
 */

#include <errno.h>
#include <linux/kconfig.h>
#include <linux/string.h>

#include <failsafe/compress.h>

#if IS_ENABLED(CONFIG_LZMA)
#include <lzma/LzmaTools.h>
#endif

#if IS_ENABLED(CONFIG_XZ)
#include <xz/unxz.h>
#endif

/*
 * The two detectors below do not depend on the matching decoder being
 * built in: a payload is recognised either way, so a caller can tell
 * "not compressed" from "compressed, but this build cannot expand it".
 */

/*
 * LZMA-Alone stream detection: property byte 0x5D (lc=3, lp=0, pb=2, what
 * the lzma and xz tools emit) and the 13-byte minimum header (properties
 * plus the uncompressed size).  The same test is used by is_lzma_data()
 * in tools/airoha_info_bl31.py.
 *
 * The declared dictionary size is deliberately not part of it:
 * LzmaDecode() decodes into the caller's buffer and uses that as the
 * dictionary (see lib/lzma/LzmaDec.c), so the value is never allocated
 * and no amount of it is a reason to reject a payload.  The tools differ
 * widely here - `lzma -c` declares 8 MiB for the Airoha blobs while the
 * mtmips U-Boot is built with `lzma -9` and declares 32 MiB - yet both
 * expand into the same few hundred KiB.
 */
static bool payload_is_lzma(const u8 *data, size_t size)
{
	return size >= 13 && data[0] == 0x5d;
}

/* .xz container magic, see the .xz file format specification (lib/xz/unxz.h). */
static bool payload_is_xz(const u8 *data, size_t size)
{
	static const u8 xz_sig[6] = { 0xfd, '7', 'z', 'X', 'Z', 0x00 };

	return size >= sizeof(xz_sig) && !memcmp(data, xz_sig, sizeof(xz_sig));
}

const char *failsafe_payload_container(const void *data, size_t size)
{
	if (!data || !size)
		return NULL;

	if (payload_is_xz(data, size))
		return "XZ";

	if (payload_is_lzma(data, size))
		return "LZMA";

	return NULL;
}

size_t failsafe_payload_expand_size(const void *data, size_t size)
{
	const u8 *p = data;
	u64 declared = 0;
	int i;

	if (!p || !payload_is_lzma(p, size))
		return FAILSAFE_COMPRESS_SCRATCH_SIZE;

	/* LZMA-Alone: properties (5) + uncompressed size (8, LE).  A build
	 * that knows the size writes it there; all ones means "unknown". */
	for (i = 0; i < 8; i++)
		declared |= (u64)p[5 + i] << (8 * i);

	if (declared < 0x1000 || declared > 0x800000)
		return FAILSAFE_COMPRESS_SCRATCH_SIZE;

	return (size_t)declared;
}

int failsafe_payload_decompress(const void *data, size_t size, void *out,
				size_t out_size, size_t *out_len)
{
	if (!data || !size || !out || !out_size || !out_len)
		return -EINVAL;

#if IS_ENABLED(CONFIG_XZ)
	if (payload_is_xz(data, size)) {
		size_t len = 0;

		if (unxz(data, size, &len, out, out_size) != UNXZ_OK)
			return -EINVAL;

		*out_len = len;
		return 0;
	}
#endif

#if IS_ENABLED(CONFIG_LZMA)
	if (payload_is_lzma(data, size)) {
		SizeT len = out_size;

		if (lzmaBuffToBuffDecompress(out, &len, data, size))
			return -EINVAL;

		*out_len = len;
		return 0;
	}
#endif

	/* Not a container this build can expand: use the payload as it is. */
	return -EOPNOTSUPP;
}
