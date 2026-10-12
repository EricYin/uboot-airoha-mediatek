// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha PWM driver - the GPIO "flash/RSP" wave generator of the
 * EN7523/EN7562/AN7563/AN7581/AN7583 SoCs.
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * The block lives in the GPIO controller (base 0x1fbf0200) and is made of
 * three register sections, exactly as the device tree describes (see the
 * vendor DTS, "airoha,airoha-pwm"):
 *
 *   section 1 (0x1fbf0234, +0x00):
 *     FLASH_MODE_CFG    per pin flash/PWM enable, pins 0..15 -> BIT(pin)
 *     RSP_MODE_CFG      per mapped pin "response mode" bit
 *     FLASH_PRD_SET0..3 two wave generators each: low[15:8] | high[7:0]
 *     FLASH_MAP_CFG0/1  four bits per mapped pin: wave[2:0] | enable[3]
 *   section 2 (0x1fbf0268, +0x00):
 *     FLASH_MODE_CFG_EXT  pins 16..31 -> BIT(pin-16), 36..51 -> BIT(pin-20)
 *   section 3 (0x1fbf0298, +0x00):
 *     CYCLE_CFG_VALUE0/1  four wave generators each, one 8 bit cycle per wave
 *
 * There are only eight wave generators for 52 pins, so a pin does not own a
 * generator: every pin maps (through its "mapped gpio" index, 16 of them)
 * onto one of the eight waves, and all pins that share a mapped index share
 * the wave and therefore the same period and duty cycle.  A pin can use a
 * wave that another pin already drives only when both want the same signal;
 * this is the same restriction the vendor driver implements ("OTHER GPIO IN
 * THIS GROUP IS USING PWM").
 *
 * A PWM channel is a GPIO pin number (0..51, pins 32..35 are not routable),
 * which is what the "pwm" command passes in:
 *
 *   pwm config 0 5 20000000 10000000   # pin 5, 20 ms period, 50% duty
 *   pwm enable 0 5
 *   pwm disable 0 5
 *
 * Periods between 4 ms and 1 s (1..250 Hz) are supported: the hardware
 * counts 250 steps per period, so the 8 bit cycle field covers that range.
 * Polarity inversion is not implemented by the hardware.
 *
 * The pin function itself (the flash mode bit of the pin) is shared with the
 * pinctrl driver's "pwm" function: this driver sets it when a channel is
 * enabled and clears it when the channel is disabled, so a plain
 * "pwm enable" is enough and both ways of selecting the function agree.
 * The pin's output enable (GPIO_OE) is programmed here as well, because the
 * wave generator only drives the pin when it is an output - the vendor
 * driver does the same from the GPIO side (SET_GPIO_OE()).
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <div64.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/time.h>
#include <pwm.h>

/* registers, relative to the base of their section */
#define REG_FLASH_MODE_CFG		0x00
#define REG_RSP_MODE_CFG		0x04
#define REG_FLASH_PRD_SET0		0x08
#define REG_FLASH_PRD_SET1		0x0c
#define REG_FLASH_PRD_SET2		0x10
#define REG_FLASH_PRD_SET3		0x14
#define REG_FLASH_MAP_CFG0		0x18
#define REG_FLASH_MAP_CFG1		0x1c

#define REG_FLASH_MODE_CFG_EXT		0x00

#define REG_CYCLE_CFG_VALUE0		0x00
#define REG_CYCLE_CFG_VALUE1		0x04

/* the GPIO output enable register, relative to section 1 */
#define GPIO_OE_FROM_SECT1		(0x14 - 0x34)

#define PWM_PIN_MAX			52
#define PWM_PIN_UNSUPPORTED_FIRST	32
#define PWM_PIN_UNSUPPORTED_LAST	35

#define PWM_WAVE_NUM			8
#define PWM_MAP_GROUP_NUM		16
#define PWM_MAP_GROUP_MEMBER_MAX	5

/* four bits per mapped pin in FLASH_MAP_CFG0/1 */
#define PWM_MAP_WAVE_MASK		0x7
#define PWM_MAP_ENABLE			BIT(3)

/* one 8 bit cycle and one 8 bit high/low pair per wave */
#define PWM_CYCLE_MAX			250
#define PWM_DUTY_MAX			255
#define PWM_PERIOD_MIN_NS		4000000ULL
#define PWM_PERIOD_MAX_NS		1000000000ULL

/*
 * Pins that share a "mapped gpio" index share the wave generator behind it,
 * so they must use the same period and duty cycle.  Index 0..7 map onto a
 * single pin, 8..15 onto five pins each (the vendor's map_group_member
 * table, which is what its check_member_flash() walks).
 */
static const int pwm_map_group_member[PWM_MAP_GROUP_NUM]
				    [PWM_MAP_GROUP_MEMBER_MAX] = {
	{ 0, -1, -1, -1, -1 },
	{ 1, -1, -1, -1, -1 },
	{ 2, -1, -1, -1, -1 },
	{ 3, -1, -1, -1, -1 },
	{ 4, -1, -1, -1, -1 },
	{ 5, -1, -1, -1, -1 },
	{ 6, -1, -1, -1, -1 },
	{ 7, -1, -1, -1, -1 },
	{ 8, 16, 24, 36, 44 },
	{ 9, 17, 25, 37, 45 },
	{ 10, 18, 26, 38, 46 },
	{ 11, 19, 27, 39, 47 },
	{ 12, 20, 28, 40, 48 },
	{ 13, 21, 29, 41, 49 },
	{ 14, 22, 30, 42, 50 },
	{ 15, 23, 31, 43, 51 },
};

struct airoha_pwm_pin_state {
	u32 cycle;		/* 8 bit period divider */
	u32 low;		/* 8 bit complement of the duty */
	u32 high;		/* 8 bit duty */
	bool configured;	/* set_config() was called */
	bool enabled;		/* the wave generator drives this pin */
	bool oe_saved;
	bool oe_input;		/* the pin was an input before we took it */
};

struct airoha_pwm_priv {
	void __iomem *sect1;
	void __iomem *sect2;
	void __iomem *sect3;
	void __iomem *gpio_oe;
	struct airoha_pwm_pin_state pin[PWM_PIN_MAX];
};

static bool airoha_pwm_pin_valid(uint pin)
{
	if (pin >= PWM_PIN_MAX)
		return false;

	/* pins 32..35 have no route to the wave generators */
	return pin < PWM_PIN_UNSUPPORTED_FIRST ||
	       pin > PWM_PIN_UNSUPPORTED_LAST;
}

/*
 * Map a pin number onto its "mapped gpio" index (0..15).  Only the pins that
 * can reach the flash/RSP hardware have one; pin 32..35 have none.
 */
static int airoha_pwm_map_pin(uint pin)
{
	if (pin < 16)
		return pin;
	if (pin < 24)
		return (pin & 15) + 8;
	if (pin < 32)
		return pin & 15;
	/* pins 32..35 have no route to the wave generators */
	if (pin < PWM_PIN_UNSUPPORTED_FIRST)
		return -EINVAL;
	if (pin <= PWM_PIN_UNSUPPORTED_LAST)
		return -EINVAL;
	if (pin < 44)
		return pin - 28;
	if (pin < PWM_PIN_MAX)
		return pin - 36;

	return -EINVAL;
}

/* the flash mode (pin function) register and bit of a pin */
static void __iomem *airoha_pwm_flash_mode_reg(struct airoha_pwm_priv *priv,
					      uint pin, u32 *mask)
{
	if (pin < 16) {
		*mask = BIT(pin);
		return priv->sect1 + REG_FLASH_MODE_CFG;
	}

	*mask = pin < 32 ? BIT(pin - 16) : BIT(pin - 20);
	return priv->sect2 + REG_FLASH_MODE_CFG_EXT;
}

static void __iomem *airoha_pwm_map_reg(struct airoha_pwm_priv *priv,
					int mapped)
{
	return priv->sect1 +
	       (mapped < 8 ? REG_FLASH_MAP_CFG0 : REG_FLASH_MAP_CFG1);
}

static u32 airoha_pwm_map_shift(int mapped)
{
	return (mapped & 7) * 4;
}

/* the cycle register of a wave and its 8 bit field position */
static void __iomem *airoha_pwm_cycle_reg(struct airoha_pwm_priv *priv,
					  int wave, u32 *shift)
{
	*shift = (wave & 3) * 8;

	return priv->sect3 + (wave < 4 ? REG_CYCLE_CFG_VALUE0 :
					 REG_CYCLE_CFG_VALUE1);
}

/* the pulse register of a wave: low[15:8] | high[7:0] */
static void __iomem *airoha_pwm_pulse_reg(struct airoha_pwm_priv *priv,
					  int wave, u32 *shift)
{
	*shift = (wave & 1) * 16;

	return priv->sect1 + REG_FLASH_PRD_SET0 + (wave >> 1) * 4;
}

static void airoha_pwm_wave_write(struct airoha_pwm_priv *priv, int wave,
				  u32 cycle, u32 low, u32 high)
{
	void __iomem *reg;
	u32 val, shift;

	reg = airoha_pwm_cycle_reg(priv, wave, &shift);
	val = readl(reg);
	val &= ~(0xff << shift);
	val |= (cycle & 0xff) << shift;
	writel(val, reg);

	reg = airoha_pwm_pulse_reg(priv, wave, &shift);
	val = readl(reg);
	val &= ~(0xffff << shift);
	val |= ((low & 0xff) << 8 | (high & 0xff)) << shift;
	writel(val, reg);
}

static void airoha_pwm_wave_read(struct airoha_pwm_priv *priv, int wave,
				 u32 *cycle, u32 *low, u32 *high)
{
	void __iomem *reg;
	u32 val, shift;

	reg = airoha_pwm_cycle_reg(priv, wave, &shift);
	*cycle = (readl(reg) >> shift) & 0xff;

	reg = airoha_pwm_pulse_reg(priv, wave, &shift);
	val = (readl(reg) >> shift) & 0xffff;
	*high = val & 0xff;
	*low = (val >> 8) & 0xff;
}

/* whether the pin is currently driven by the wave generator */
static bool airoha_pwm_pin_in_flash_mode(struct airoha_pwm_priv *priv,
					 uint pin)
{
	u32 mask;
	void __iomem *reg = airoha_pwm_flash_mode_reg(priv, pin, &mask);

	return readl(reg) & mask;
}

/* whether another pin of the same mapped group already uses the wave */
static bool airoha_pwm_group_active(struct airoha_pwm_priv *priv, uint pin)
{
	int mapped = airoha_pwm_map_pin(pin);
	int i;

	if (mapped < 0)
		return false;

	for (i = 0; i < PWM_MAP_GROUP_MEMBER_MAX; i++) {
		int member = pwm_map_group_member[mapped][i];

		if (member < 0 || member == pin)
			continue;

		if (airoha_pwm_pin_in_flash_mode(priv, member))
			return true;
	}

	return false;
}

/* the wave the group of a pin is mapped onto, -1 if it is not mapped */
static int airoha_pwm_group_wave(struct airoha_pwm_priv *priv, int mapped)
{
	u32 val;

	val = readl(airoha_pwm_map_reg(priv, mapped)) >>
	      airoha_pwm_map_shift(mapped);

	if (!(val & PWM_MAP_ENABLE))
		return -1;

	return val & PWM_MAP_WAVE_MASK;
}

/* whether an enabled pin (of any group) is driven by this wave */
static bool airoha_pwm_wave_in_use(struct airoha_pwm_priv *priv, int wave)
{
	int mapped;

	for (mapped = 0; mapped < PWM_MAP_GROUP_NUM; mapped++) {
		if (airoha_pwm_group_wave(priv, mapped) == wave)
			return true;
	}

	return false;
}

static bool airoha_pwm_wave_matches(struct airoha_pwm_priv *priv, int wave,
				    u32 cycle, u32 low, u32 high)
{
	u32 w_cycle, w_low, w_high;

	airoha_pwm_wave_read(priv, wave, &w_cycle, &w_low, &w_high);

	return w_cycle == cycle && w_low == low && w_high == high;
}

/*
 * Pick the wave generator for a pin: a group that is already driven must keep
 * its wave (and needs an identical signal), otherwise a wave with the same
 * signal is shared and a free one is taken as the last resort.
 */
static int airoha_pwm_select_wave(struct udevice *dev, uint pin)
{
	struct airoha_pwm_priv *priv = dev_get_priv(dev);
	const struct airoha_pwm_pin_state *st = &priv->pin[pin];
	int mapped = airoha_pwm_map_pin(pin);
	int wave, i;

	if (mapped < 0)
		return -EINVAL;

	if (airoha_pwm_group_active(priv, pin)) {
		wave = airoha_pwm_group_wave(priv, mapped);
		if (wave < 0)
			return -EINVAL;

		if (airoha_pwm_wave_matches(priv, wave, st->cycle, st->low,
					    st->high))
			return wave;

		dev_err(dev, "pin %u: other pins of its group already "
			"use a different PWM signal\n", pin);
		return -EBUSY;
	}

	for (i = 0; i < PWM_WAVE_NUM; i++) {
		if (!airoha_pwm_wave_in_use(priv, i))
			continue;
		if (airoha_pwm_wave_matches(priv, i, st->cycle, st->low,
					    st->high))
			return i;
	}

	for (i = 0; i < PWM_WAVE_NUM; i++)
		if (!airoha_pwm_wave_in_use(priv, i))
			return i;

	dev_err(dev, "pin %u: no free wave generator left\n", pin);

	return -ENOSPC;
}

static void airoha_pwm_disable(struct airoha_pwm_priv *priv, uint pin)
{
	int mapped = airoha_pwm_map_pin(pin);
	u32 mask, val, shift;
	void __iomem *reg;

	/* leave the pin function back to the pinctrl/gpio defaults */
	reg = airoha_pwm_flash_mode_reg(priv, pin, &mask);
	clrbits_le32(reg, mask);

	/*
	 * Only give the wave generator up when no other pin of the same
	 * mapped group is still using it.
	 */
	if (mapped >= 0 && !airoha_pwm_group_active(priv, pin)) {
		reg = airoha_pwm_map_reg(priv, mapped);
		shift = airoha_pwm_map_shift(mapped);
		val = readl(reg);
		val &= ~(PWM_MAP_ENABLE << shift);
		writel(val, reg);
	}

	/* an input pin must stay an input */
	if (priv->pin[pin].oe_saved && priv->pin[pin].oe_input)
		clrbits_le32(priv->gpio_oe, BIT(pin));

	priv->pin[pin].enabled = false;
}

static int airoha_pwm_set_enable(struct udevice *dev, uint channel, bool enable)
{
	struct airoha_pwm_priv *priv = dev_get_priv(dev);
	struct airoha_pwm_pin_state *st;
	int mapped, wave;
	u32 shift, val;
	void __iomem *reg;

	if (!airoha_pwm_pin_valid(channel)) {
		dev_err(dev, "pin %u cannot be used as PWM\n", channel);
		return -EINVAL;
	}

	if (!enable) {
		if (priv->pin[channel].enabled)
			airoha_pwm_disable(priv, channel);

		return 0;
	}

	st = &priv->pin[channel];
	if (!st->configured)
		return -EINVAL;

	wave = airoha_pwm_select_wave(dev, channel);
	if (wave < 0)
		return wave;

	mapped = airoha_pwm_map_pin(channel);

	/* remember the direction once, then make the pin an output */
	if (!st->enabled) {
		st->oe_input = !(readl(priv->gpio_oe) & BIT(channel));
		st->oe_saved = true;
	}
	setbits_le32(priv->gpio_oe, BIT(channel));

	airoha_pwm_wave_write(priv, wave, st->cycle, st->low, st->high);

	/* select the pin function (also done by "pinctrl set ... pwm") */
	val = 0;
	reg = airoha_pwm_flash_mode_reg(priv, channel, &val);
	setbits_le32(reg, val);

	/* response mode + mapping of the pin onto its wave */
	setbits_le32(priv->sect1 + REG_RSP_MODE_CFG, BIT(mapped));

	reg = airoha_pwm_map_reg(priv, mapped);
	shift = airoha_pwm_map_shift(mapped);
	val = readl(reg);
	val &= ~(0xf << shift);
	val |= (wave | PWM_MAP_ENABLE) << shift;
	writel(val, reg);

	st->enabled = true;

	dev_dbg(dev, "pin %u on wave %d: cycle %u, low %u, high %u\n",
		channel, wave, st->cycle, st->low, st->high);

	return 0;
}

static int airoha_pwm_set_config(struct udevice *dev, uint channel,
				 uint period_ns, uint duty_ns)
{
	struct airoha_pwm_priv *priv = dev_get_priv(dev);
	struct airoha_pwm_pin_state *st;
	u64 cycle;
	u32 high;

	if (!airoha_pwm_pin_valid(channel)) {
		dev_err(dev, "pin %u cannot be used as PWM\n", channel);
		return -EINVAL;
	}

	if (period_ns < PWM_PERIOD_MIN_NS || period_ns > PWM_PERIOD_MAX_NS) {
		dev_err(dev, "period %u ns outside the supported range "
			"(%llu..%llu ns)\n", period_ns, PWM_PERIOD_MIN_NS,
			PWM_PERIOD_MAX_NS);
		return -EINVAL;
	}

	if (duty_ns > period_ns) {
		dev_err(dev, "duty cycle %u ns exceeds the period\n", duty_ns);
		return -EINVAL;
	}

	/*
	 * The wave generator counts 250 steps per period, so the 8 bit cycle
	 * field is the period in units of 4 ms (the vendor driver scales the
	 * period by 250/1e9 ns as well).
	 */
	cycle = DIV_ROUND_CLOSEST_ULL((u64)period_ns * PWM_CYCLE_MAX,
				      1000000000ULL);
	if (!cycle)
		cycle = 1;
	if (cycle > 0xff)
		cycle = 0xff;

	high = DIV_ROUND_CLOSEST_ULL((u64)PWM_DUTY_MAX * duty_ns, period_ns);

	st = &priv->pin[channel];

	if (st->enabled) {
		struct airoha_pwm_pin_state old = *st;
		int mapped = airoha_pwm_map_pin(channel);
		int wave;

		st->cycle = (u32)cycle;
		st->high = high;
		st->low = PWM_DUTY_MAX - high;

		/*
		 * Re-tune the wave generator this pin is already driving
		 * instead of allocating a second one for the same pin: the pin
		 * stays where it is and no stale cycle/duty values are left in
		 * a wave that is then unused.
		 */
		wave = mapped >= 0 ? airoha_pwm_group_wave(priv, mapped) : -1;

		if (wave >= 0 && airoha_pwm_group_active(priv, channel)) {
			/*
			 * Other pins of the group run off this very wave, so
			 * the signal can only change together with them.
			 */
			if (airoha_pwm_wave_matches(priv, wave, st->cycle,
						    st->low, st->high))
				return 0;

			*st = old;
			dev_err(dev, "pin %u: the wave it shares with other "
				"pins of its group cannot be changed alone\n",
				channel);

			return -EBUSY;
		}

		if (wave < 0)
			wave = airoha_pwm_select_wave(dev, channel);
		if (wave < 0) {
			*st = old;
			return wave;
		}

		airoha_pwm_wave_write(priv, wave, st->cycle, st->low,
				      st->high);

		return 0;
	}

	st->cycle = (u32)cycle;
	st->high = high;
	st->low = PWM_DUTY_MAX - high;
	st->configured = true;

	return 0;
}

static int airoha_pwm_probe(struct udevice *dev)
{
	struct airoha_pwm_priv *priv = dev_get_priv(dev);

	priv->sect1 = dev_remap_addr_index(dev, 0);
	priv->sect2 = dev_remap_addr_index(dev, 1);
	priv->sect3 = dev_remap_addr_index(dev, 2);
	if (!priv->sect1 || !priv->sect2 || !priv->sect3) {
		dev_err(dev, "missing register sections\n");
		return -EINVAL;
	}

	/*
	 * Section 1 is the GPIO block plus 0x34, so the output enable of the
	 * pins we drive sits 0x20 before it.
	 */
	priv->gpio_oe = priv->sect1 + GPIO_OE_FROM_SECT1;

	return 0;
}

static const struct pwm_ops airoha_pwm_ops = {
	.set_config	= airoha_pwm_set_config,
	.set_enable	= airoha_pwm_set_enable,
};

static const struct udevice_id airoha_pwm_ids[] = {
	{ .compatible = "airoha,airoha-pwm" },
	{ .compatible = "airoha,en7523-pwm" },
	{ }
};

U_BOOT_DRIVER(airoha_pwm) = {
	.name		= "airoha-pwm",
	.id		= UCLASS_PWM,
	.of_match	= airoha_pwm_ids,
	.ops		= &airoha_pwm_ops,
	.probe		= airoha_pwm_probe,
	.priv_auto	= sizeof(struct airoha_pwm_priv),
};
