/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe Web UI - boot mode of the running session.
 *
 * A bootloader can run either from the flash (its own image was stored there
 * by the boot chain) or from RAM, when the image was fetched over the console
 * by the previous boot stage (the TF-A BL2 XMODEM recovery on Airoha, the
 * BootROM XMODEM path on EcoNet).  The second case is a volatile recovery
 * session: nothing the user uploads through the network reaches the flash
 * until it is written explicitly, so forgetting to flash a bootloader - or
 * installing a firmware that does not match the bootloader already in the
 * flash - bricks the device on the next reset.  The Web UI warns about it,
 * see main.js and the "boot" field of GET /sysinfo.
 *
 * The mode is reported by the boot chain where it can be reported at all
 * (see <soc/airoha/bootsrc.h>); everywhere else it stays "unknown" and the
 * Web UI stays quiet instead of guessing.
 */

#ifndef _FAILSAFE_BOOT_MODE_H_
#define _FAILSAFE_BOOT_MODE_H_

#ifdef __cplusplus
extern "C" {
#endif

enum failsafe_boot_mode {
	/* No boot chain report available (unsupported platform, or a boot
	 * chain that does not implement the handoff yet). */
	FAILSAFE_BOOT_UNKNOWN = 0,
	/* The running image was read from the flash. */
	FAILSAFE_BOOT_FLASH,
	/* The running image came over the console and lives in DRAM only. */
	FAILSAFE_BOOT_RAM,
};

/**
 * failsafe_boot_mode() - boot mode of the running session
 *
 * The environment variable 'failsafe_boot_mode' (auto|ram|flash, default
 * "auto") overrides the report of the boot chain; it is meant for testing
 * and for boards whose boot chain cannot report the mode itself.
 *
 * Returns the mode, FAILSAFE_BOOT_UNKNOWN when it cannot be determined.
 */
enum failsafe_boot_mode failsafe_boot_mode(void);

/**
 * failsafe_boot_mode_name() - stable name of a boot mode
 *
 * Returns "ram", "flash" or "unknown"; used for the JSON report of
 * GET /sysinfo, so the names are part of the Web UI contract.
 */
const char *failsafe_boot_mode_name(enum failsafe_boot_mode mode);

#ifdef __cplusplus
}
#endif

#endif /* _FAILSAFE_BOOT_MODE_H_ */
