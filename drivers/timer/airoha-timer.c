// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha APB timer driver
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * The Airoha ARM SoCs (EN7523, AN7563, AN7581, AN7583) contain a block of
 * eight 32-bit down counters, usually called the APB timer.  Each counter
 * reloads itself from its load value register when it reaches zero, so a
 * counter can be turned into a free running up counter by leaving the load
 * value at its maximum and reading the current value register:
 *
 *	ticks = load - current
 *
 * The block sits at 0x1fbf0100 on all of them.  Following the vendor
 * firmware, the driver uses timer 1: the ASIC delay helpers
 * (delay1us()/delay1ms()) drive timer 2 and the watchdog is wired to
 * timer 3, so those must be left alone.
 *
 * The counter ticks at half the system ("BUS") clock, taken from the value
 * BL2 publishes in the NP-SCU global parameter register, the same one the
 * vendor bootloader and the TF-A delay helpers use.  A board whose boot
 * chain does not fill it in can describe its timer input clock with a
 * clock-frequency property (in Hz) on the timer node.
 *
 * The register map and the clock handoff are in
 * <soc/airoha/apb-timer.h>, shared with the apbtimer debug command.
 */

#include <dm.h>
#include <div64.h>
#include <log.h>
#include <timer.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>
#include <soc/airoha/apb-timer.h>

DECLARE_GLOBAL_DATA_PTR;

struct airoha_apb_timer_priv {
	void __iomem *base;
};

static u64 airoha_apb_timer_get_count(struct udevice *dev)
{
	struct airoha_apb_timer_priv *priv = dev_get_priv(dev);
	u32 current = readl(priv->base +
			    AIROHA_APB_TIMER_VLR(AIROHA_APB_TIMER_TICK));

	/* the counter runs down, report an up counting one */
	return timer_conv_64(AIROHA_APB_TIMER_LOAD - current);
}

/**
 * airoha_apb_timer_clk() - work out the timer input clock
 * @node: timer device node
 * @sys_clk_mhz: receives the system clock the rate was derived from, or 0
 *		 when it came from the device tree
 *
 * Returns: the timer input clock in Hz, 0 when it cannot be determined.
 */
static ulong airoha_apb_timer_clk(ofnode node, u32 *sys_clk_mhz)
{
	u32 sys_clk, rate;

	*sys_clk_mhz = 0;

	/* A boot chain without the BL2 handoff can describe the clock itself */
	if (!ofnode_read_u32(node, "clock-frequency", &rate))
		return rate;

	sys_clk = airoha_apb_timer_sys_clk();
	if (!sys_clk) {
		log_debug("%s: no system clock in SYS_GLOBAL_PARM\n", __func__);
		return 0;
	}

	*sys_clk_mhz = sys_clk;

	/* the register holds MHz, the counter ticks at half the system clock */
	return (ulong)sys_clk * 500000;
}

static int airoha_apb_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct airoha_apb_timer_priv *priv = dev_get_priv(dev);
	u32 sys_clk;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -ENOENT;

	uc_priv->clock_rate = airoha_apb_timer_clk(dev_ofnode(dev), &sys_clk);
	if (!uc_priv->clock_rate) {
		dev_err(dev, "no timer input clock, add a clock-frequency property\n");
		return -EINVAL;
	}

	/*
	 * Stop the counter before reprogramming it.  Only the enable bit of the
	 * tick timer is written: the other counters, the watchdog bits and the
	 * timer's interrupt bit (driven by hardware, see apb-timer.h) are left
	 * as they are.
	 */
	clrbits_le32(priv->base + AIROHA_APB_TIMER_CTL,
		     AIROHA_APB_TIMER_TICK_EN);
	writel(AIROHA_APB_TIMER_LOAD,
	       priv->base + AIROHA_APB_TIMER_LDV(AIROHA_APB_TIMER_TICK));
	/* make sure the reload value is in place before the counter starts */
	readl(priv->base + AIROHA_APB_TIMER_LDV(AIROHA_APB_TIMER_TICK));
	setbits_le32(priv->base + AIROHA_APB_TIMER_CTL,
		     AIROHA_APB_TIMER_TICK_EN);

	/*
	 * The tick rate is the one thing that cannot be told from the shell
	 * alone, and the default loglevel would hide a log message, so report
	 * it unconditionally.  "apbtimer info" shows the same at runtime.
	 */
	if (sys_clk)
		printf("APB timer: %lu Hz (system clock %u MHz / 2)\n",
		       uc_priv->clock_rate, sys_clk);
	else
		printf("APB timer: %lu Hz (clock-frequency)\n",
		       uc_priv->clock_rate);

	return 0;
}

#if CONFIG_IS_ENABLED(BOOTSTAGE)
/*
 * bootstage takes its first timestamps before the driver model is up
 * (initf_bootstage runs before initf_dm), so those early calls have no
 * udevice to work with and report 0.
 */
ulong timer_get_boot_us(void)
{
	u64 ticks, us;

	if (!gd->timer && dm_timer_init())
		return 0;

	if (timer_get_count(gd->timer, &ticks))
		return 0;

	us = ticks * 1000000;
	do_div(us, timer_get_rate(gd->timer));

	return us;
}
#endif

static const struct timer_ops airoha_apb_timer_ops = {
	.get_count = airoha_apb_timer_get_count,
};

static const struct udevice_id airoha_apb_timer_ids[] = {
	{ .compatible = "airoha,airoha-timer" },
	{ }
};

U_BOOT_DRIVER(airoha_apb_timer) = {
	.name = "airoha_apb_timer",
	.id = UCLASS_TIMER,
	.of_match = airoha_apb_timer_ids,
	.priv_auto = sizeof(struct airoha_apb_timer_priv),
	.probe = airoha_apb_timer_probe,
	.ops = &airoha_apb_timer_ops,
};
