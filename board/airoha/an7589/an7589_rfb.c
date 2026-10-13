// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha AN7589 evaluation board
 */

#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	/* address of boot parameters */
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;

	return 0;
}
