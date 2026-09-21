/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * EN7516/EN7527 (built by the en751627_reference target) - the big-endian
 * member of the EN7528 MIPS1004Kc family.  It shares the peripheral register
 * map of econet,en7528.dtsi, so the DRAM base and the early stack layout are
 * the same as on EN7528.
 */
#ifndef __CONFIG_EN7516_H
#define __CONFIG_EN7516_H

#define CFG_SYS_SDRAM_BASE	0x80000000
#define CFG_SYS_INIT_SP_OFFSET	0x00800000

#endif /* __CONFIG_EN7516_H */
