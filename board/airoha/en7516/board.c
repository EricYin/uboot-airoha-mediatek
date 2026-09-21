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
	 *
	 * The EN7528 GIC/EIC and CP0 timer handoff cleanup done by
	 * board/airoha/en7528/board.c is deliberately not repeated here:
	 * EN7516/EN7527 boots through its own vendor boot2 stage (see
	 * flash/en751627/boot2.S in the early-boot tree), which does not
	 * leave the EN7528 vendor bootram state behind.
	 */
	return 0;
}
