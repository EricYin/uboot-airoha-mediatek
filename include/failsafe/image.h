/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe boot-image helpers (FIT / legacy
 * uImage validation and the RAM-boot path).
 */

#ifndef _FAILSAFE_IMAGE_H_
#define _FAILSAFE_IMAGE_H_

#include <linux/types.h>

/**
 * failsafe_image_validate_fit() - validate a FIT image
 * @data: image contents
 * @size: image size in bytes
 * @what: image type name used in the diagnostics
 *
 * Returns 0 when @data is a valid FIT, -EINVAL otherwise.  When FIT
 * support is not built in every image is rejected.
 */
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what);

/**
 * failsafe_image_validate_legacy() - validate a legacy uImage
 * @data: image contents
 * @size: image size in bytes
 * @what: image type name used in the diagnostics
 *
 * Checks the 64-byte header CRC (ih_hcrc) plus the payload CRC (ih_dcrc)
 * after verifying that the declared payload size fits inside the image.
 *
 * Returns 0 when valid, -EINVAL otherwise.
 */
int failsafe_image_validate_legacy(const void *data, size_t size,
				   const char *what);

/**
 * failsafe_firmware_check_model() - optional strict board-model check
 * @data: image contents
 * @size: image size in bytes
 * @what: image type name used in the diagnostics
 *
 * Enabled by default.  A FIT firmware is only accepted if it declares a
 * 'compatible' that matches the running U-Boot's control device tree
 * (the board this recovery runs on).  Uses fit_conf_find_compat(), the
 * same logic U-Boot uses to select a FIT configuration.  The check is
 * disabled only when the environment variable 'failsafe_strict_model' is
 * set to "0"; any other value (including unset) keeps it enabled.
 *
 * Returns 0 when allowed (or the check is disabled / not applicable),
 * -EINVAL when the firmware does not match the board.
 */
int failsafe_firmware_check_model(const void *data, size_t size,
				  const char *what);

/**
 * failsafe_uboot_check_model() - strict board-model check on a U-Boot
 * @data: U-Boot image (the "nt-fw" / BL33 payload of a FIP)
 * @size: image size in bytes
 * @what: image type name used in the diagnostics
 *
 * The U-Boot counterpart of failsafe_firmware_check_model(): the control
 * device tree U-Boot carries - linked in or appended to the image - must
 * declare a 'compatible' that matches the running U-Boot's control
 * device tree.  The device tree is searched for from the end of the
 * image, so anything that ends in (or contains) the U-Boot image can be
 * passed, not only a FIP entry: a bare u-boot.bin, a composite SPL /
 * TCBoot loader image, a padded flash image.  The payload may be stored
 * compressed (LZMA-Alone on Airoha, .xz on MediaTek with FIP compression
 * enabled) and is expanded transparently; a container this build has no
 * decoder for (CONFIG_LZMA / CONFIG_XZ) makes the check skip.  Disabled
 * only when the environment variable 'failsafe_strict_model' is set to
 * "0".
 *
 * Returns 0 when allowed (or the check is disabled / not applicable),
 * -EINVAL when the U-Boot is built for another board.
 */
int failsafe_uboot_check_model(const void *data, size_t size,
			       const char *what);

/**
 * failsafe_image_is_legacy() - does @data start with a legacy uImage?
 * @data: image contents
 * @size: image size in bytes
 */
bool failsafe_image_is_legacy(const void *data, size_t size);

/**
 * failsafe_image_validate_firmware() - validate a firmware (system) image
 * @data: image contents
 * @size: image size in bytes
 * @what: image type name used in the diagnostics
 *
 * A system image is one single boot image (a FIT / ITB holding the kernel
 * and the rootfs, which the boards supported today use - there is no split
 * layout with separate kernel / rootfs partitions).  It is checked with the
 * structural FIT check plus the opt-in strict board-model gate; legacy
 * uImage firmware is not accepted.
 *
 * Returns 0 when the image is accepted, a negative errno otherwise.
 */
int failsafe_image_validate_firmware(const void *data, size_t size,
				     const char *what);

/**
 * failsafe_boot_image_from_mem() - boot an uploaded image from DRAM
 * @data_load_addr: address the image was uploaded to
 * @image_size: image size in bytes
 * @load_fallback: staging address when "loadaddr" is not set
 *
 * FIT / legacy uImage images (an initramfs) are booted with bootm, any
 * other raw binary is copied to the "loadaddr" address and executed
 * with "go".  A raw image must be position-independent or linked to run
 * at that address.
 */
int failsafe_boot_image_from_mem(ulong data_load_addr, size_t image_size,
				 ulong load_fallback);

#endif /* _FAILSAFE_IMAGE_H_ */
