// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Mikhail Kshevetskiy <mikhail.kshevetskiy@iopsys.eu>
 */
#include <fdtdec.h>
#include <fdt_support.h>
#include <init.h>
#include <sysreset.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kconfig.h>
#include <linux/sizes.h>
#include <soc/airoha/pkgids.h>

DECLARE_GLOBAL_DATA_PTR;

#define EN7523_DRAM_BASE					0x80000000ULL
#define EN7523_SYS_GLOBAL_PARM				0x1fb00284UL
#define EN7523_SYS_GLOBAL_DRAM_SIZE_MASK	GENMASK(27, 20)
#define EN7523_SYS_GLOBAL_DRAM_SIZE_SHIFT	20

int print_cpuinfo(void)
{
	u32 hir = get_pkg();
	u32 pdidr = get_pdidr();
	u32 pkgid = get_pkgid();
	const char *soc_name = airoha_soc_name_from_regs(hir, pkgid, pdidr);

	if (pkgid != END_PACKAGE_ID) {
		printf("SoC:   Airoha %s\n", soc_name);
	} else {
		printf("SoC:   Airoha EN7523/EN7529/EN7562\n");
	}

	return 0;
}

/*
 * The first-stage bootloader initializes and trains DRAM, then detects its
 * total size using address aliasing. The detected size is stored in the
 * global parameter register in units of 16 MiB.
 *
 * Reading this saved value is preferable to trying to derive total capacity
 * from DRAMC RKCFG, whose RKSIZE and RKMODE fields describe controller rank
 * configuration rather than directly encoding the detected memory size.
 */
static phys_size_t en7523_dram_get_size(void)
{
	void __iomem *sys_global =
		(void __iomem *)EN7523_SYS_GLOBAL_PARM;
	unsigned int size_units;
	u32 value;

	value = readl(sys_global);

	size_units =
		(value & EN7523_SYS_GLOBAL_DRAM_SIZE_MASK) >>
		EN7523_SYS_GLOBAL_DRAM_SIZE_SHIFT;

	debug("EN7523 DRAM: global-param=%08x units=%u\n",
	      value, size_units);

	return (phys_size_t)size_units * SZ_16M;
}

/*
 * Zero means the previous boot stage did not populate the field. DRAM starts
 * at 0x80000000, so no more than 1 GiB can be represented below the 32-bit
 * physical address limit.
 */
static bool en7523_dram_size_valid(phys_size_t size)
{
	return size >= SZ_32M && size <= SZ_1G;
}

/*
 * Detect the DRAM size by writing a pattern above every power of two
 * boundary and looking for address aliasing. get_ram_size() saves and
 * restores the locations it touches.
 *
 * Return: the detected size, 0 on failure.
 */
static phys_size_t en7523_dram_size_probe(void)
{
	long size;

	size = get_ram_size((long *)(uintptr_t)EN7523_DRAM_BASE, SZ_1G);

	debug("EN7523 DRAM: probing reports %ld MiB\n", size >> 20);

	if (size <= 0 || !en7523_dram_size_valid(size))
		return 0;

	return (phys_size_t)size;
}

int dram_init(void)
{
	phys_size_t size;

	gd->ram_base = EN7523_DRAM_BASE;

	size = en7523_dram_get_size();

	if (IS_ENABLED(CONFIG_AIROHA_DRAM_SIZE_PROBE)) {
		phys_size_t probed = en7523_dram_size_probe();

		/* Probing describes the memory that is actually there. */
		if (probed && probed != size) {
			if (size)
				printf("EN7523 DRAM: SCU reports %llu MiB, probing reports %llu MiB, using the probed value\n",
				       (unsigned long long)(size >> 20),
				       (unsigned long long)(probed >> 20));

			size = probed;
		}
	}

	if (en7523_dram_size_valid(size)) {
		gd->ram_size = size;

		debug("EN7523 DRAM: base=%llx size=%llu MiB\n",
		      (unsigned long long)gd->ram_base,
		      (unsigned long long)(gd->ram_size >> 20));

		return 0;
	}

	/* Last resort, trust whatever the device tree describes. */
	printf("Invalid EN7523 DRAM size: %llu MiB, falling back to the device tree\n",
	       (unsigned long long)(size >> 20));

	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	int bank;

	gd->dram[0].start = gd->ram_base;
	gd->dram[0].size = gd->ram_size;

	for (bank = 1; bank < CONFIG_NR_DRAM_BANKS; bank++) {
		gd->dram[bank].start = 0;
		gd->dram[bank].size = 0;
	}

	return 0;
}

#ifdef CONFIG_OF_SYSTEM_SETUP
int ft_system_setup(void *blob, struct bd_info *bd)
{
	u64 start[1] = { gd->ram_base };
	u64 size[1] = { gd->ram_size };

	(void)bd;

	return fdt_fixup_memory_banks(blob, start, size, 1);
}
#endif

void __noreturn reset_cpu(void)
{
	writel(0x80000000, 0x1FB00040);
	while (1) {
		/* loop forever */
	}
}
