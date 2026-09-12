/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the MediaTek failsafe image validation module
 * (failsafe_validate.c).
 *
 * The per-image-type structural checks (FIP ToC, BL2 preloader, FIT)
 * live in failsafe_validate.c and are controlled by the master switch
 * CONFIG_MTK_FAILSAFE_VALIDATE ("Failsafe image validation" menu in
 * board/mediatek/Kconfig).  The generic storage capacity checks are done
 * separately by the board write path (failsafe_validate_image) and are
 * always performed.
 *
 * The board independent FIP / BL2 / image parsing itself lives in
 * failsafe/bootimg/ and is shared with the Airoha board code; this
 * header only exposes the platform dispatch entry point.
 *
 * When the master switch is off this header provides a no-op stub, so
 * callers need no #ifdef of their own.
 */

#ifndef _MTK_FAILSAFE_VALIDATE_H_
#define _MTK_FAILSAFE_VALIDATE_H_

#include <linux/kconfig.h>
#include <linux/types.h>
#include <failsafe/fw_type.h>

#if IS_ENABLED(CONFIG_MTK_FAILSAFE_VALIDATE)
/*
 * Structurally validate an uploaded image before it is flashed:
 * per-type checks (BL2 / FIP / FIRMWARE toggles).
 */
int failsafe_validate_image_content(const void *data, size_t size,
				    failsafe_fw_t fw);
#else
static inline int failsafe_validate_image_content(const void *data,
						  size_t size,
						  failsafe_fw_t fw)
{
	(void)data;
	(void)size;
	(void)fw;

	return 0;
}
#endif

#endif /* _MTK_FAILSAFE_VALIDATE_H_ */
