// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 AIROHA Inc
 *
 * Airoha thermal sensor driver for U-Boot (DM thermal uclass)
 *
 * Author: Elijah Yu <elijah.yu@airoha.com>
 *    Yuzhii0718 <yuzhii0718@outlook.com>
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <fdtdec.h>
#include <limits.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <thermal.h>
#include <asm/io.h>
#include <soc/airoha/thermal.h>

/* Registers inside the chip-SCU window (0x1fa20000) */
#define PLLRG_PROTECT_EN7581		0x268
#define PLLRG_PROTECT_EN7523		0x264
#define PROTECT_KEY_AN7583		0x80
#define PROTECT_KEY_EN7581		0x12

/* ADC mux field: MUX_TADC[3:1] on every SoC that has a TADC */
#define TADC_MUX_SHIFT			1
#define TADC_MUX_MASK			0x7
#define TADC_RST_BIT			4

/* AN7583 selects the diode step through MUX_SENSOR[3:2] */
#define AN7583_SENSOR_SHIFT		2
#define AN7583_SENSOR_MASK		0x3
#define AN7583_SENSOR_D0		0x0
#define AN7583_SENSOR_ZERO		0x1
#define AN7583_SENSOR_D1		0x2

#define ADC_SAMPLES			6	/* drop min+max, average the rest */
#define ADC_SETTLE_MS			10
#define ADC_SETTLE_MS_EN7523		4

struct airoha_sensor {
	u8 mux;			/* value for the ADC mux field */
	s16 slope_x100;		/* ADC code per 0.1C, x100 */
	s16 init_x10;		/* temperature at code30, x10 */
	s16 coeff_x100;		/* AN7583: diode gain coefficient */
	s16 offset;		/* AN7583: 30C offset */
};

struct airoha_thermal_priv;

struct airoha_soc {
	const char *name;
	u32 protect_reg;
	u32 protect_key;
	u8 num_sensors;
	const struct airoha_sensor *sensors;
	int (*init)(struct udevice *dev);
	int (*get)(struct udevice *dev, u8 sensor, int *millicelsius);
};

#define AIROHA_MAX_SENSORS		3

struct airoha_thermal_cal {
	s32 code30;
	s16 slope_x100;
	s16 init_x10;
};

struct airoha_thermal_priv {
	void __iomem *ptp;	/* efuse/PTP registers (0x1efbd000 or SCU) */
	void __iomem *scu;	/* chip SCU */
	void __iomem *np_scu;	/* optional second SCU range (EN7523 PDID) */
	const struct airoha_soc *soc;
	struct airoha_thermal_cal cal[AIROHA_MAX_SENSORS];
	int pin;		/* DT-forced sensor, -1 = report the hottest */
	bool calibrated;
	bool busy;		/* the ADC is shared: one reader at a time */
};

/* EN7523 */
#define EN7523_MUX_TADC			0x2ec
#define EN7523_DOUT_TADC		0x2f0
#define EN7523_EFUSE_REG		0xf20
#define EN7523_IDDQ_REG			0xf28
#define EN7523_XTAL_SEL			0x254
#define EN7523_PKG_SHIFT		14
#define EN7523_PKG_MASK			0x3
#define EN7523_PKG_BGA			3
#define EN7523_PDID_REG			0x5c	/* in the NP-SCU range */
#define EN7523_SLOPE_E2			1810
#define EN7523_SLOPE_E3			1750
#define EN7523_CODE30_DEFAULT_E2	34799
#define EN7523_CODE30_DEFAULT_E3	31662
#define EN7523_BIAS_E2QFP		130
#define EN7523_BIAS_E2BGA		366
#define EN7523_BIAS_E3BGA		346
#define EN7523_NONCAL_TEMP_X10		600
#define EN7523_NONCAL_STEP_X10		8

/* EN7581 / AN7581 */
#define EN7581_MUX_TADC			0x2ec
#define EN7581_DOUT_TADC		0x2f8
#define EN7581_MUX_TADC_CPU		0x2fc
#define EN7581_DOUT_TADC_CPU		0x308
#define EN7581_TADC_SET_MASK		0xf3ff
#define EN7581_TADC_SET_VAL		0x0390
#define EN7581_MUX_DIODE		0x7
#define EN7581_EFUSE_REG		0xf20
#define EN7581_EFUSE_OFFSET_SHIFT	16
#define EN7581_EFUSE_OFFSET_MASK	0xffff
#define EN7581_CPU_SENSOR_REG		0xf28
#define EN7581_SLOPE_DIO_DEFAULT	5710
#define EN7581_SLOPE_DIO_AVS		5645
#define EN7581_INIT_CPK_X10		300
#define EN7581_INIT_FTK_X10		620
#define EN7581_INIT_NONK_X10		550

/* AN7583 */
#define AN7583_MUX_TADC			0x2e4
#define AN7583_DOUT_TADC		0x2f0
#define AN7583_MUX_SENSOR		0x2a0
#define AN7583_TADC_SET_MASK		0x1ffff
#define AN7583_TADC_SET_VAL		0x33bc

/* AN7563 (AN7552/EN7552) */
#define AN7563_MUX_TADC			0x2e4
#define AN7563_DOUT_TADC		0x2f0
#define AN7563_MUX_TADC_CPU		0x2f4
#define AN7563_DOUT_TADC_CPU		0x2fc
#define AN7563_TADC_SET_MASK		0xf3ff
#define AN7563_TADC_SET_VAL		0x33be
#define AN7563_MUX_DIODE		0x7
#define AN7563_MUX_BJT			0x6
#define AN7563_EFUSE_REG		0xf20
#define AN7563_SLOPE_DIODE		5565
#define AN7563_SLOPE_BJT		6059
#define AN7563_INIT_CPK_X10		300
#define AN7563_INIT_NONK_X10		550

/* ------------------------------------------------------------------ */
/* small register helpers                                             */
/* ------------------------------------------------------------------ */

static inline u32 scu_read(struct airoha_thermal_priv *priv, u32 reg)
{
	return readl(priv->scu + reg);
}

static inline void scu_write(struct airoha_thermal_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->scu + reg);
}

static inline u32 ptp_read(struct airoha_thermal_priv *priv, u32 reg)
{
	return readl(priv->ptp + reg);
}

/* Unlock the PLLRG protection so the TADC registers can be written.
 * Returns the value to restore afterwards.
 */
static u32 pllrg_unlock(struct airoha_thermal_priv *priv)
{
	u32 saved = scu_read(priv, priv->soc->protect_reg);

	scu_write(priv, priv->soc->protect_reg, priv->soc->protect_key);

	return saved;
}

static void pllrg_restore(struct airoha_thermal_priv *priv, u32 saved)
{
	scu_write(priv, priv->soc->protect_reg, saved);
}

/* Reset the TADC: vendor code drives the reset bit 0 then 1 while the
 * protection is already lifted by the caller.
 */
static void tadc_reset(struct airoha_thermal_priv *priv, u32 reg)
{
	u32 val;

	val = scu_read(priv, reg);
	scu_write(priv, reg, val & ~BIT(TADC_RST_BIT));
	val = scu_read(priv, reg);
	scu_write(priv, reg, val | BIT(TADC_RST_BIT));
}

/* Program the ADC mux and let the sensor settle (vendor: 10 ms, EN7523: 4 ms) */
static void tadc_select(struct airoha_thermal_priv *priv, u32 reg, u32 mux,
			u32 settle_ms)
{
	u32 val = scu_read(priv, reg);

	val &= ~(TADC_MUX_MASK << TADC_MUX_SHIFT);
	val |= (mux & TADC_MUX_MASK) << TADC_MUX_SHIFT;
	scu_write(priv, reg, val);

	mdelay(settle_ms);
}

/* Six samples, drop the min and the max, average the remaining four */
static u32 adc_read_avg(struct airoha_thermal_priv *priv, u32 out_reg)
{
	u32 min, max, sum, val;
	int i;

	val = scu_read(priv, out_reg);
	min = max = sum = val;

	for (i = 1; i < ADC_SAMPLES; i++) {
		val = scu_read(priv, out_reg);
		sum += val;
		if (val > max)
			max = val;
		else if (val < min)
			min = val;
	}

	return (sum - max - min) >> 2;
}

/* Vendor conversion: (avg - code30) * 1000 / slope + init, in 0.1C units */
static int raw_to_millicelsius(u32 avg, const struct airoha_thermal_cal *cal)
{
	s32 temp_x10;

	temp_x10 = 1000 * ((s32)avg - cal->code30) / cal->slope_x100;
	temp_x10 += cal->init_x10;

	return temp_x10 * 100;
}

/* ------------------------------------------------------------------ */
/* EN7523 / EN7562 / EN7529                                           */
/* ------------------------------------------------------------------ */

static const struct airoha_sensor en7523_sensors[] = {
	{ .mux = 0, .slope_x100 = EN7523_SLOPE_E3, .init_x10 = 300 },
};

static int en7523_thermal_init(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	struct airoha_thermal_cal *cal = &priv->cal[0];
	u32 pkg, pdid, efuse;

	/* TADC reset through the documented EN7523 sequence:
	 * MUX_TADC[7] 0 -> 1, then MUX_TADC[6:0] = 0x70 (initial mode).
	 */
	scu_write(priv, priv->soc->protect_reg, priv->soc->protect_key);

	scu_write(priv, EN7523_MUX_TADC,
		  (scu_read(priv, EN7523_MUX_TADC) & ~BIT(7)) | (0 << 7));
	scu_write(priv, EN7523_MUX_TADC,
		  (scu_read(priv, EN7523_MUX_TADC) & ~BIT(7)) | (1 << 7));
	scu_write(priv, EN7523_MUX_TADC,
		  (scu_read(priv, EN7523_MUX_TADC) & ~0x7f) | 0x70);

	pdid = priv->np_scu ? readl(priv->np_scu + EN7523_PDID_REG) & 0xffff : 0;
	pkg = (scu_read(priv, EN7523_XTAL_SEL) >> EN7523_PKG_SHIFT) & EN7523_PKG_MASK;
	efuse = ptp_read(priv, EN7523_EFUSE_REG);
	priv->calibrated = efuse != 0;

	if (pdid == 2) {
		cal->slope_x100 = EN7523_SLOPE_E2;
		cal->code30 = (pkg == EN7523_PKG_BGA) ? EN7523_BIAS_E2BGA
						     : EN7523_BIAS_E2QFP;
		cal->init_x10 = 300;
		if (priv->calibrated)
			cal->code30 += EN7523_CODE30_DEFAULT_E2;
	} else {
		cal->slope_x100 = EN7523_SLOPE_E3;
		cal->code30 = EN7523_BIAS_E3BGA;
		cal->init_x10 = 300;
		if (priv->calibrated)
			cal->code30 += EN7523_CODE30_DEFAULT_E3;
	}

	dev_dbg(dev, "EN7523: %scalibrated, %s package, code30 %d, slope %d\n",
		priv->calibrated ? "" : "not ", pkg == EN7523_PKG_BGA ? "BGA" : "QFP",
		cal->code30, cal->slope_x100);

	return 0;
}

static int en7523_thermal_get(struct udevice *dev, u8 sensor, int *millicelsius)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved, adc;

	saved = pllrg_unlock(priv);

	/* Vendor read_TADC(): config mode, reset, then mux 0 = thermal */
	scu_write(priv, EN7523_MUX_TADC, 0x70);
	scu_write(priv, EN7523_MUX_TADC, 0xf0);
	scu_write(priv, EN7523_MUX_TADC,
		  (scu_read(priv, EN7523_MUX_TADC) & ~0x7) | (sensor & 0x3));
	mdelay(ADC_SETTLE_MS_EN7523);
	adc = scu_read(priv, EN7523_DOUT_TADC);

	pllrg_restore(priv, saved);

	if (!priv->calibrated) {
		/* Documented fallback: 60C plus the IDDQ bits of PTPSPARE2 */
		*millicelsius = (EN7523_NONCAL_TEMP_X10 +
				 (ptp_read(priv, EN7523_IDDQ_REG) & 0xf) *
				 EN7523_NONCAL_STEP_X10) * 100;
		return 0;
	}

	*millicelsius = raw_to_millicelsius(adc, &priv->cal[0]);

	return 0;
}

/* ------------------------------------------------------------------ */
/* EN7581 / AN7581                                                    */
/* ------------------------------------------------------------------ */

static const struct airoha_sensor en7581_sensors[] = {
	{ .mux = EN7581_MUX_DIODE, .slope_x100 = EN7581_SLOPE_DIO_DEFAULT,
	  .init_x10 = EN7581_INIT_CPK_X10 },
};

static int en7581_thermal_init(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	struct airoha_thermal_cal *cal = &priv->cal[0];
	u32 saved, efuse, cpu_sensor;

	saved = pllrg_unlock(priv);

	/* Both the AVS and the CPU TADC are brought up, as the vendor does */
	tadc_reset(priv, EN7581_MUX_TADC);
	scu_write(priv, EN7581_MUX_TADC,
		  (scu_read(priv, EN7581_MUX_TADC) & ~EN7581_TADC_SET_MASK) |
		  EN7581_TADC_SET_VAL);
	tadc_reset(priv, EN7581_MUX_TADC_CPU);
	scu_write(priv, EN7581_MUX_TADC_CPU,
		  (scu_read(priv, EN7581_MUX_TADC_CPU) & ~EN7581_TADC_SET_MASK) |
		  EN7581_TADC_SET_VAL);
	tadc_select(priv, EN7581_MUX_TADC, EN7581_MUX_DIODE, ADC_SETTLE_MS);

	efuse = ptp_read(priv, EN7581_EFUSE_REG);
	cpu_sensor = ptp_read(priv, EN7581_CPU_SENSOR_REG);
	priv->calibrated = efuse != 0;

	if (priv->calibrated) {
		cal->code30 = (efuse >> EN7581_EFUSE_OFFSET_SHIFT) &
			      EN7581_EFUSE_OFFSET_MASK;
		if (cpu_sensor) {
			cal->slope_x100 = EN7581_SLOPE_DIO_DEFAULT;
			cal->init_x10 = EN7581_INIT_FTK_X10;
		} else {
			cal->slope_x100 = EN7581_SLOPE_DIO_AVS;
			cal->init_x10 = EN7581_INIT_CPK_X10;
		}
	} else {
		cal->code30 = scu_read(priv, EN7581_DOUT_TADC);
		cal->slope_x100 = EN7581_SLOPE_DIO_DEFAULT;
		cal->init_x10 = EN7581_INIT_NONK_X10;
		dev_info(dev, "no thermal calibration efuse, using non calibrated values\n");
	}

	pllrg_restore(priv, saved);

	dev_dbg(dev, "EN7581: %scalibrated %s sensor, code30 %d, slope %d\n",
		priv->calibrated ? "" : "not ",
		cpu_sensor ? "CPU" : "package/AVS",
		cal->code30, cal->slope_x100);

	return 0;
}

static int en7581_thermal_get(struct udevice *dev, u8 sensor, int *millicelsius)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved, avg;

	saved = pllrg_unlock(priv);
	tadc_select(priv, EN7581_MUX_TADC, EN7581_MUX_DIODE, ADC_SETTLE_MS);
	avg = adc_read_avg(priv, EN7581_DOUT_TADC);
	pllrg_restore(priv, saved);

	*millicelsius = raw_to_millicelsius(avg, &priv->cal[0]);

	return 0;
}

/* ------------------------------------------------------------------ */
/* AN7583: bandgap, GBE and CPU sensor behind one ADC mux             */
/* ------------------------------------------------------------------ */

static const struct airoha_sensor an7583_sensors[] = {
	{ .mux = 0, .slope_x100 = 7440, .init_x10 = 300, .coeff_x100 = 973,  .offset = 294 },
	{ .mux = 5, .slope_x100 = 7620, .init_x10 = 300, .coeff_x100 = 995,  .offset = 298 },
	{ .mux = 6, .slope_x100 = 8390, .init_x10 = 300, .coeff_x100 = 1035, .offset = 344 },
};

static int an7583_thermal_init(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved;

	saved = pllrg_unlock(priv);
	tadc_reset(priv, AN7583_MUX_TADC);
	scu_write(priv, AN7583_MUX_TADC,
		  (scu_read(priv, AN7583_MUX_TADC) & ~AN7583_TADC_SET_MASK) |
		  AN7583_TADC_SET_VAL);
	pllrg_restore(priv, saved);

	/* Vendor init leaves the mux on the bandgap sensor, diode 0 */
	saved = pllrg_unlock(priv);
	tadc_select(priv, AN7583_MUX_TADC, an7583_sensors[0].mux, ADC_SETTLE_MS);
	pllrg_restore(priv, saved);

	return 0;
}

/* One diode step: ADC mux = sensor, MUX_SENSOR = step (zero / D0 / D1) */
static u32 an7583_diode(struct airoha_thermal_priv *priv, u32 mux, u32 step)
{
	u32 val;

	val = scu_read(priv, AN7583_MUX_SENSOR);
	val &= ~(AN7583_SENSOR_MASK << AN7583_SENSOR_SHIFT);
	val |= (step & AN7583_SENSOR_MASK) << AN7583_SENSOR_SHIFT;
	scu_write(priv, AN7583_MUX_SENSOR, val);

	tadc_select(priv, AN7583_MUX_TADC, mux, ADC_SETTLE_MS);

	return scu_read(priv, AN7583_DOUT_TADC);
}

static int an7583_thermal_get(struct udevice *dev, u8 sensor, int *millicelsius)
{
	const struct airoha_sensor *s = &an7583_sensors[sensor];
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved, zero, d0, d1;
	int delta, gain, temp_x10;

	saved = pllrg_unlock(priv);
	zero = an7583_diode(priv, s->mux, AN7583_SENSOR_ZERO);
	d0 = an7583_diode(priv, s->mux, AN7583_SENSOR_D0);
	d1 = an7583_diode(priv, s->mux, AN7583_SENSOR_D1);
	pllrg_restore(priv, saved);

	delta = (int)d1 - (int)d0;
	gain = (delta * s->coeff_x100) / 100 + ((int)zero - (int)d1);
	if (gain == 0)
		return -EIO;

	temp_x10 = (s->slope_x100 * delta * 10) / gain - s->offset * 10;
	*millicelsius = temp_x10 * 100;

	return 0;
}

/* ------------------------------------------------------------------ */
/* AN7563 / AN7552 / EN7552: diode and BJT                            */
/* ------------------------------------------------------------------ */

static const struct airoha_sensor an7563_sensors[] = {
	{ .mux = AN7563_MUX_DIODE, .slope_x100 = AN7563_SLOPE_DIODE,
	  .init_x10 = AN7563_INIT_CPK_X10 },
	{ .mux = AN7563_MUX_BJT, .slope_x100 = AN7563_SLOPE_BJT,
	  .init_x10 = AN7563_INIT_CPK_X10 },
};

static int an7563_thermal_init(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved, efuse;

	saved = pllrg_unlock(priv);

	/* TADC and TADC_CPU both get the documented initial mode */
	tadc_reset(priv, AN7563_MUX_TADC);
	scu_write(priv, AN7563_MUX_TADC,
		  (scu_read(priv, AN7563_MUX_TADC) & ~AN7563_TADC_SET_MASK) |
		  AN7563_TADC_SET_VAL);
	tadc_reset(priv, AN7563_MUX_TADC_CPU);
	scu_write(priv, AN7563_MUX_TADC_CPU,
		  (scu_read(priv, AN7563_MUX_TADC_CPU) & ~AN7563_TADC_SET_MASK) |
		  AN7563_TADC_SET_VAL);
	tadc_select(priv, AN7563_MUX_TADC, AN7563_MUX_DIODE, ADC_SETTLE_MS);

	/* PTPSPARE0: [15:0] diode 30C code, [31:16] BJT 30C code */
	efuse = ptp_read(priv, AN7563_EFUSE_REG);
	priv->calibrated = efuse != 0;

	priv->cal[0].slope_x100 = AN7563_SLOPE_DIODE;
	priv->cal[1].slope_x100 = AN7563_SLOPE_BJT;

	if (priv->calibrated) {
		priv->cal[0].code30 = efuse & 0xffff;
		priv->cal[1].code30 = (efuse >> 16) & 0xffff;
		priv->cal[0].init_x10 = AN7563_INIT_CPK_X10;
		priv->cal[1].init_x10 = AN7563_INIT_CPK_X10;
	} else {
		/* Not calibrated: the vendor uses the present ADC reading of
		 * each sensor as its 30C code.
		 */
		tadc_select(priv, AN7563_MUX_TADC, AN7563_MUX_DIODE, ADC_SETTLE_MS);
		priv->cal[0].code30 = adc_read_avg(priv, AN7563_DOUT_TADC);
		tadc_select(priv, AN7563_MUX_TADC, AN7563_MUX_BJT, ADC_SETTLE_MS);
		priv->cal[1].code30 = adc_read_avg(priv, AN7563_DOUT_TADC);
		tadc_select(priv, AN7563_MUX_TADC, AN7563_MUX_DIODE, ADC_SETTLE_MS);

		priv->cal[0].init_x10 = AN7563_INIT_NONK_X10;
		priv->cal[1].init_x10 = AN7563_INIT_NONK_X10;
		dev_info(dev, "no thermal calibration efuse, using non calibrated values\n");
	}

	pllrg_restore(priv, saved);

	return 0;
}

static int an7563_thermal_get(struct udevice *dev, u8 sensor, int *millicelsius)
{
	const struct airoha_sensor *s = &an7563_sensors[sensor];
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	u32 saved, avg;

	saved = pllrg_unlock(priv);
	tadc_select(priv, AN7563_MUX_TADC, s->mux, ADC_SETTLE_MS);
	avg = adc_read_avg(priv, AN7563_DOUT_TADC);
	pllrg_restore(priv, saved);

	*millicelsius = raw_to_millicelsius(avg, &priv->cal[sensor]);

	return 0;
}

/* ------------------------------------------------------------------ */
/* SoC tables                                                         */
/* ------------------------------------------------------------------ */

static const struct airoha_soc en7523_soc = {
	.name = "EN7523",
	.protect_reg = PLLRG_PROTECT_EN7523,
	.protect_key = PROTECT_KEY_AN7583,
	.num_sensors = ARRAY_SIZE(en7523_sensors),
	.sensors = en7523_sensors,
	.init = en7523_thermal_init,
	.get = en7523_thermal_get,
};

static const struct airoha_soc en7581_soc = {
	.name = "EN7581",
	.protect_reg = PLLRG_PROTECT_EN7581,
	.protect_key = PROTECT_KEY_EN7581,
	.num_sensors = ARRAY_SIZE(en7581_sensors),
	.sensors = en7581_sensors,
	.init = en7581_thermal_init,
	.get = en7581_thermal_get,
};

static const struct airoha_soc an7583_soc = {
	.name = "AN7583",
	.protect_reg = PLLRG_PROTECT_EN7581,
	.protect_key = PROTECT_KEY_AN7583,
	.num_sensors = ARRAY_SIZE(an7583_sensors),
	.sensors = an7583_sensors,
	.init = an7583_thermal_init,
	.get = an7583_thermal_get,
};

static const struct airoha_soc an7563_soc = {
	.name = "AN7563",
	.protect_reg = PLLRG_PROTECT_EN7581,
	.protect_key = PROTECT_KEY_AN7583,
	.num_sensors = ARRAY_SIZE(an7563_sensors),
	.sensors = an7563_sensors,
	.init = an7563_thermal_init,
	.get = an7563_thermal_get,
};

/* ------------------------------------------------------------------ */
/* DM plumbing                                                        */
/* ------------------------------------------------------------------ */

/*
 * Take the address of the chip SCU.  EN7523/EN7581/AN7563 point at it with an
 * "airoha,chip-scu" phandle whose first range is the chip SCU (the EN7523
 * merged node carries the NP-SCU, with the PDID, as its second range).
 * AN7583 has the thermal node inside the SCU MFD, so the parent is the SCU.
 */
static int airoha_thermal_get_bases(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	ofnode node;
	fdt_addr_t addr;
	int ret;

	ret = dev_read_phandle_with_args(dev, "airoha,chip-scu", NULL, 0, 0, &args);
	if (!ret) {
		node = args.node;
	} else {
		struct udevice *parent = dev_get_parent(dev);

		if (!parent) {
			dev_err(dev, "no chip-scu phandle and no parent SCU\n");
			return ret;
		}
		node = dev_ofnode(parent);
	}

	addr = ofnode_get_addr(node);
	if (addr == FDT_ADDR_T_NONE) {
		dev_err(dev, "failed to get the chip SCU address\n");
		return -EINVAL;
	}
	priv->scu = (void __iomem *)(uintptr_t)addr;

	/* Optional second range: EN7523 NP-SCU (PDID) */
	addr = ofnode_get_addr_index(node, 1);
	if (addr != FDT_ADDR_T_NONE)
		priv->np_scu = (void __iomem *)(uintptr_t)addr;

	/*
	 * The efuse (PTP spare) registers are read through the device's own
	 * range when it has one; AN7583 is a SCU child and uses the SCU.
	 */
	addr = dev_read_addr(dev);
	if (addr != FDT_ADDR_T_NONE)
		priv->ptp = (void __iomem *)(uintptr_t)addr;
	else
		priv->ptp = priv->scu;

	return 0;
}

static int airoha_thermal_probe(struct udevice *dev)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	const struct airoha_soc *soc;
	u32 pin = 0;
	int ret;

	soc = (const struct airoha_soc *)dev_get_driver_data(dev);
	if (!soc || !soc->init || !soc->get)
		return -EINVAL;

	priv->soc = soc;
	priv->pin = -1;

	ret = airoha_thermal_get_bases(dev);
	if (ret)
		return ret;

	/* Optional: pin the reading to one sensor instead of the hottest one */
	if (!dev_read_u32(dev, "airoha,sensor", &pin) &&
	    pin < soc->num_sensors)
		priv->pin = pin;

	return soc->init(dev);
}

/*
 * Read one sensor.  The sensors share one ADC behind one mux, so a read must
 * not be interleaved with another one (the vendor kernel serialises this with
 * a mutex/spinlock and upstream added a lock for the same reason: two
 * interleaved reads convert another sensor's diodes with the wrong constants).
 * U-Boot runs cooperatively, so a nested reader is refused rather than queued.
 */
static int airoha_thermal_read_one(struct udevice *dev, int sensor, int *temp)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	int ret;

	if (priv->busy)
		return -EBUSY;

	priv->busy = true;
	ret = priv->soc->get(dev, sensor, temp);
	priv->busy = false;

	return ret;
}

/* Driver specific interface, used by the "airoha_thermal" command */
int airoha_thermal_sensor_count(struct udevice *dev)
{
	struct airoha_thermal_priv *priv;

	if (!dev->driver || strcmp(dev->driver->name, "airoha-thermal"))
		return 0;

	priv = dev_get_priv(dev);

	return priv->soc->num_sensors;
}

const char *airoha_thermal_sensor_name(struct udevice *dev, int sensor)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	static const char *const an7583_names[] = { "bandgap", "gbe", "cpu" };
	static const char *const an7563_names[] = { "diode", "bjt" };

	if (sensor < 0 || sensor >= priv->soc->num_sensors)
		return "?";

	if (!strcmp(priv->soc->name, "AN7583"))
		return an7583_names[sensor];
	if (!strcmp(priv->soc->name, "AN7563"))
		return an7563_names[sensor];

	/* Single sensor SoCs: EN7523 thermal diode, EN7581 CPU/package diode */
	return !strcmp(priv->soc->name, "EN7581") ? "diode" : "thermal";
}

int airoha_thermal_sensor_temp(struct udevice *dev, int sensor,
			       int *millicelsius)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);

	if (sensor < 0 || sensor >= priv->soc->num_sensors)
		return -EINVAL;

	return airoha_thermal_read_one(dev, sensor, millicelsius);
}

static int airoha_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	int hottest = INT_MIN;
	int ret = 0;
	int i;

	/* A sensor pinned in the device tree is reported as it is */
	if (priv->pin >= 0)
		return airoha_thermal_read_one(dev, priv->pin, temp);

	/* Otherwise report the hottest sensor, like the vendor driver does */
	for (i = 0; i < priv->soc->num_sensors; i++) {
		int mc;

		ret = airoha_thermal_read_one(dev, i, &mc);
		if (ret)
			return ret;

		hottest = max(hottest, mc);
	}

	*temp = hottest;

	return 0;
}

static const struct dm_thermal_ops airoha_thermal_ops = {
	.get_temp = airoha_thermal_get_temp,
};

static const struct udevice_id airoha_thermal_ids[] = {
	{ .compatible = "airoha,en7523-thermal", .data = (ulong)&en7523_soc },
	{ .compatible = "airoha,en7581-thermal", .data = (ulong)&en7581_soc },
	{ .compatible = "airoha,an7583-thermal", .data = (ulong)&an7583_soc },
	{ .compatible = "airoha,an7563-thermal", .data = (ulong)&an7563_soc },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(airoha_thermal) = {
	.name		= "airoha-thermal",
	.id		= UCLASS_THERMAL,
	.of_match	= airoha_thermal_ids,
	.ops		= &airoha_thermal_ops,
	.probe		= airoha_thermal_probe,
	.priv_auto	= sizeof(struct airoha_thermal_priv),
};
