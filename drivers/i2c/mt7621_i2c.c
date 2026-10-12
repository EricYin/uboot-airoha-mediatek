// SPDX-License-Identifier: GPL-2.0+
/*
 * U-Boot driver for the MediaTek MT7621 I2C controller.
 *
 * Derived from the Linux kernel driver:
 *   drivers/i2c/busses/i2c-mt7621.c
 *
 * Copyright (C) 2013 Steven Liu <steven_liu@mediatek.com>
 * Copyright (C) 2014 Sittisak <sittisaks@hotmail.com>
 * Copyright (C) 2016 Michael Lee <igvtee@gmail.com>
 * Copyright (C) 2018 Jan Breuer <jan.breuer@jaybee.cz>
 * Copyright (C) 2025 Justin Swartz <justin.swartz@risingedge.co.za>
 */

#include <asm/io.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <dm.h>
#include <clk.h>
#include <i2c.h>
#include <log.h>
#include <reset.h>
#include <time.h>

#define REG_SM0CFG2                   0x28
#define REG_SM0CTL0                   0x40
#define REG_SM0CTL1                   0x44
#define REG_SM0D0                     0x50
#define REG_SM0D1                     0x54

#define SM0CFG2_MODE_MANUAL           0

#define SM0CTL0_ODRAIN                BIT(31)
#define SM0CTL0_CLK_DIV_MASK          (0x7ff << 16)
#define SM0CTL0_CLK_DIV_MAX           0x7ff
#define SM0CTL0_EN                    BIT(1)
#define SM0CTL0_SCL_STRETCH           BIT(0)

#define SM0CTL1_TRI                   BIT(0)
#define SM0CTL1_TRI_IDLE              0
#define SM0CTL1_START                 (1 << 4)
#define SM0CTL1_WRITE                 (2 << 4)
#define SM0CTL1_STOP                  (3 << 4)
#define SM0CTL1_READ_LAST             (4 << 4)
#define SM0CTL1_READ                  (5 << 4)
#define SM0CTL1_PGLEN(x)              ((((x) - 1) << 8) & SM0CTL1_PGLEN_MASK)
#define SM0CTL1_PGLEN_MASK            (0x7 << 8)
#define SM0CTL1_ACK_MASK              (0xff << 16)

#define TIMEOUT_1SEC                  1000
#define I2C_MAX_STD_MODE_FREQ         100000

/*
 * The original MT7621 driver clamped the clock divider to a fixed minimum of
 * 99.  With the 50 MHz clock source of that SoC this works out to "SCL is never
 * faster than 500 kHz", so the same limit is expressed here as a frequency:
 * MT7621 is unaffected, while on the Airoha SoCs - whose SIF clock is 20 MHz -
 * the 400 kHz that the Airoha vendor driver and its DT use is reachable again.
 * The fixed floor of 99 used to cap those boards at 200 kHz.
 */
#define I2C_MAX_SCL_FREQ              500000

/*
 * Known limitations of this driver:
 *
 *  - SM0CTL0_CLK_DIV is handled as 11 bit wide, like the original MT7621
 *    driver does.  The Airoha vendor driver treats the field as 12 bit
 *    (0xfff << 16), so one of the two is wrong.  It only makes a difference
 *    below ~9.8 kHz SCL with a 20 MHz clock (bit 27 is never set for the
 *    speeds programmed here), so the field is left as it is instead of
 *    guessing which description the silicon follows.
 *
 *  - 400 kHz is what the Airoha vendor driver programs from its DT, so the
 *    controller is expected to support it, but the resulting SCL has not been
 *    measured on Airoha hardware with this driver yet.  Check it with a scope
 *    (or against a device that is known to be strict about timing) before
 *    relying on fast mode.
 */

struct mt7621_i2c_priv {
	void __iomem *base;
	uint speed;
	u32 clk_div;
	struct clk clk;
	struct reset_ctl reset_ctl;
};

static int mt7621_i2c_wait_idle(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	ulong start_time = get_timer(0);
	u32 value;

	while (get_timer(start_time) < TIMEOUT_1SEC) {
		value = readl(priv->base + REG_SM0CTL1);
		if ((value & SM0CTL1_TRI) == SM0CTL1_TRI_IDLE)
			return 0;

		udelay(10);
	}

	return -ETIMEDOUT;
}

/* Program the divider of the current speed and keep the controller enabled */
static void mt7621_i2c_apply_clk_div(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	u32 value = readl(priv->base + REG_SM0CTL0);

	value &= ~SM0CTL0_CLK_DIV_MASK;
	value |= (priv->clk_div << 16) & SM0CTL0_CLK_DIV_MASK;
	value |= SM0CTL0_EN | SM0CTL0_SCL_STRETCH;
	writel(value, priv->base + REG_SM0CTL0);
}

static int mt7621_i2c_reset(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);

	/* the reset control is optional, the registers are set up either way */
	if (priv->reset_ctl.dev) {
		reset_assert(&priv->reset_ctl);
		udelay(100);
		reset_deassert(&priv->reset_ctl);
	}

	mt7621_i2c_apply_clk_div(dev);

	writel(SM0CFG2_MODE_MANUAL, priv->base + REG_SM0CFG2);
	return 0;
}

static int mt7621_i2c_master_start(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);

	writel(SM0CTL1_START | SM0CTL1_TRI, priv->base + REG_SM0CTL1);
	return mt7621_i2c_wait_idle(dev);
}

static int mt7621_i2c_master_stop(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);

	writel(SM0CTL1_STOP | SM0CTL1_TRI, priv->base + REG_SM0CTL1);
	return mt7621_i2c_wait_idle(dev);
}

static int mt7621_i2c_master_cmd(struct udevice *dev, u32 cmd, int len)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);

	writel(cmd | SM0CTL1_TRI | SM0CTL1_PGLEN(len),
	       priv->base + REG_SM0CTL1);

	return mt7621_i2c_wait_idle(dev);
}

static int mt7621_i2c_7bit_address(struct udevice *dev, struct i2c_msg *msg)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	u32 addr = msg->addr << 1;

	if (msg->flags & I2C_M_RD)
		addr |= 1;

	writel(addr, priv->base + REG_SM0D0);
	return mt7621_i2c_master_cmd(dev, SM0CTL1_WRITE, 1);
}

static int mt7621_i2c_10bit_address(struct udevice *dev, struct i2c_msg *msg)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	u16 addr = 0xf0 | ((msg->addr >> 7) & 0x06) | (msg->addr & 0xff) << 8;

	if (msg->flags & I2C_M_RD)
		addr |= 1;

	writel(addr, priv->base + REG_SM0D0);
	return mt7621_i2c_master_cmd(dev, SM0CTL1_WRITE, 2);
}

static int mt7621_i2c_address(struct udevice *dev, struct i2c_msg *msg)
{
	int ret;

	if (msg->flags & I2C_M_TEN) {
		ret = mt7621_i2c_10bit_address(dev, msg);
		if (ret)
			return ret;
	} else {
		ret = mt7621_i2c_7bit_address(dev, msg);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt7621_i2c_check_ack(struct udevice *dev, struct i2c_msg *msg,
				u32 length)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	u32 status = readl(priv->base + REG_SM0CTL1);
	u32 expected = GENMASK(length - 1, 0);
	u32 mask = (expected << 16) & SM0CTL1_ACK_MASK;

	if (msg->flags & I2C_M_IGNORE_NAK)
		return 0;

	if ((status & mask) != mask)
		return -ENXIO;

	return 0;
}

static int mt7621_i2c_master_read(struct udevice *dev, struct i2c_msg *msg)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	int offset, length, last, ret;
	u32 cmd;
	u32 data[2];

	for (offset = 0; offset < msg->len; offset += 8) {
		if (msg->len - offset >= 8)
			length = 8;
		else
			length = msg->len - offset;

		last = msg->len - offset <= 8;
		cmd = last ? SM0CTL1_READ_LAST : SM0CTL1_READ;
		ret = mt7621_i2c_master_cmd(dev, cmd, length);
		if (ret)
			return ret;

		data[0] = readl(priv->base + REG_SM0D0);
		data[1] = readl(priv->base + REG_SM0D1);
		memcpy(&msg->buf[offset], data, length);
	}

	return 0;
}

static int mt7621_i2c_master_write(struct udevice *dev, struct i2c_msg *msg)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	int offset, length, ret;
	u32 data[2];

	for (offset = 0; offset < msg->len; offset += 8) {
		if (msg->len - offset >= 8)
			length = 8;
		else
			length = msg->len - offset;

		memcpy(data, &msg->buf[offset], length);
		writel(data[0], priv->base + REG_SM0D0);
		writel(data[1], priv->base + REG_SM0D1);

		ret = mt7621_i2c_master_cmd(dev, SM0CTL1_WRITE, length);
		if (ret)
			return ret;

		ret = mt7621_i2c_check_ack(dev, msg, length);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt7621_i2c_xfer(struct udevice *dev, struct i2c_msg *msgs, int count)
{
	struct i2c_msg *msg;
	int index, ret;

	for (index = 0; index < count; index++) {
		msg = &msgs[index];

		ret = mt7621_i2c_wait_idle(dev);
		if (ret)
			goto reset;

		ret = mt7621_i2c_master_start(dev);
		if (ret)
			goto reset;

		ret = mt7621_i2c_address(dev, msg);
		if (ret)
			goto reset;

		ret = mt7621_i2c_check_ack(dev, msg, 1);
		if (ret)
			goto stop;

		if (msg->flags & I2C_M_RD) {
			ret = mt7621_i2c_master_read(dev, msg);
			if (ret)
				goto reset;
		} else {
			ret = mt7621_i2c_master_write(dev, msg);
			if (ret)
				goto reset;
		}
	}

	ret = mt7621_i2c_wait_idle(dev);
	if (ret)
		goto reset;

	ret = mt7621_i2c_master_stop(dev);
	if (ret)
		goto reset;

	return 0;

stop:
	ret = mt7621_i2c_master_stop(dev);
	if (ret)
		goto reset;
	return -ENXIO;

reset:
	mt7621_i2c_reset(dev);
	return ret;
}

static int mt7621_i2c_set_speed(struct udevice *dev, uint speed)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	ulong clk_rate = clk_get_rate(&priv->clk);
	u32 clk_div, min_div;

	if (!speed || !clk_rate)
		return -EINVAL;

	clk_div = clk_rate / speed - 1;

	/* keep SCL at or below I2C_MAX_SCL_FREQ, see the comment at the top */
	min_div = clk_rate / I2C_MAX_SCL_FREQ - 1;
	if (clk_div < min_div)
		clk_div = min_div;

	if (clk_div > SM0CTL0_CLK_DIV_MAX)
		clk_div = SM0CTL0_CLK_DIV_MAX;

	priv->speed = speed;
	priv->clk_div = clk_div;

	/* apply it now, so a runtime speed change takes effect immediately */
	mt7621_i2c_apply_clk_div(dev);

	return 0;
}

static const struct dm_i2c_ops mt7621_i2c_ops = {
	.xfer          = mt7621_i2c_xfer,
	.set_bus_speed = mt7621_i2c_set_speed,
	.deblock       = mt7621_i2c_reset,
};

static int mt7621_i2c_of_to_plat(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);

	priv->base = dev_remap_addr(dev);
	return 0;
}

int mt7621_i2c_probe(struct udevice *dev)
{
	struct mt7621_i2c_priv *priv = dev_get_priv(dev);
	u32 speed = I2C_MAX_STD_MODE_FREQ;
	int ret;

	priv->base = dev_remap_addr(dev);
	if (!priv->base) {
		dev_err(dev, "failed to get base address\n");
		return -EINVAL;
	}

	/*
	 * The source clock is needed to compute the divider, so it is required.
	 * Accept the name used by the Airoha DTs, the one from the upstream
	 * MT7621 binding, and finally the first entry, so a DT without
	 * clock-names is usable as well.
	 */
	ret = clk_get_by_name(dev, "sys_clock", &priv->clk);
	if (ret)
		ret = clk_get_by_name(dev, "i2c", &priv->clk);
	if (ret)
		ret = clk_get_by_index(dev, 0, &priv->clk);
	if (ret) {
		dev_err(dev, "failed to get clock source\n");
		return ret;
	}

	/*
	 * A reset is only used to put the controller into a known state before
	 * the registers are programmed, so a DT without one still works.  Note
	 * that a failed lookup leaves reset_ctl zeroed, which is what
	 * mt7621_i2c_reset() tests for.
	 */
	ret = reset_get_by_name(dev, "i2c_reset", &priv->reset_ctl);
	if (ret)
		ret = reset_get_by_name(dev, "i2c", &priv->reset_ctl);
	if (ret)
		reset_get_by_index(dev, 0, &priv->reset_ctl);

	ret = clk_enable(&priv->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		return ret;
	}

	/* clock-frequency is the standard way to request a bus speed */
	dev_read_u32(dev, "clock-frequency", &speed);
	mt7621_i2c_set_speed(dev, speed);
	mt7621_i2c_reset(dev);

	return 0;
}

static const struct udevice_id mt7621_i2c_ids[] = {
	{ .compatible = "mediatek,mt7621-i2c" },
	{ }
};

U_BOOT_DRIVER(mt7621_i2c) = {
	.name       = "mt7621_i2c",
	.id         = UCLASS_I2C,
	.of_match   = mt7621_i2c_ids,
	.of_to_plat = mt7621_i2c_of_to_plat,
	.probe      = mt7621_i2c_probe,
	.priv_auto  = sizeof(struct mt7621_i2c_priv),
	.ops        = &mt7621_i2c_ops,
};
