// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 MediaTek Inc.
 */

#include <config.h>
#include <mmc.h>
#include <part.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	/* address of boot parameters */
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;

	return 0;
}

#ifdef CONFIG_MMC
int mmc_get_boot_dev(void)
{
	int g_mmc_devid = -1;
	char *uflag = (char *)0x81DFFFF0;
	struct blk_desc *desc;

	if (blk_get_device_by_str("mmc", "1", &desc) < 0)
		return 0;

	if (strncmp(uflag,"eMMC",4)==0) {
		g_mmc_devid = 0;
		printf("Boot From Emmc(id:%d)\n\n", g_mmc_devid);
	} else {
		g_mmc_devid = 1;
		printf("Boot From SD(id:%d)\n\n", g_mmc_devid);
	}
	return g_mmc_devid;
}

int mmc_get_env_dev(void)
{
	struct udevice *dev;
	const char *mmcdev;

	switch (mmc_get_boot_dev()) {
	case 0:
		mmcdev = "mmc@11230000";
		break;
	case 1:
		mmcdev = "mmc@11240000";
		break;
	default:
		return -1;
	}

	if (uclass_get_device_by_name(UCLASS_MMC, mmcdev, &dev))
		return -1;

	return dev_seq(dev);
}
#endif
