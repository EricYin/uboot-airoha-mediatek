// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <asm/io.h>
#include <asm/mipsregs.h>

#define EN7528_GIC_VL_BASE				0xbf8c8000UL
#define EN7528_GIC_VO_BASE				0xbf8cc000UL
#define EN7528_GIC_VL_RMASK				(EN7528_GIC_VL_BASE + 0x000c)
#define EN7528_GIC_VL_TIMER_MAP			(EN7528_GIC_VL_BASE + 0x0048)
#define EN7528_GIC_VL_OTHER				(EN7528_GIC_VL_BASE + 0x0080)
#define EN7528_GIC_VL_EIC_SHADOW_SET	(EN7528_GIC_VL_BASE + 0x0100)
#define EN7528_GIC_VO_EIC_SHADOW_SET	(EN7528_GIC_VO_BASE + 0x0100)

#define EN7528_GIC_LOCAL_IRQS			0x7fU
#define EN7528_GIC_MAP_TO_PIN0			0x80000000U
#define EN7528_GIC_NUM_EIC_VECTORS		64U
#define EN7528_GIC_NUM_VPS				4U

static void en7528_clear_eic_shadow_sets(void)
{
	u32 cur_vp = read_c0_ebase() & MIPS_EBASE_CPUNUM;
	u32 i, vp;

	/*
	 * The vendor bootram does this after boot2: all 64 EIC vectors are
	 * returned to shadow register set 0 on every VP.  boot2 itself does
	 * not provide that final handoff cleanup.
	 */
	for (i = 0; i < EN7528_GIC_NUM_EIC_VECTORS; i++)
		__raw_writel(0, (void __iomem *)(EN7528_GIC_VL_EIC_SHADOW_SET +
					       i * sizeof(u32)));

	for (vp = 0; vp < EN7528_GIC_NUM_VPS; vp++) {
		if (vp == cur_vp)
			continue;

		__raw_writel(vp, (void __iomem *)EN7528_GIC_VL_OTHER);
		asm volatile("sync" : : : "memory");

		for (i = 0; i < EN7528_GIC_NUM_EIC_VECTORS; i++)
			__raw_writel(0,
				     (void __iomem *)(EN7528_GIC_VO_EIC_SHADOW_SET +
						       i * sizeof(u32)));
	}

	/* Restore redirected local-register access to VP0, as the SDK does. */
	__raw_writel(0, (void __iomem *)EN7528_GIC_VL_OTHER);
	asm volatile("sync" : : : "memory");
}

static void en7528_quiesce_legacy_timer(void)
{
	/*
	 * The vendor EN7528 boot2 leaves the CP0 Count/Compare interrupt
	 * routable through the GIC and maps it to EIC pin 0x28 + VP ID.
	 * Linux CPS/GIC does not use that firmware routing: it installs the
	 * GIC dispatcher on EIC pin 0 and this board uses the external HPT as
	 * its clockevent.
	 *
	 * U-Boot resets Count and Compare to zero during startup.  If U-Boot
	 * stays up for a full Count wrap the CP0 timer therefore becomes
	 * pending again.  Do not pass that pending legacy interrupt to Linux.
	 */
	__raw_writel(EN7528_GIC_LOCAL_IRQS,
		     (void __iomem *)EN7528_GIC_VL_RMASK);
	__raw_writel(EN7528_GIC_MAP_TO_PIN0,
		     (void __iomem *)EN7528_GIC_VL_TIMER_MAP);
	asm volatile("sync" : : : "memory");

	/* Writing Compare clears Cause.TI.  Keep the next match far away. */
	write_c0_count(0);
	write_c0_compare(~0U);
	asm volatile("ehb" : : : "memory");
}

int board_init(void)
{
	if (IS_ENABLED(CONFIG_TARGET_EN7528))
		en7528_clear_eic_shadow_sets();

 	return 0;
 }

void board_quiesce_devices(void)
{
	if (!IS_ENABLED(CONFIG_TARGET_EN7528))
		return;

	/* Re-assert the SDK's clean EIC state immediately before Linux. */
	en7528_clear_eic_shadow_sets();
	en7528_quiesce_legacy_timer();
}
