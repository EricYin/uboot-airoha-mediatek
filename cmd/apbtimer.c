// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha APB timer debug command
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Reports the APB timer the airoha-timer driver runs as the U-Boot tick and
 * validates it from the shell:
 *
 *   apbtimer info       - tick source, its clock and the raw registers
 *   apbtimer dump       - dump of the whole APB timer register block
 *   apbtimer check [ms] - measure the timer frequency in a [ms] window
 *			   (default 200) against the Arm generic timer
 *
 * The Arm generic timer is the only clock in the SoC that can validate the
 * APB timer without external equipment: its frequency does not come from the
 * same BL2 handoff, and the kernel relies on it too.
 */

#include <command.h>
#include <dm.h>
#include <timer.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <soc/airoha/apb-timer.h>

DECLARE_GLOBAL_DATA_PTR;

#define APBTIMER_CHECK_DEF_MS	200
#define APBTIMER_CHECK_MAX_MS	5000
/* Deviation above which the clock is reported as mismatching: 0.1% */
#define APBTIMER_CHECK_MAX_PPM	1000

/* Physical count of the Arm generic timer */
static u64 apbtimer_cntpct(void)
{
#ifdef CONFIG_ARM64
	u64 val;

	asm volatile("isb; mrs %0, cntpct_el0" : "=r" (val));

	return val;
#else
	u32 low, high;

	asm volatile("mrrc p15, 0, %0, %1, c14" : "=r" (low), "=r" (high));

	return ((u64)high << 32) | low;
#endif
}

/* Frequency of the Arm generic timer, 0 when the boot chain left it unset */
static u32 apbtimer_cntfrq(void)
{
	u32 val;

#ifdef CONFIG_ARM64
	asm volatile("mrs %0, cntfrq_el0" : "=r" (val));
#else
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (val));
#endif

	return val;
}

/*
 * The tick source U-Boot settled on: the same device get_ticks() uses, so
 * what is inspected here is what actually drives the shell's delays.
 */
static struct udevice *apbtimer_dev(void)
{
	if (!gd->timer && dm_timer_init())
		return NULL;

	return gd->timer;
}

static int apbtimer_info(void)
{
	struct udevice *dev = apbtimer_dev();
	struct timer_dev_priv *uc_priv;
	void __iomem *base;
	u32 ctl, ldv, vlr;
	u64 ticks, ms;

	if (!dev) {
		printf("no timer device (is CONFIG_AIROHA_TIMER enabled?)\n");
		return CMD_RET_FAILURE;
	}

	base = dev_read_addr_ptr(dev);
	uc_priv = dev_get_uclass_priv(dev);
	if (!uc_priv->clock_rate) {
		printf("timer has no clock rate, the driver probe did not finish\n");
		return CMD_RET_FAILURE;
	}

	ctl = readl(base + AIROHA_APB_TIMER_CTL);
	ldv = readl(base + AIROHA_APB_TIMER_LDV(AIROHA_APB_TIMER_TICK));
	vlr = readl(base + AIROHA_APB_TIMER_VLR(AIROHA_APB_TIMER_TICK));
	timer_get_count(dev, &ticks);

	ms = ticks * 1000;
	do_div(ms, uc_priv->clock_rate);

	printf("APB timer info\n");
	printf("  device   : %s (%s)\n", dev->name, dev->driver->name);
	printf("  base     : %08lx\n", (ulong)base);
	printf("  clock    : %lu Hz\n", uc_priv->clock_rate);
	printf("  sys clock: %u MHz (SYS_GLOBAL_PARM %08x)\n",
	       airoha_apb_timer_sys_clk(),
	       readl((void __iomem *)AIROHA_NP_SCU_SYS_GLOBAL_PARM));
	printf("  CTL      : %08x (timer%d enable=%u, int=%u)\n", ctl,
	       AIROHA_APB_TIMER_TICK, !!(ctl & AIROHA_APB_TIMER_TICK_EN),
	       !!(ctl & AIROHA_APB_TIMER_INT(AIROHA_APB_TIMER_TICK)));
	printf("  LDV%d     : %08x\n", AIROHA_APB_TIMER_TICK, ldv);
	printf("  VLR%d     : %08x\n", AIROHA_APB_TIMER_TICK, vlr);
	printf("  elapsed  : %llu ms (%llu s.%03llu), %llu ticks\n",
	       ms, ms / 1000, ms % 1000, ticks);

	return CMD_RET_SUCCESS;
}

static int apbtimer_dump(void)
{
	struct udevice *dev = apbtimer_dev();
	void __iomem *base;
	int i;

	if (!dev) {
		printf("no timer device (is CONFIG_AIROHA_TIMER enabled?)\n");
		return CMD_RET_FAILURE;
	}

	base = dev_read_addr_ptr(dev);

	printf("APB timer registers @ %08lx\n", (ulong)base);
	for (i = 0; i < AIROHA_APB_TIMER_BLOCK_SIZE / sizeof(u32); i++) {
		if (!(i % 4))
			printf("\n%08lx:", (ulong)base + i * sizeof(u32));
		printf(" %08x", readl(base + i * sizeof(u32)));
	}
	printf("\n\n  CTL=0x00, LDV(n)=0x04+8n, VLR(n)=0x08+8n,\n"
	       "  WDOG_THSLD(0..3)=0x34/0x44/0x48/0x4c, WDOG_RLD=0x38\n");

	return CMD_RET_SUCCESS;
}

static int apbtimer_check(ulong window_ms)
{
	struct udevice *dev = apbtimer_dev();
	ulong rate;
	u32 frq;
	u64 c0, c1, a0, a1, dc, da, hz, diff, ppm;

	if (!dev) {
		printf("no timer device (is CONFIG_AIROHA_TIMER enabled?)\n");
		return CMD_RET_FAILURE;
	}

	frq = apbtimer_cntfrq();
	if (!frq) {
		printf("CNTFRQ is 0, no Arm generic timer reference available\n");
		return CMD_RET_FAILURE;
	}

	rate = timer_get_rate(dev);
	if (!rate) {
		printf("timer has no clock rate, the driver probe did not finish\n");
		return CMD_RET_FAILURE;
	}

	c0 = apbtimer_cntpct();
	timer_get_count(dev, &a0);
	mdelay(window_ms);
	c1 = apbtimer_cntpct();
	timer_get_count(dev, &a1);

	dc = c1 - c0;
	da = a1 - a0;
	if (!dc || a1 <= a0) {
		printf("no progress: generic %llu ticks, apb %llu ticks\n",
		       dc, da);
		return CMD_RET_FAILURE;
	}

	/* Ticks per second the APB timer runs at, measured on the generic one */
	hz = div64_u64(da * frq, dc);

	diff = hz > rate ? hz - rate : rate - hz;
	ppm = div64_u64(diff * 1000000, rate);

	printf("APB timer check (%lu ms window)\n", window_ms);
	printf("  arm generic: %llu ticks @ %u Hz (%llu ms)\n",
	       dc, frq, div64_u64(dc * 1000, frq));
	printf("  apb timer  : %llu ticks (%llu ms at the driver rate)\n",
	       da, div64_u64(da * 1000, rate));
	printf("  measured   : %llu Hz, driver says %lu Hz\n", hz, rate);
	/* the Hz figure shows sub-ppm residuals, ppm stays truncated */
	printf("  deviation  : %s%llu Hz, %llu ppm\n",
	       hz >= rate ? "+" : "-", diff, ppm);
	printf("  verdict    : %s\n", ppm <= APBTIMER_CHECK_MAX_PPM ?
	       "OK" : "MISMATCH, check the system clock or clock-frequency");

	return ppm <= APBTIMER_CHECK_MAX_PPM ? CMD_RET_SUCCESS :
					       CMD_RET_FAILURE;
}

static int do_apbtimer(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	ulong window_ms;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "info"))
		return apbtimer_info();

	if (!strcmp(argv[1], "dump"))
		return apbtimer_dump();

	if (!strcmp(argv[1], "check")) {
		window_ms = APBTIMER_CHECK_DEF_MS;
		if (argc > 2) {
			window_ms = dectoul(argv[2], NULL);
			if (!window_ms || window_ms > APBTIMER_CHECK_MAX_MS) {
				printf("window must be 1..%d ms\n",
				       APBTIMER_CHECK_MAX_MS);
				return CMD_RET_USAGE;
			}
		}

		return apbtimer_check(window_ms);
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	apbtimer, 3, 1, do_apbtimer,
	"inspect the Airoha APB timer used as the U-Boot tick",
	"info - tick source, its clock and the raw timer registers\n"
	"apbtimer dump - dump the whole APB timer register block\n"
	"apbtimer check [ms] - measure the timer frequency over a window of\n"
	"    [ms] (default 200) against the Arm generic timer and report the\n"
	"    deviation; this is the only check that can prove the tick rate\n"
	"    is physically right, as everything else is self-consistent."
);
