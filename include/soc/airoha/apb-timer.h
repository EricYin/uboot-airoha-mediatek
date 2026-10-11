/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Airoha APB timer block
 *
 * Register map of the APB timer block and the NP-SCU handoff carrying the
 * clock it runs on.  Shared by the timer driver and its debug command.
 */

#ifndef __AIROHA_APB_TIMER_H__
#define __AIROHA_APB_TIMER_H__

#include <asm/io.h>
#include <linux/bitops.h>

/* Registers, relative to the block base address (0x1fbf0100) */
#define AIROHA_APB_TIMER_CTL		0x00
#define AIROHA_APB_TIMER_LDV(n)		(0x04 + (n) * 8)
#define AIROHA_APB_TIMER_VLR(n)		(0x08 + (n) * 8)
#define AIROHA_APB_TIMER_BLOCK_SIZE	0x40

/* Timer the vendor firmware dedicates to the tick */
#define AIROHA_APB_TIMER_TICK		1

/* Enable bit of the tick timer inside AIROHA_APB_TIMER_CTL */
#define AIROHA_APB_TIMER_TICK_EN	BIT(AIROHA_APB_TIMER_TICK)

/*
 * Interrupt bit of a timer, mapped by the vendor firmware to the timer's
 * enable bit + 16 (timer1 -> bit 17).
 *
 * It is not configured by U-Boot: it has been observed going high on its own
 * while nothing in the boot chain writes it, so it reads as a flag the timer
 * raises itself (its reload event is the likely source) rather than a
 * configuration bit.  U-Boot takes no timer interrupt, and the counter runs
 * with the bit either way, so the driver leaves it alone and "apbtimer info"
 * reports it.
 *
 * The vendor driver names it AIROHA_TIMER_CTRL_INT_ACK(n): writing a 1 to it
 * clears the pending interrupt, which fits the flag behaviour described above
 * (and is why the vendor sets it, rather than clears it, to acknowledge).
 */
#define AIROHA_APB_TIMER_INT(n)		BIT((n) + 16)

/*
 * Load value picked for the tick timer.  The counter reloads itself every
 * 2^32 ticks; leaving the elapsed count to the timer uclass lets it extend
 * that to 64 bits.
 */
#define AIROHA_APB_TIMER_LOAD		0xffffffff

/*
 * The watchdog is counter 5.  It uses the load and current value registers of
 * timer 5, while the two registers the next counter would own are repurposed
 * as its pretimeout threshold and its reload trigger.
 */
#define AIROHA_APB_TIMER_WDT		5
#define AIROHA_APB_TIMER_WDT_LDV	AIROHA_APB_TIMER_LDV(AIROHA_APB_TIMER_WDT)
#define AIROHA_APB_TIMER_WDT_VLR	AIROHA_APB_TIMER_VLR(AIROHA_APB_TIMER_WDT)
#define AIROHA_APB_TIMER_WDT_THSLD	0x34	/* pretimeout threshold */
#define AIROHA_APB_TIMER_WDT_RLD	0x38	/* reload trigger */
/* Writing this to AIROHA_APB_TIMER_WDT_RLD reloads the counter */
#define AIROHA_APB_TIMER_WDT_RELOAD	BIT(0)

/* Enable bit of the watchdog counter inside AIROHA_APB_TIMER_CTL */
#define AIROHA_APB_TIMER_WDT_TIMER_EN	BIT(AIROHA_APB_TIMER_WDT)
/* Reset the SoC when the counter reaches zero */
#define AIROHA_APB_TIMER_WDT_EN		BIT(25)
/*
 * Pending interrupt of the watchdog counter (the vendor's INT_ACK bit).
 * Writing a 1 clears it; U-Boot takes no timer interrupt but clears a stale
 * one when it arms the watchdog.
 */
#define AIROHA_APB_TIMER_WDT_INT_ACK	AIROHA_APB_TIMER_INT(AIROHA_APB_TIMER_WDT)

/*
 * The load register is 32 bits wide, so the longest timeout a single count
 * fits in is AIROHA_APB_TIMER_WDT_LOAD_MAX ticks of the watchdog clock.
 */
#define AIROHA_APB_TIMER_WDT_LOAD_MAX	0xffffffff

/*
 * NP-SCU global parameter register (SYS_GLOBAL_PARM) written by BL2, see the
 * TF-A port at plat/ecnt/en7523/ecnt_system.c.  Its sys_clk field holds the
 * system clock in MHz; the APB timer ticks at half of it.
 */
#define AIROHA_NP_SCU_SYS_GLOBAL_PARM	0x1fb00284UL
#define AIROHA_NP_SCU_SYS_CLK_MASK	GENMASK(19, 10)
#define AIROHA_NP_SCU_SYS_CLK_SHIFT	10
/* sys_clk is 10 bits wide and the bus clock never exceeds 512 MHz */
#define AIROHA_NP_SCU_SYS_CLK_MAX	512

/**
 * airoha_apb_timer_sys_clk() - system clock published by BL2, in MHz
 *
 * Returns: the system clock in MHz, or 0 when the boot chain did not publish
 *	    a plausible value in SYS_GLOBAL_PARM.
 */
static inline u32 airoha_apb_timer_sys_clk(void)
{
	u32 value = readl((void __iomem *)AIROHA_NP_SCU_SYS_GLOBAL_PARM);

	value = (value & AIROHA_NP_SCU_SYS_CLK_MASK) >>
		AIROHA_NP_SCU_SYS_CLK_SHIFT;

	return value > AIROHA_NP_SCU_SYS_CLK_MAX ? 0 : value;
}

#endif /* __AIROHA_APB_TIMER_H__ */
