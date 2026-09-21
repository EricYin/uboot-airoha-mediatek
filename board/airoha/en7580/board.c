// SPDX-License-Identifier: GPL-2.0+

#include <init.h>

int board_init(void)
{
	return 0;
}

int board_late_init(void)
{
	/*
	 * DRAM initialization and the post-DRAM flash/chainloader stage are
	 * provided by the external airoha_mips_dramc project; U-Boot proper is
	 * entered after DRAM is already up, so no late board setup is required
	 * here beyond what the generic init sequence already performs.
	 */
	return 0;
}
