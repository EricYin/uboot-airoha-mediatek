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
 * The container is expanded by the shared payload helper (see
 * failsafe/bootimg/compress.c); a raw payload is scanned directly.
 */

#include <failsafe/bl2.h>
#include <failsafe/bl31.h>
#include <failsafe/compress.h>

bool failsafe_bl31_parse_banner(const void *data, size_t size,
				void *scratch, size_t scratch_size,
				struct failsafe_version_info *info)
{
	size_t out_len = 0;

	if (scratch && scratch_size &&
	    !failsafe_payload_decompress(data, size, scratch, scratch_size,
					 &out_len) && out_len)
		return failsafe_bl2_parse_banner(scratch, out_len, info);

	/* Raw payload (or no decoder for its container). */
	return failsafe_bl2_parse_banner(data, size, info);
}
