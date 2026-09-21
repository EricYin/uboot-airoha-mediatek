// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 * Author: Mikhail Kshevetskiy <mikhail.kshevetskiy@iopsys.eu>
 */
#include <fdt_support.h>
#include <init.h>
#include <sysreset.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <soc/airoha/pkgids.h>

DECLARE_GLOBAL_DATA_PTR;

#define EN7528_RESET_CONTROL				0xbfb00040UL
#define EN7528_SYS_GLOBAL_PARM				0xbfb00284UL
#define NP_SCU_BASE		((void __iomem *)CKSEG1ADDR(0x1fb00000))

int dram_init(void)
{
	u32 value, size_units, size_mb;

	/*
	 * SYS_GLOBAL_PARM stores DRAM size in units of 16 MiB.
	 *
	 * Keep the existing endian-specific field placement here. This matches
	 * the vendor GET_DRAM_SIZE() contract, which returns dram_size << 4.
	 */
	value = __raw_readl((void __iomem *)EN7528_SYS_GLOBAL_PARM);
	if (IS_ENABLED(CONFIG_TARGET_EN751627))
		size_units = value & GENMASK(11, 0);
	else
		size_units = (value >> 20) & GENMASK(11, 0);

	size_mb = size_units << 4;

	debug("EN751627/EN7528 DRAM: global-param=%08x units=%u size=%u MiB\n",
	      value, size_units, size_mb);

	if (size_mb < 32 || size_mb > 512) {
		printf("Invalid EN751627/EN7528 calibrated DRAM size: %u MiB\n",
		       size_mb);
		return -EINVAL;
	}

	gd->ram_size = (phys_size_t)size_mb * SZ_1M;

	return 0;
}

int dram_init_banksize(void)
{
	int bank;

	gd->bd->bi_dram[0].start = gd->ram_base;
	gd->bd->bi_dram[0].size = gd->ram_size;

	for (bank = 1; bank < CONFIG_NR_DRAM_BANKS; bank++) {
		gd->bd->bi_dram[bank].start = 0;
		gd->bd->bi_dram[bank].size = 0;
	}

	return 0;
}

#ifdef CONFIG_OF_SYSTEM_SETUP
int ft_system_setup(void *blob, struct bd_info *bd)
{
	u64 start[1] = { bd->bi_dram[0].start };
	u64 size[1] = { bd->bi_dram[0].size };

	return fdt_fixup_memory_banks(blob, start, size, 1);
}
#endif

void _machine_restart(void)
{
	__raw_writel(0x80000000, (void __iomem *)EN7528_RESET_CONTROL);
	while (1) {
		/* loop forever */
	}
}
