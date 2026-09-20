// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: BL31 (ATF EL3 runtime) banner extraction,
 * shared by the Airoha / MediaTek board code.
 *
 * BL31 embeds the same version banner as the BL2 preloader, but the FIP
 * payload is compressed, in one of two containers:
 *   - Airoha    : LZMA-Alone stream (props 0x5D + uncompressed size),
 *                 exactly what tools/airoha_info_bl31.py decodes.
 *   - MediaTek  : the FIP comes in a plain flavour (BL31 raw) and an XZ
 *                 flavour (BL31 inside a .xz container, magic
 *                 FD 37 7A 58 5A 00).
 * A raw payload is scanned directly.
 */

#include <malloc.h>
#include <linux/string.h>

#include <failsafe/bl2.h>
#include <failsafe/bl31.h>

#if IS_ENABLED(CONFIG_LZMA)
#include <lzma/LzmaTools.h>
#endif

#if IS_ENABLED(CONFIG_XZ)
#include <xz/unxz.h>
#endif

/* ------------------------------------------------------------------ */
/*  Payload container detection                                        */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_LZMA)
/*
 * LZMA-Alone stream detection: property byte 0x5D (lc=3, lp=0, pb=2)
 * followed by a dictionary size that fits in a 32-bit word.  The same
 * check is used by is_lzma_data() in tools/airoha_info_bl31.py.
 */
static bool bl31_is_lzma(const u8 *data, size_t size)
{
	u32 dict_size;

	if (size < 13 || data[0] != 0x5d)
		return false;

	dict_size = (u32)data[1] | ((u32)data[2] << 8) |
		    ((u32)data[3] << 16) | ((u32)data[4] << 24);

	return dict_size <= 0x800000;
}
#endif /* CONFIG_LZMA */

#if IS_ENABLED(CONFIG_XZ)
/* .xz container magic, see the .xz file format specification (lib/xz/unxz.h). */
static bool bl31_is_xz(const u8 *data, size_t size)
{
	return size >= sizeof(xz_magic) &&
	       !memcmp(data, xz_magic, sizeof(xz_magic));
}
#endif /* CONFIG_XZ */

bool failsafe_bl31_parse_banner(const void *data, size_t size,
				void *scratch, size_t scratch_size,
				struct failsafe_version_info *info)
{
	const u8 *buf = data;

	if (scratch && scratch_size) {
		size_t out_len = 0;

#if IS_ENABLED(CONFIG_XZ)
		if (bl31_is_xz(buf, size)) {
			size_t xz_len = 0;

			if (unxz(buf, size, &xz_len, scratch, scratch_size) == UNXZ_OK)
				out_len = xz_len;
		}
#endif
#if IS_ENABLED(CONFIG_LZMA)
		if (!out_len && bl31_is_lzma(buf, size)) {
			SizeT len = scratch_size;

			if (!lzmaBuffToBuffDecompress(scratch, &len, buf, size))
				out_len = len;
		}
#endif
		if (out_len)
			return failsafe_bl2_parse_banner(scratch, out_len,
							 info);
	}

	/* Raw payload (or no decoder for its container). */
	return failsafe_bl2_parse_banner(buf, size, info);
}
