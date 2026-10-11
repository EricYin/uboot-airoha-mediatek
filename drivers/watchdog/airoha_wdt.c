// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha watchdog driver
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * The Airoha ARM SoCs (EN7523, AN7563, AN7581, AN7583) wire the SoC reset to
 * counter 5 of the APB timer block: with its enable bit set the counter reloads
 * itself from its load value register and, when it reaches zero, asserts the
 * reset.  Servicing the watchdog only reloads the counter from that value.
 *
 * The counter ticks at half the system ("BUS") clock.  Like the timer driver,
 * the rate is taken from the value BL2 publishes in SYS_GLOBAL_PARM; a boot
 * chain that does not fill it in can describe the rate with a clock-frequency
 * property (in Hz) on the watchdog node instead.
 *
 * The vendor driver uses the same block to report CPUs that stopped petting
 * it: it arms the counter, takes its interrupt as an early warning and always
 * keeps a threshold in the pretimeout register.  U-Boot takes no timer
 * interrupt and only needs the reset, so it arms the counter the way the
 * vendor firmware does when it takes the SoC down through the watchdog
 * (TF-A plat_system_reset(), the vendor kernel's watchDogReset()): interrupt
 * bit clear and the threshold at its no-pretimeout value.
 *
 * Names seen for this one block: the vendor driver calls the counter WDG and
 * the tick GPT, an older vendor revision calls it "timer 3" together with the
 * compatible "airoha,an7563-wdt", and the upstream Linux driver calls the
 * compatible "airoha,en7581-wdt".
 *
 * Like the vendor driver, the SCU named by the "airoha,scu" phandle is used to
 * cut the DRAM controller on an7583 before that reset, and to report how the
 * SoC came up.
 */

#include <dm.h>
#include <div64.h>
#include <fdtdec.h>
#include <hang.h>
#include <log.h>
#include <wdt.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>
#include <soc/airoha/apb-timer.h>

/*
 * NP-SCU registers the watchdog reaches out to: the DRAM controller, which
 * an7583 has to cut before it resets (see airoha_wdt_expire_now()), and the
 * power management status that says how the SoC came up.
 */
#define AIROHA_SCU_DRAMC_CONF		0x074
#define   AIROHA_SCU_DRAMC_EN		BIT(2)
#define AIROHA_SCU_PMC			0x080
#define   AIROHA_SCU_PMC_WDG_RST_R	BIT(31)
#define   AIROHA_SCU_PMC_SYS_RST_R	BIT(30)

/* Timeout the vendor driver picks when it resets through the watchdog */
#define AIROHA_WDT_RESET_TIMEOUT_MS	50

struct airoha_wdt_soc_data {
	bool stop_dramc;	/* cut the DRAM controller before resetting */
};

struct airoha_wdt_priv {
	void __iomem *base;
	void __iomem *scu_base;	/* NP-SCU named by the "airoha,scu" phandle */
	u32 clk_hz;		/* watchdog counter input clock */
};

/*
 * Servicing the watchdog reloads the counter from its load value.  The reload
 * register is a trigger, the bit clears itself once the hardware took it.
 */
static int airoha_wdt_ping(struct udevice *dev)
{
	struct airoha_wdt_priv *priv = dev_get_priv(dev);

	writel(AIROHA_APB_TIMER_WDT_RELOAD,
	       priv->base + AIROHA_APB_TIMER_WDT_RLD);

	return 0;
}

static void airoha_wdt_disable(struct airoha_wdt_priv *priv)
{
	clrbits_le32(priv->base + AIROHA_APB_TIMER_CTL,
		     AIROHA_APB_TIMER_WDT_EN |
		     AIROHA_APB_TIMER_WDT_TIMER_EN);
}

static int airoha_wdt_start(struct udevice *dev, u64 timeout_ms, ulong flags)
{
	struct airoha_wdt_priv *priv = dev_get_priv(dev);
	u64 ticks = timeout_ms * priv->clk_hz;

	do_div(ticks, 1000);

	if (ticks > AIROHA_APB_TIMER_WDT_LOAD_MAX) {
		u64 max_ms = (u64)AIROHA_APB_TIMER_WDT_LOAD_MAX * 1000;

		do_div(max_ms, priv->clk_hz);
		dev_warn(dev, "timeout capped to %llu ms, the counter is 32 bit wide\n",
			 max_ms);
		ticks = AIROHA_APB_TIMER_WDT_LOAD_MAX;
	}
	/* a zero count is not well defined, keep the shortest timeout instead */
	if (!ticks)
		ticks = 1;

	/*
	 * Stop the counter before reprogramming it, so an expiry during the
	 * update cannot assert the reset.
	 */
	airoha_wdt_disable(priv);

	/*
	 * U-Boot takes no watchdog interrupt, so the threshold only has to
	 * keep out of the way: the vendor writes the no-pretimeout value
	 * there, and a value left over from an earlier boot could sit above
	 * the load value, where the vendor never puts it.
	 */
	writel(0, priv->base + AIROHA_APB_TIMER_WDT_THSLD);
	writel((u32)ticks, priv->base + AIROHA_APB_TIMER_WDT_LDV);
	/* make sure the reload value is in place before the counter starts */
	readl(priv->base + AIROHA_APB_TIMER_WDT_LDV);

	/*
	 * Take the counter down the way the vendor resets the SoC with it
	 * (TF-A plat_system_reset(), the vendor kernel's watchDogReset()):
	 * start the counter, let it load the new value, and only then let it
	 * reset the SoC.  The interrupt bit stays clear, as it does there.
	 */
	clrbits_le32(priv->base + AIROHA_APB_TIMER_CTL,
		     AIROHA_APB_TIMER_WDT_INT_ACK);
	setbits_le32(priv->base + AIROHA_APB_TIMER_CTL,
		     AIROHA_APB_TIMER_WDT_TIMER_EN);
	airoha_wdt_ping(dev);
	setbits_le32(priv->base + AIROHA_APB_TIMER_CTL, AIROHA_APB_TIMER_WDT_EN);

	return 0;
}

static int airoha_wdt_stop(struct udevice *dev)
{
	airoha_wdt_disable(dev_get_priv(dev));

	return 0;
}

static int airoha_wdt_reset(struct udevice *dev)
{
	return airoha_wdt_ping(dev);
}

/**
 * airoha_wdt_scu_base() - map the NP-SCU the watchdog reaches out to
 * @dev: watchdog device
 *
 * The "airoha,scu" phandle names the SCU node.  Its argument, when there is
 * one, picks the range to use: en7523/en7562 keep the chip SCU in the first
 * range of their SCU node and the NP-SCU in the second, so their watchdog
 * asks for it with "airoha,scu = <&scu 1>".  On an7583 the clock and reset
 * provider is a child of the syscon that carries the reg, so fall back to its
 * parent when the node itself has no address.
 *
 * Return: the mapped SCU base, or NULL when there is none to be had.
 */
static void __iomem *airoha_wdt_scu_base(struct udevice *dev)
{
	struct ofnode_phandle_args args;
	fdt_addr_t addr;
	ofnode node;
	int index = 0;

	if (dev_read_phandle_with_args(dev, "airoha,scu", NULL, 0, 0, &args))
		return NULL;

	if (args.args_count)
		index = args.args[0];

	node = args.node;
	addr = ofnode_get_addr_index(node, index);
	if (addr == FDT_ADDR_T_NONE) {
		node = ofnode_get_parent(node);
		if (ofnode_valid(node))
			addr = ofnode_get_addr_index(node, index);
	}

	if (addr == FDT_ADDR_T_NONE)
		return NULL;

	return (void __iomem *)(uintptr_t)addr;
}

/*
 * Report how the SoC came up; a watchdog reset is otherwise indistinguishable
 * from a power cycle.  Only the interesting cases are printed so a normal boot
 * keeps its console quiet.  The register is a bitmap of every source seen so
 * far and its bits are write-one-to-clear, which is why only the two reported
 * here are cleared: the rest (the power-on and other flags the vendor keeps
 * there) are not ours to drop.
 */
static void airoha_wdt_show_reset_cause(struct udevice *dev)
{
	struct airoha_wdt_priv *priv = dev_get_priv(dev);
	u32 pmc;

	if (!priv->scu_base)
		return;

	pmc = readl(priv->scu_base + AIROHA_SCU_PMC);
	if (!(pmc & (AIROHA_SCU_PMC_WDG_RST_R | AIROHA_SCU_PMC_SYS_RST_R)))
		return;

	printf("Reset: %s\n",
	       (pmc & AIROHA_SCU_PMC_WDG_RST_R) ? "hardware watchdog reset" :
						  "software reset");
	writel(pmc & (AIROHA_SCU_PMC_WDG_RST_R | AIROHA_SCU_PMC_SYS_RST_R),
	       priv->scu_base + AIROHA_SCU_PMC);
}

/*
 * Cut the DRAM controller before the reset hits.  The vendor driver does this
 * on an7583 only, right before it takes the SoC down through the watchdog.
 */
static void airoha_wdt_stop_dramc(struct udevice *dev)
{
	struct airoha_wdt_priv *priv = dev_get_priv(dev);
	u32 val;

	if (!priv->scu_base) {
		dev_warn(dev, "no SCU, DRAM controller left running\n");
		return;
	}

	val = readl(priv->scu_base + AIROHA_SCU_DRAMC_CONF);
	writel(val & ~AIROHA_SCU_DRAMC_EN,
	       priv->scu_base + AIROHA_SCU_DRAMC_CONF);
}

static int airoha_wdt_expire_now(struct udevice *dev, ulong flags)
{
	const struct airoha_wdt_soc_data *soc_data =
		(const struct airoha_wdt_soc_data *)dev_get_driver_data(dev);
	int ret;

	/*
	 * Arm a short timeout and let the SoC reset itself, like the vendor
	 * driver does when it decides to restart.  Report a failure instead of
	 * spinning here: the caller can still tell the user what went wrong.
	 */
	ret = airoha_wdt_start(dev, AIROHA_WDT_RESET_TIMEOUT_MS, flags);
	if (ret)
		return ret;

	if (soc_data && soc_data->stop_dramc)
		airoha_wdt_stop_dramc(dev);

	hang();

	return 0;
}

/**
 * airoha_wdt_clk() - work out the watchdog counter input clock
 * @node: watchdog device node
 *
 * Returns: the counter input clock in Hz, 0 when it cannot be determined.
 */
static ulong airoha_wdt_clk(ofnode node)
{
	u32 sys_clk, rate;

	/* A boot chain without the BL2 handoff can describe the clock itself */
	if (!ofnode_read_u32(node, "clock-frequency", &rate))
		return rate;

	sys_clk = airoha_apb_timer_sys_clk();
	if (!sys_clk) {
		log_debug("%s: no system clock in SYS_GLOBAL_PARM\n", __func__);
		return 0;
	}

	/* the register holds MHz, the counter ticks at half the system clock */
	return (ulong)sys_clk * 500000;
}

static int airoha_wdt_probe(struct udevice *dev)
{
	struct airoha_wdt_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -ENOENT;

	priv->clk_hz = airoha_wdt_clk(dev_ofnode(dev));
	if (!priv->clk_hz) {
		dev_err(dev, "no watchdog clock, add a clock-frequency property\n");
		return -EINVAL;
	}

	/* Only an7583 has to touch the SCU, and only when it resets */
	priv->scu_base = airoha_wdt_scu_base(dev);

	airoha_wdt_show_reset_cause(dev);

	/* Leave the watchdog disarmed, whatever the boot chain set up */
	return airoha_wdt_stop(dev);
}

static const struct wdt_ops airoha_wdt_ops = {
	.start = airoha_wdt_start,
	.stop = airoha_wdt_stop,
	.reset = airoha_wdt_reset,
	.expire_now = airoha_wdt_expire_now,
};

static const struct airoha_wdt_soc_data an7583_wdt_data = {
	.stop_dramc = true,
};

static const struct udevice_id airoha_wdt_ids[] = {
	{ .compatible = "airoha,an7563-wdt" },
	{ .compatible = "airoha,en7581-wdt" },
	{ .compatible = "airoha,en7523-wdt" },
	{ .compatible = "airoha,an7583-wdt", .data = (ulong)&an7583_wdt_data },
	{ }
};

U_BOOT_DRIVER(airoha_wdt) = {
	.name = "airoha_wdt",
	.id = UCLASS_WDT,
	.of_match = airoha_wdt_ids,
	.priv_auto = sizeof(struct airoha_wdt_priv),
	.probe = airoha_wdt_probe,
	.ops = &airoha_wdt_ops,
};
