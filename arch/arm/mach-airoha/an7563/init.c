// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Ray Liu <ray.xy.liu@gmail.com>
 *
 * AN7563/AN7552 SoC init code.
 *
 * Mirrors the EN7523 ARMv7 init shape: dual Cortex-A53 cluster running in
 * AArch32 mode, no armv8/mmu mm_region table, no aarch64 PSCI helpers.
 */

#include <fdtdec.h>
#include <init.h>
#include <sysreset.h>
#include <asm/system.h>
#include <linux/io.h>

int print_cpuinfo(void)
{
	printf("CPU:   Airoha AN7563/AN7552\n");
	return 0;
}

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

void __noreturn reset_cpu(void)
{
	writel(0x80000000, 0x1FB00040);
	while (1) {
		/* loop forever */
	}
}
