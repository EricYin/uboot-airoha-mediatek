/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe BL31 (ARM Trusted Firmware EL3
 * runtime) helper.
 *
 * BL31 uses the same "v<version> / Built : <date>" banner as the BL2
 * preloader (see failsafe/bl2.h), but the FIP payload is compressed:
 * LZMA-Alone on Airoha, either raw or inside an XZ container on
 * MediaTek.  Both boards share this helper.
 *
 * The helper is optional, because BL31 is normally packed together with
 * BL33 (U-Boot) inside one FIP: CONFIG_WEBUI_FAILSAFE_BL31 decides
 * whether failsafe/bootimg/bl31.c is built at all and whether
 * failsafe_atf_version_info() fills in the BL31 banner.
 */

#ifndef _FAILSAFE_BL31_H_
#define _FAILSAFE_BL31_H_

#include <linux/kconfig.h>
#include <linux/types.h>
#include <failsafe/version.h>

/*
 * Scratch space for the decompression of a BL31 payload.  The Airoha
 * bl31.lzma expands to roughly 100 KiB, the MediaTek bl31.xz to about
 * 41 KiB; the rest is headroom.
 */
#define FAILSAFE_BL31_SCRATCH_SIZE	0x200000

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)

/**
 * failsafe_bl31_parse_banner() - extract the BL31 version banner
 * @data: BL31 payload as stored in the FIP
 * @size: payload size in bytes
 * @scratch: scratch buffer for the decompression
 * @scratch_size: size of @scratch
 * @info: output structure, fully overwritten
 *
 * @data is decompressed into @scratch first when it carries an LZMA-Alone
 * stream (CONFIG_LZMA) or an .xz container (CONFIG_XZ); a raw payload is
 * scanned directly.  Without the matching decoder the compressed payload
 * simply yields no banner.  The banner rules are identical to the BL2
 * preloader, so failsafe_bl2_parse_banner() performs the extraction.
 *
 * Returns true when at least the version or the build date was found.
 */
bool failsafe_bl31_parse_banner(const void *data, size_t size,
				void *scratch, size_t scratch_size,
				struct failsafe_version_info *info);

#endif /* CONFIG_WEBUI_FAILSAFE_BL31 */

#endif /* _FAILSAFE_BL31_H_ */
