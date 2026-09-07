/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Configuration for Airoha EN7523
 *
 * Author: Mikhail Kshevetskiy <mikhail.kshevetskiy@iopsys.eu>
 */

#ifndef __EN7523_H
#define __EN7523_H

#include <linux/sizes.h>

#define CFG_SYS_UBOOT_BASE		CONFIG_TEXT_BASE

#define CFG_SYS_INIT_RAM_ADDR           CONFIG_TEXT_BASE
#define CFG_SYS_INIT_RAM_SIZE           SZ_2M

/* DRAM */
#define CFG_SYS_SDRAM_BASE		0x80000000

/* Extra environment variables */
#ifdef CONFIG_AIROHA_DEFAULT_FIT_BOOT_CONF
#define FIT_BOOT_CONF_ENV	"bootconf=" CONFIG_AIROHA_DEFAULT_FIT_BOOT_CONF "\0"
#else
#define FIT_BOOT_CONF_ENV
#endif

#define CFG_EXTRA_ENV_SETTINGS	\
	FIT_BOOT_CONF_ENV

#endif
