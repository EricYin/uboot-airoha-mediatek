/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the Mediatek failsafe image validation module
 * (failsafe_validate.c).
 *
 * All structural image checks (FIP ToC, BL2 preloader, FIT, legacy
 * uImage) live in failsafe_validate.c and are controlled by the master
 * switch CONFIG_MTK_FAILSAFE_VALIDATE ("Failsafe image validation"
 * menu in board/mediatek/Kconfig).  The generic storage capacity checks
 * (MTD partition size / 'fip' static volume size) are defined in
 * failsafe.c and are always performed by the failsafe write path
 * (failsafe_validate_image), independently of the master switch: they
 * are basic storage-safety gates that must not be compiled out.  When
 * the master switch is off this header provides a no-op stub for the
 * structural validation entry point, so callers need no #ifdef of
 * their own.
 *
 * Independently of the compile-time master switch, the "failsafe_validate"
 * environment variable can turn the structural validation off at runtime
 * (set it to 0/no): failsafe_validate_image() then skips the structural
 * checks but still performs the generic storage capacity checks.
 */

#ifndef _MTK_FAILSAFE_VALIDATE_H_
#define _MTK_FAILSAFE_VALIDATE_H_

#include <failsafe/fw_type.h>
#include <linux/types.h>

/*
 * Structurally validate an uploaded image before it is flashed:
 * per-type checks (UBOOT / BL2 / FIP / CHAINLOADER / FIRMWARE toggles).
 * The generic storage capacity checks are done separately by
 * failsafe_validate_image() in failsafe.c (always enabled).
 */
int failsafe_validate_image_content(const void *data, size_t size,
				    failsafe_fw_t fw);

#endif
