// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe Web UI - boot mode of the running session.
 *
 * Where the boot chain can report how the running U-Boot was obtained, the
 * answer comes from the platform (see <soc/airoha/bootsrc.h> and
 * <soc/mediatek/bootsrc.h>):
 *
 *   - ARM Airoha (EN7523 / AN7581 / AN7583 / AN7552): BL2 of the TF-A port
 *     publishes the storage the FIP came from in a NP-SCU scratch register,
 *     so a RAM (XMODEM) recovery session is distinguishable from a flash
 *     boot.  This is read on demand, i.e. only when something asks, which
 *     keeps it out of the boot path entirely.
 *   - MediaTek Filogic (mt7981 / mt7986 / mt7987 / mt7988): BL2 of the TF-A
 *     port publishes it in the arguments BL33 is entered with, which the
 *     startup code captures in save_boot_params(); read on demand as well.
 *   - EcoNet MIPS (EN751221): the BootROM latches its XMODEM recovery path
 *     in the CHIP-SCU; the latch is sampled and cleared very early, so the
 *     value is already cached here.
 *   - Everything else (MIPS mtmips, ...): unknown.  The Web UI then shows no
 *     warning; a platform is added by reporting the mode from its own arch
 *     code.
 *
 * The environment variable 'failsafe_boot_mode' (auto|ram|flash, default
 * "auto") overrides the report.  It is deliberately a plain runtime variable
 * (nothing sets it during boot): it exists so a test or a board without a
 * report from the boot chain can force the warning, and it must not be
 * confused with the real state - which is why the header calls the hardware
 * report the source of truth.
 */

#include <env.h>
#include <errno.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/types.h>
#include <failsafe/boot_mode.h>
#include <failsafe/cprint.h>

#if (defined(CONFIG_ARCH_AIROHA) && \
	(defined(CONFIG_ARM) || defined(CONFIG_ARM64))) || \
	defined(CONFIG_ARCH_ECONET)
#include <soc/airoha/bootsrc.h>
#endif

#if defined(CONFIG_ARCH_MEDIATEK) && defined(CONFIG_ARM64)
#include <soc/mediatek/bootsrc.h>
#endif

#if defined(CONFIG_ARCH_ECONET)
/*
 * Weak default: only the SoCs whose BootROM is known to latch the XMODEM
 * recovery path provide the strong definition (en751221).  The others report
 * "unknown" instead of guessing that a boot came from the flash.
 */
__weak bool econet_bootrom_recovery(void)
{
	return false;
}
#endif

/*
 * 'failsafe_boot_mode' = auto (default) | ram | flash.  Anything else is
 * rejected so a typo cannot silently look like a valid forced mode.
 */
static int boot_mode_from_env(enum failsafe_boot_mode *mode)
{
	const char *str = env_get("failsafe_boot_mode");

	if (!str || !str[0] || !strcmp(str, "auto"))
		return -ENOENT;

	if (!strcmp(str, "ram")) {
		*mode = FAILSAFE_BOOT_RAM;
		return 0;
	}

	if (!strcmp(str, "flash")) {
		*mode = FAILSAFE_BOOT_FLASH;
		return 0;
	}

	return -EINVAL;
}

enum failsafe_boot_mode failsafe_boot_mode(void)
{
	enum failsafe_boot_mode mode;

	if (!boot_mode_from_env(&mode))
		return mode;

#if defined(CONFIG_ARCH_AIROHA) && \
	(defined(CONFIG_ARM) || defined(CONFIG_ARM64))
	{
		enum airoha_boot_source src;

		if (!airoha_get_boot_source(&src))
			return (src == AIROHA_BOOT_SOURCE_XMODEM) ?
				FAILSAFE_BOOT_RAM : FAILSAFE_BOOT_FLASH;
	}
#endif

#if defined(CONFIG_ARCH_MEDIATEK) && defined(CONFIG_ARM64)
	{
		enum mtk_boot_source src;

		if (!mtk_get_boot_source(&src))
			return (src == MTK_BOOT_SOURCE_RAM) ?
				FAILSAFE_BOOT_RAM : FAILSAFE_BOOT_FLASH;
	}
#endif

#if defined(CONFIG_ARCH_ECONET)
	if (econet_bootrom_recovery())
		return FAILSAFE_BOOT_RAM;
#endif

	return FAILSAFE_BOOT_UNKNOWN;
}

const char *failsafe_boot_mode_name(enum failsafe_boot_mode mode)
{
	switch (mode) {
	case FAILSAFE_BOOT_RAM:
		return "ram";
	case FAILSAFE_BOOT_FLASH:
		return "flash";
	default:
		return "unknown";
	}
}

void failsafe_boot_mode_print(void)
{
	switch (failsafe_boot_mode()) {
	case FAILSAFE_BOOT_RAM:
		/*
		 * The state the Web UI warns about: everything uploaded in
		 * this session stays in DRAM, so it is the one that ends in a
		 * brick when the flash does not receive a bootloader.
		 */
		cprintln(CAUTION,
			 "Boot source: RAM (volatile session)");
		break;
	case FAILSAFE_BOOT_FLASH:
		cprintln(PROMPT, "Boot source: flash");
		break;
	default:
		cprintln(PROMPT, "Boot source: unknown");
		break;
	}
}
