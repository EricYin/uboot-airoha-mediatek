// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Based on the Linux thermal-airoha.c but majorly reworked for U-Boot DM thermal framework.
 *
 * Airoha Thermal Driver - U-Boot DM version
 *
 * Author: Christian Marangi <ansuelsmth@gmail.com>(original driver)
 *  Yuzhii0718 <admin@yuzhii0718.eu.org>
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <fdtdec.h>
#include <limits.h>
#include <malloc.h>
#include <thermal.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/types.h>

/*
 * FIELD_{GET,PREP} may be missing from older U-Boot compat headers that
 * do not pull in the full <linux/bitfield.h> subset. Provide local
 * fallback implementations that match Linux semantics exactly.
 *
 * All masks passed in by this driver are non-zero compile-time constants
 * so __builtin_ctz is well-defined.
 */
#ifndef FIELD_GET
#define FIELD_GET(_mask, _reg)		((typeof(_mask))(((_reg) & (_mask)) >> __builtin_ctz(_mask)))
#endif

#ifndef FIELD_PREP
#define FIELD_PREP(_mask, _val)		((typeof(_mask))((((typeof(_mask))(_val)) << __builtin_ctz(_mask)) & (_mask)))
#endif

/* SCU regs */
#define EN7581_PLLRG_PROTECT			0x268
#define EN7581_PWD_TADC				0x2ec
#define   EN7581_MUX_TADC			GENMASK(3, 1)
#define EN7581_DOUT_TADC			0x2f8
#define   EN7581_DOUT_TADC_MASK			GENMASK(15, 0)

#define AN7583_MUX_SENSOR			0x2a0
#define   AN7583_LOAD_ADJ			GENMASK(3, 2)
#define AN7583_MUX_TADC				0x2e4
#define   AN7583_MUX_TADC_MASK			GENMASK(3, 1)
#define AN7583_DOUT_TADC			0x2f0

#define EN7523_PLLRG_PROTECT		0x264
#define EN7523_MUX_TADC				0x2ec
#define EN7523_DOUT_TADC			0x2f0
#define EN7523_CODE_30_DEFAULT_E2	0x87ef
#define EN7523_CODE_30_DEFAULT_E3	0x7bae
#define EN7523_BIAS_E2QFP			0x82
#define EN7523_BIAS_E2BGA			0x16e
#define EN7523_BIAS_E3BGA			0x15a
#define EN7523_PKG_BGA				3

/* PTP_THERMAL regs */
#define EN7581_TEMPMONCTL0			0x800
#define   EN7581_SENSE3_EN			BIT(3)
#define   EN7581_SENSE2_EN			BIT(2)
#define   EN7581_SENSE1_EN			BIT(1)
#define   EN7581_SENSE0_EN			BIT(0)
#define EN7581_TEMPMONCTL1			0x804
/* period unit calculated in BUS clock * 256 scaling-up */
#define   EN7581_PERIOD_UNIT			GENMASK(9, 0)
#define EN7581_TEMPMONCTL2			0x808
#define   EN7581_FILT_INTERVAL			GENMASK(25, 16)
#define   EN7581_SEN_INTERVAL			GENMASK(9, 0)
#define EN7581_TEMPMONINT			0x80C
#define   EN7581_STAGE3_INT_EN			BIT(31)
#define   EN7581_STAGE2_INT_EN			BIT(30)
#define   EN7581_STAGE1_INT_EN			BIT(29)
#define   EN7581_FILTER_INT_EN_3		BIT(28)
#define   EN7581_IMMD_INT_EN3			BIT(27)
#define   EN7581_NOHOTINTEN3			BIT(26)
#define   EN7581_HOFSINTEN3			BIT(25)
#define   EN7581_LOFSINTEN3			BIT(24)
#define   EN7581_HINTEN3			BIT(23)
#define   EN7581_CINTEN3			BIT(22)
#define   EN7581_FILTER_INT_EN_2		BIT(21)
#define   EN7581_FILTER_INT_EN_1		BIT(20)
#define   EN7581_FILTER_INT_EN_0		BIT(19)
#define   EN7581_IMMD_INT_EN2			BIT(18)
#define   EN7581_IMMD_INT_EN1			BIT(17)
#define   EN7581_IMMD_INT_EN0			BIT(16)
#define   EN7581_TIME_OUT_INT_EN		BIT(15)
#define   EN7581_NOHOTINTEN2			BIT(14)
#define   EN7581_HOFSINTEN2			BIT(13)
#define   EN7581_LOFSINTEN2			BIT(12)
#define   EN7581_HINTEN2			BIT(11)
#define   EN7581_CINTEN2			BIT(10)
#define   EN7581_NOHOTINTEN1			BIT(9)
#define   EN7581_HOFSINTEN1			BIT(8)
#define   EN7581_LOFSINTEN1			BIT(7)
#define   EN7581_HINTEN1			BIT(6)
#define   EN7581_CINTEN1			BIT(5)
#define   EN7581_NOHOTINTEN0			BIT(4)
/* Similar to COLD and HOT also these seems to be swapped in documentation */
#define   EN7581_LOFSINTEN0			BIT(3) /* In documentation: BIT(2) */
#define   EN7581_HOFSINTEN0			BIT(2) /* In documentation: BIT(3) */
/* It seems documentation have these swapped as the HW
 * - Fire BIT(1) when lower than EN7581_COLD_THRE
 * - Fire BIT(0) and BIT(5) when higher than EN7581_HOT2NORMAL_THRE or
 *     EN7581_HOT_THRE
 */
#define   EN7581_CINTEN0			BIT(1) /* In documentation: BIT(0) */
#define   EN7581_HINTEN0			BIT(0) /* In documentation: BIT(1) */
#define EN7581_TEMPMONINTSTS			0x810
#define   EN7581_STAGE3_INT_STAT		BIT(31)
#define   EN7581_STAGE2_INT_STAT		BIT(30)
#define   EN7581_STAGE1_INT_STAT		BIT(29)
#define   EN7581_FILTER_INT_STAT_3		BIT(28)
#define   EN7581_IMMD_INT_STS3			BIT(27)
#define   EN7581_NOHOTINTSTS3			BIT(26)
#define   EN7581_HOFSINTSTS3			BIT(25)
#define   EN7581_LOFSINTSTS3			BIT(24)
#define   EN7581_HINTSTS3			BIT(23)
#define   EN7581_CINTSTS3			BIT(22)
#define   EN7581_FILTER_INT_STAT_2		BIT(21)
#define   EN7581_FILTER_INT_STAT_1		BIT(20)
#define   EN7581_FILTER_INT_STAT_0		BIT(19)
#define   EN7581_IMMD_INT_STS2			BIT(18)
#define   EN7581_IMMD_INT_STS1			BIT(17)
#define   EN7581_IMMD_INT_STS0			BIT(16)
#define   EN7581_TIME_OUT_INT_STAT		BIT(15)
#define   EN7581_NOHOTINTSTS2			BIT(14)
#define   EN7581_HOFSINTSTS2			BIT(13)
#define   EN7581_LOFSINTSTS2			BIT(12)
#define   EN7581_HINTSTS2			BIT(11)
#define   EN7581_CINTSTS2			BIT(10)
#define   EN7581_NOHOTINTSTS1			BIT(9)
#define   EN7581_HOFSINTSTS1			BIT(8)
#define   EN7581_LOFSINTSTS1			BIT(7)
#define   EN7581_HINTSTS1			BIT(6)
#define   EN7581_CINTSTS1			BIT(5)
#define   EN7581_NOHOTINTSTS0			BIT(4)
/* Similar to COLD and HOT also these seems to be swapped in documentation */
#define   EN7581_LOFSINTSTS0			BIT(3) /* In documentation: BIT(2) */
#define   EN7581_HOFSINTSTS0			BIT(2) /* In documentation: BIT(3) */
/* It seems documentation have these swapped as the HW
 * - Fire BIT(1) when lower than EN7581_COLD_THRE
 * - Fire BIT(0) and BIT(5) when higher than EN7581_HOT2NORMAL_THRE or
 *     EN7581_HOT_THRE
 *
 * To clear things, we swap the define but we keep them documented here.
 */
#define   EN7581_CINTSTS0			BIT(1) /* In documentation: BIT(0) */
#define   EN7581_HINTSTS0			BIT(0) /* In documentation: BIT(1)*/
/* Monitor will take the bigger threshold between HOT2NORMAL and HOT
 * and will fire both HOT2NORMAL and HOT interrupt when higher than the 2
 *
 * It has also been observed that not setting HOT2NORMAL makes the monitor
 * treat COLD threshold as HOT2NORMAL.
 */
#define EN7581_TEMPH2NTHRE			0x824
/* It seems HOT2NORMAL is actually NORMAL2HOT */
#define   EN7581_HOT2NORMAL_THRE		GENMASK(11, 0)
#define EN7581_TEMPHTHRE			0x828
#define   EN7581_HOT_THRE			GENMASK(11, 0)
/* Monitor will use this as HOT2NORMAL (fire interrupt when lower than...)*/
#define EN7581_TEMPCTHRE			0x82c
#define   EN7581_COLD_THRE			GENMASK(11, 0)
/* Also LOW and HIGH offset register are swapped */
#define EN7581_TEMPOFFSETL			0x830 /* In documentation: 0x834 */
#define   EN7581_LOW_OFFSET			GENMASK(11, 0)
#define EN7581_TEMPOFFSETH			0x834 /* In documentation: 0x830 */
#define   EN7581_HIGH_OFFSET			GENMASK(11, 0)
#define EN7581_TEMPMSRCTL0			0x838
#define   EN7581_MSRCTL3			GENMASK(11, 9)
#define   EN7581_MSRCTL2			GENMASK(8, 6)
#define   EN7581_MSRCTL1			GENMASK(5, 3)
#define   EN7581_MSRCTL0			GENMASK(2, 0)
#define EN7581_TEMPADCVALIDADDR			0x878
#define   EN7581_ADC_VALID_ADDR			GENMASK(31, 0)
#define EN7581_TEMPADCVOLTADDR			0x87c
#define   EN7581_ADC_VOLT_ADDR			GENMASK(31, 0)
#define EN7581_TEMPRDCTRL			0x880
/*
 * NOTICE: AHB have this set to 0 by default. Means that
 * the same addr is used for ADC volt and valid reading.
 * In such case, VALID ADDR is used and volt addr is ignored.
 */
#define   EN7581_RD_CTRL_DIFF			BIT(0)
#define EN7581_TEMPADCVALIDMASK			0x884
#define   EN7581_ADV_RD_VALID_POLARITY		BIT(5)
#define   EN7581_ADV_RD_VALID_POS		GENMASK(4, 0)
#define EN7581_TEMPADCVOLTAGESHIFT		0x888
#define   EN7581_ADC_VOLTAGE_SHIFT		GENMASK(4, 0)
/*
 * Same values for each CTL.
 * Can operate in:
 * - 1 sample
 * - 2 sample and make average of them
 * - 4,6,10,16 sample, drop max and min and make average of them
 */
#define   EN7581_MSRCTL_1SAMPLE			0x0
#define   EN7581_MSRCTL_AVG2SAMPLE		0x1
#define   EN7581_MSRCTL_4SAMPLE_MAX_MIX_AVG2	0x2
#define   EN7581_MSRCTL_6SAMPLE_MAX_MIX_AVG4	0x3
#define   EN7581_MSRCTL_10SAMPLE_MAX_MIX_AVG8	0x4
#define   EN7581_MSRCTL_18SAMPLE_MAX_MIX_AVG16	0x5
#define EN7581_TEMPAHBPOLL			0x840
#define   EN7581_ADC_POLL_INTVL			GENMASK(31, 0)
/* PTPSPARE0,2 reg are used to store efuse info for calibrated temp offset */
#define EN7581_EFUSE_TEMP_OFFSET_REG		0xf20 /* PTPSPARE0 */
#define   EN7581_EFUSE_TEMP_OFFSET		GENMASK(31, 16)
#define EN7581_PTPSPARE1			0xf24 /* PTPSPARE1 */
#define EN7581_EFUSE_TEMP_CPU_SENSOR_REG	0xf28 /* PTPSPARE2 */

#define EN7581_SLOPE_X100_DIO_DEFAULT		5645
#define EN7581_SLOPE_X100_DIO_AVS		5645
#define EN7523_SLOPE_E2				0x712
#define EN7523_SLOPE_E3				0x6d6

#define EN7581_INIT_TEMP_CPK_X10		300
#define EN7581_INIT_TEMP_FTK_X10		620
#define EN7581_INIT_TEMP_NONK_X10		550

#define EN7581_SCU_THERMAL_PROTECT_KEY		0x12
#define EN7581_SCU_THERMAL_MUX_DIODE1		0x7

#define AN7583_SCU_THERMAL_PROTECT_KEY		0x80
#define AN7583_NUM_SENSOR			3

#define AIROHA_THERMAL_NO_MUX_SENSOR		-1

/* Convert temp (in milliC) to raw value as read from ADC
 *   ((((temp / 100) - init) * slope) / 1000) + offset
 */
#define TEMP_TO_RAW(priv, temp)			((((((temp) / 100) - (priv)->init_temp) * \
						  (priv)->default_slope) / 1000) + \
						 (priv)->default_offset)

/* Convert raw to temp (in milliC, same as Linux RAW_TO_TEMP)
 *   ((((raw - offset) * 1000) / slope + init) * 100)
 */
#define RAW_TO_TEMP(priv, raw)			(((((raw) - (priv)->default_offset) * 1000) / \
						  (priv)->default_slope + \
						  (priv)->init_temp) * 100)

#define AIROHA_MAX_SAMPLES			6

/*
 * AN7583 supports all these ADC mux but the original driver
 * always checked temp with the AN7583_BGP_TEMP_SENSOR.
 * Assume using the other sensor temperature is invalid and
 * always read from AN7583_BGP_TEMP_SENSOR.
 *
 * On top of this it's defined that AN7583 supports 3
 * sensor: AN7583_BGP_TEMP_SENSOR, AN7583_GBE_TEMP_SENSOR,
 * AN7583_CPU_TEMP_SENSOR.
 *
 * Provide the ADC mux for reference.
 */
enum an7583_thermal_adc_mux {
	AN7583_BGP_TEMP_SENSOR,
	AN7583_PAD_AVS,
	AN7583_CORE_POWER,
	AN7583_AVSDAC_OUT,
	AN7583_VCM,
	AN7583_GBE_TEMP_SENSOR,
	AN7583_CPU_TEMP_SENSOR,

	AN7583_ADC_MUX_MAX,
};

enum an7583_thermal_diode_mux {
	AN7583_D0_TADC,
	AN7583_ZERO_TADC,
	AN7583_D1_TADC,
};

enum airoha_thermal_chip_scu_field {
	AIROHA_THERMAL_DOUT_TADC,
	AIROHA_THERMAL_MUX_SENSOR,
	AIROHA_THERMAL_MUX_TADC,

	/* keep last */
	AIROHA_THERMAL_FIELD_MAX,
};

/* Descriptor for a chip_scu register field (replaces Linux regmap_field) */
struct airoha_scu_field_desc {
	u32 reg;	/* register offset within chip_scu base */
	u8 lsb;		/* least significant bit position */
	u8 msb;		/* most significant bit position */
};

struct airoha_thermal_priv {
	void __iomem *base;		/* PTP thermal regs (our own regs) */
	void __iomem *chip_scu_base;	/* SCU regs from phandle / parent */
	void __iomem *np_scu_base;	/* EN7523 NP-SCU (chip id regs), 2nd reg range */
	struct airoha_scu_field_desc scu_fields[AIROHA_THERMAL_FIELD_MAX];
	phys_addr_t scu_adc_phys;	/* physical base of SCU (for ADC addr) */

	u32 pllrg_protect_key;
	u32 pllrg_protect;
	int current_adc;

	int init_temp;
	int default_slope;
	int default_offset;
};

struct airoha_thermal_soc_data {
	u32 pllrg_protect_key;
	u32 pllrg_protect;
	bool has_monitor;	/* has EN7581-style AHB monitor */
	bool mfd_child;		/* AN7583: chip_scu comes from parent node */

	int (*probe)(struct udevice *dev, struct airoha_thermal_priv *priv);
	int (*get_temp)(struct udevice *dev, int *temp);
};

static const unsigned int an7583_thermal_coeff[AN7583_ADC_MUX_MAX] = {
	[AN7583_BGP_TEMP_SENSOR] = 973,
	[AN7583_GBE_TEMP_SENSOR] = 995,
	[AN7583_CPU_TEMP_SENSOR] = 1035,
};

static const unsigned int an7583_thermal_slope[AN7583_ADC_MUX_MAX] = {
	[AN7583_BGP_TEMP_SENSOR] = 7440,
	[AN7583_GBE_TEMP_SENSOR] = 7620,
	[AN7583_CPU_TEMP_SENSOR] = 8390,
};

static const unsigned int an7583_thermal_offset[AN7583_ADC_MUX_MAX] = {
	[AN7583_BGP_TEMP_SENSOR] = 294,
	[AN7583_GBE_TEMP_SENSOR] = 298,
	[AN7583_CPU_TEMP_SENSOR] = 344,
};

/* ---------- MMIO / field helpers replacing regmap ---------- */

static inline u32 airoha_scu_read(struct airoha_thermal_priv *priv, u32 reg)
{
	return readl(priv->chip_scu_base + reg);
}

static inline void airoha_scu_write(struct airoha_thermal_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->chip_scu_base + reg);
}

static inline u32 airoha_base_read(struct airoha_thermal_priv *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static inline void airoha_base_write(struct airoha_thermal_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->base + reg);
}

static u32 airoha_field_read(struct airoha_thermal_priv *priv,
			     enum airoha_thermal_chip_scu_field id)
{
	const struct airoha_scu_field_desc *d = &priv->scu_fields[id];
	u32 val;
	u32 mask;

	val = airoha_scu_read(priv, d->reg);
	mask = GENMASK(d->msb, d->lsb);
	return (val & mask) >> d->lsb;
}

static void airoha_field_write(struct airoha_thermal_priv *priv,
			       enum airoha_thermal_chip_scu_field id,
			       u32 field_val)
{
	const struct airoha_scu_field_desc *d = &priv->scu_fields[id];
	u32 mask = GENMASK(d->msb, d->lsb);
	u32 val;

	val = airoha_scu_read(priv, d->reg);
	val &= ~mask;
	val |= (field_val << d->lsb) & mask;
	airoha_scu_write(priv, d->reg, val);
}

static void airoha_init_field(struct airoha_scu_field_desc *d,
			      u32 reg, u8 lsb, u8 msb)
{
	d->reg = reg;
	d->lsb = lsb;
	d->msb = msb;
}

/* ---------- Core HW access helpers ---------- */

static u32 get_pdid(struct airoha_thermal_priv *priv)
{
	u32 val;

	/* EN7523 keeps the product id (PDIDR) in the NP-SCU, the second
	 * register range of the merged scu node (0x1fb00000). Reading it
	 * from chip-SCU returns 0 and mis-selects the E3 calibration.
	 */
	if (priv->np_scu_base)
		val = readl(priv->np_scu_base + 0x5c);
	else
		val = airoha_scu_read(priv, 0x5c);

	return val & 0xffff;
}

static u32 get_pkg_type(struct airoha_thermal_priv *priv)
{
	u32 val;
	val = airoha_scu_read(priv, 0x254);
	return (val >> 14) & 0x3;
}

static u32 airoha_get_thermal_ADC(struct airoha_thermal_priv *priv)
{
	return airoha_field_read(priv, AIROHA_THERMAL_DOUT_TADC);
}

static void airoha_set_thermal_mux(struct airoha_thermal_priv *priv,
				   int tdac_idx, int sensor_idx)
{
	u32 pllrg;

	/* Save PLLRG current value */
	pllrg = airoha_scu_read(priv, priv->pllrg_protect);

	/* Give access to Thermal regs */
	airoha_scu_write(priv, priv->pllrg_protect, priv->pllrg_protect_key);

	/*
	 * Configure Thermal Sensor mux to sensor_idx.
	 * (if not supported, sensor_idx is AIROHA_THERMAL_NO_MUX_SENSOR)
	 */
	if (sensor_idx != AIROHA_THERMAL_NO_MUX_SENSOR)
		airoha_field_write(priv, AIROHA_THERMAL_MUX_SENSOR, sensor_idx);

	/* Configure Thermal ADC mux to tdac_idx */
	if (priv->current_adc != tdac_idx) {
		airoha_field_write(priv, AIROHA_THERMAL_MUX_TADC, tdac_idx);
		priv->current_adc = tdac_idx;
	}

	/* Sleep 10 ms for Thermal ADC to enable */
	mdelay(10);

	/* Restore PLLRG value on exit */
	airoha_scu_write(priv, priv->pllrg_protect, pllrg);
}

/* ---------- EN7581-style get_temp ---------- */

static int en7581_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	int min_value, max_value, avg_value, value;
	int i;

	avg_value = 0;
	min_value = INT_MAX;
	max_value = INT_MIN;

	for (i = 0; i < AIROHA_MAX_SAMPLES; i++) {
		value = airoha_get_thermal_ADC(priv);
		min_value = min(value, min_value);
		max_value = max(value, max_value);
		avg_value += value;
	}

	/* Drop min and max and average for the remaining sample */
	avg_value -= (min_value + max_value);
	avg_value /= AIROHA_MAX_SAMPLES - 2;

	/* RAW_TO_TEMP returns milliCelsius (same as Linux driver) */
	*temp = RAW_TO_TEMP(priv, avg_value);
	return 0;
}

static void en7581_thermal_setup_adc_val(struct udevice *dev,
					 struct airoha_thermal_priv *priv)
{
	u32 efuse_calib_info, cpu_sensor;

	/* Setup Thermal Sensor to ADC mode and setup the mux to DIODE1 */
	airoha_set_thermal_mux(priv, EN7581_SCU_THERMAL_MUX_DIODE1,
			       AIROHA_THERMAL_NO_MUX_SENSOR);

	efuse_calib_info = airoha_base_read(priv, EN7581_EFUSE_TEMP_OFFSET_REG);
	if (efuse_calib_info) {
		priv->default_offset = FIELD_GET(EN7581_EFUSE_TEMP_OFFSET, efuse_calib_info);
		/* Different slope are applied if the sensor is used for CPU or for package */
		cpu_sensor = airoha_base_read(priv, EN7581_EFUSE_TEMP_CPU_SENSOR_REG);
		if (cpu_sensor) {
			priv->default_slope = EN7581_SLOPE_X100_DIO_DEFAULT;
			priv->init_temp = EN7581_INIT_TEMP_FTK_X10;
		} else {
			priv->default_slope = EN7581_SLOPE_X100_DIO_AVS;
			priv->init_temp = EN7581_INIT_TEMP_CPK_X10;
		}
	} else {
		priv->default_offset = airoha_get_thermal_ADC(priv);
		priv->default_slope = EN7581_SLOPE_X100_DIO_DEFAULT;
		priv->init_temp = EN7581_INIT_TEMP_NONK_X10;
		dev_info(dev, "missing thermal calibration EFUSE, using non calibrated value\n");
	}
}

static void en7581_thermal_setup_monitor(struct airoha_thermal_priv *priv)
{
	/* Set measure mode */
	airoha_base_write(priv, EN7581_TEMPMSRCTL0,
		     FIELD_PREP(EN7581_MSRCTL0, EN7581_MSRCTL_6SAMPLE_MAX_MIX_AVG4));

	/*
	 * Configure ADC valid reading addr
	 * The AHB temp monitor system doesn't have direct access to the
	 * thermal sensor. It does instead work by providing various
	 * addresses to configure how to access and setup an ADC for the
	 * sensor. EN7581 supports only one sensor hence the
	 * implementation is greatly simplified but the AHB supports
	 * up to 4 different sensors from the same ADC that can be
	 * switched by tuning the ADC mux or writing address.
	 *
	 * We set valid instead of volt as we don't enable valid/volt
	 * split reading and AHB read valid addr in such case.
	 */
	airoha_base_write(priv, EN7581_TEMPADCVALIDADDR,
		     priv->scu_adc_phys + EN7581_DOUT_TADC);

	/*
	 * Configure valid bit on a fake value of bit 16. The ADC outputs
	 * max of 2 bytes for voltage.
	 */
	airoha_base_write(priv, EN7581_TEMPADCVALIDMASK,
		     FIELD_PREP(EN7581_ADV_RD_VALID_POS, 16));

	/*
	 * AHB supports max 12 bytes for ADC voltage. Shift the read
	 * value 4 bit to the right. Precision lost by this is minimal
	 * in the order of half a °C and is acceptable in the context
	 * of triggering interrupt in critical condition.
	 */
	airoha_base_write(priv, EN7581_TEMPADCVOLTAGESHIFT,
		     FIELD_PREP(EN7581_ADC_VOLTAGE_SHIFT, 4));

	/* BUS clock is 300MHz counting unit is 3 * 68.64 * 256 = 52.715us */
	airoha_base_write(priv, EN7581_TEMPMONCTL1,
		     FIELD_PREP(EN7581_PERIOD_UNIT, 3));

	/*
	 * filt interval is 1 * 52.715us = 52.715us,
	 * sen interval is 379 * 52.715us = 19.97ms
	 */
	airoha_base_write(priv, EN7581_TEMPMONCTL2,
		     FIELD_PREP(EN7581_FILT_INTERVAL, 1) |
		     FIELD_PREP(EN7581_SEN_INTERVAL, 379));

	/* AHB poll is set to 146 * 68.64 = 10.02us */
	airoha_base_write(priv, EN7581_TEMPAHBPOLL,
		     FIELD_PREP(EN7581_ADC_POLL_INTVL, 146));
}

/* EN7581 SCU field descriptors */
static const struct airoha_scu_field_desc en7581_chip_scu_fields_tmpl[AIROHA_THERMAL_FIELD_MAX] = {
	[AIROHA_THERMAL_DOUT_TADC]  = { EN7581_DOUT_TADC,  0, 15 },
	[AIROHA_THERMAL_MUX_SENSOR] = { 0, 0, 0 },	/* not supported */
	[AIROHA_THERMAL_MUX_TADC]   = { EN7581_PWD_TADC,  1,  3 },
};

/* ---------- AN7583 get_temp ---------- */

static int an7583_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	int sensor_idx;
	int delta_diode, delta_gain;
	int coeff, slope, offset;
	int diode_zero, diode_d0, diode_d1;
	int temp_centi;

	/* Always read sensor AN7583_BGP_TEMP_SENSOR */
	sensor_idx = AN7583_BGP_TEMP_SENSOR;

	coeff = an7583_thermal_coeff[sensor_idx];
	slope = an7583_thermal_slope[sensor_idx];
	offset = an7583_thermal_offset[sensor_idx];

	airoha_set_thermal_mux(priv, sensor_idx, AN7583_ZERO_TADC);
	diode_zero = airoha_get_thermal_ADC(priv);
	airoha_set_thermal_mux(priv, sensor_idx, AN7583_D0_TADC);
	diode_d0 = airoha_get_thermal_ADC(priv);
	airoha_set_thermal_mux(priv, sensor_idx, AN7583_D1_TADC);
	diode_d1 = airoha_get_thermal_ADC(priv);

	delta_diode = diode_d1 - diode_d0;
	delta_gain = (delta_diode * coeff) / 100 + (diode_zero - diode_d1);
	temp_centi = (slope * delta_diode * 10) / delta_gain - offset * 10;
	temp_centi *= 100;

	/* temp_centi is already in milliCelsius (matches Linux driver) */
	*temp = temp_centi;
	return 0;
}

/* AN7583 SCU field descriptors */
static const struct airoha_scu_field_desc an7583_chip_scu_fields_tmpl[AIROHA_THERMAL_FIELD_MAX] = {
	[AIROHA_THERMAL_DOUT_TADC]  = { AN7583_DOUT_TADC,  0, 31 },
	[AIROHA_THERMAL_MUX_TADC]   = { AN7583_MUX_TADC,   1,  3 },
	[AIROHA_THERMAL_MUX_SENSOR] = { AN7583_MUX_SENSOR, 2,  3 },
};

/* EN7523 SCU field descriptors */
static const struct airoha_scu_field_desc en7523_chip_scu_fields_tmpl[AIROHA_THERMAL_FIELD_MAX] = {
	[AIROHA_THERMAL_DOUT_TADC]  = { EN7523_DOUT_TADC,  0, 15 },
	[AIROHA_THERMAL_MUX_SENSOR] = { 0, 0, 0 },	/* not supported */
	[AIROHA_THERMAL_MUX_TADC]   = { EN7523_MUX_TADC,  1,  3 },
};

/* ---------- EN7523 get_temp ---------- */

static int en7523_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct airoha_thermal_priv *priv = dev_get_priv(dev);
	int adc_val, temp_x10;

	adc_val = airoha_get_thermal_ADC(priv);
	temp_x10 = 1000 * (adc_val - priv->default_offset) / priv->default_slope + 300;

	/* temp_x10 is already in milliCelsius (matches Linux driver) */
	*temp = temp_x10 * 100;
	return 0;
}

/* ---------- Per-SoC probe functions ---------- */

static int airoha_get_chip_scu_from_phandle(struct udevice *dev,
					    struct airoha_thermal_priv *priv)
{
	struct ofnode_phandle_args args;
	ofnode scu_node;
	fdt_addr_t addr;
	int ret;

	ret = dev_read_phandle_with_args(dev, "airoha,chip-scu", NULL, 0, 0, &args);
	if (ret) {
		dev_err(dev, "missing airoha,chip-scu phandle: %d\n", ret);
		return ret;
	}
	scu_node = args.node;

	addr = ofnode_get_addr(scu_node);
	if (addr == FDT_ADDR_T_NONE) {
		dev_err(dev, "failed to get chip-scu base address\n");
		return -EINVAL;
	}

	priv->chip_scu_base = (void __iomem *)(uintptr_t)addr;
	priv->scu_adc_phys = (phys_addr_t)addr;

	/* EN7523: the scu node merges chip-SCU (reg 0) and NP-SCU (reg 1).
	 * Chip identification regs (PDIDR at 0x5c) live in the NP-SCU.
	 * Platforms with a dedicated chip_scu node have no second range
	 * and ofnode_get_addr_index() returns FDT_ADDR_T_NONE.
	 */
	addr = ofnode_get_addr_index(scu_node, 1);
	if (addr != FDT_ADDR_T_NONE)
		priv->np_scu_base = (void __iomem *)(uintptr_t)addr;

	return 0;
}

static int airoha_get_chip_scu_from_parent(struct udevice *dev,
					    struct airoha_thermal_priv *priv)
{
	struct udevice *parent = dev_get_parent(dev);
	fdt_addr_t addr;

	if (!parent) {
		dev_err(dev, "AN7583 requires a parent MFD node\n");
		return -EINVAL;
	}

	addr = dev_read_addr(parent);
	if (addr == FDT_ADDR_T_NONE) {
		dev_err(dev, "failed to get parent (SCU) base address\n");
		return -EINVAL;
	}

	priv->chip_scu_base = (void __iomem *)(uintptr_t)addr;
	priv->scu_adc_phys = (phys_addr_t)addr;

	return 0;
}

static void airoha_install_scu_fields(struct airoha_thermal_priv *priv,
				      const struct airoha_scu_field_desc *tmpl,
				      bool has_mux_sensor)
{
	int i;

	for (i = 0; i < AIROHA_THERMAL_FIELD_MAX; i++) {
		if (i == AIROHA_THERMAL_MUX_SENSOR && !has_mux_sensor)
			continue;
		airoha_init_field(&priv->scu_fields[i],
				  tmpl[i].reg, tmpl[i].lsb, tmpl[i].msb);
	}
}

static int en7581_thermal_probe(struct udevice *dev,
				struct airoha_thermal_priv *priv)
{
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = airoha_get_chip_scu_from_phandle(dev, priv);
	if (ret)
		return ret;

	airoha_install_scu_fields(priv, en7581_chip_scu_fields_tmpl, false);

	/*
	 * Interrupt handling is intentionally omitted in the U-Boot port.
	 * Thermal readings are done by polling.
	 */

	en7581_thermal_setup_monitor(priv);
	en7581_thermal_setup_adc_val(dev, priv);

	return 0;
}

static int an7583_thermal_probe(struct udevice *dev,
				struct airoha_thermal_priv *priv)
{
	int ret;

	ret = airoha_get_chip_scu_from_parent(dev, priv);
	if (ret)
		return ret;

	airoha_install_scu_fields(priv, an7583_chip_scu_fields_tmpl, true);

	/* AN7583 has no dedicated "base" PTP thermal region */
	priv->base = priv->chip_scu_base;

	return 0;
}

static int en7523_thermal_probe(struct udevice *dev,
				struct airoha_thermal_priv *priv)
{
	u32 pdid, pkg, efuse_val, val;
	int ret;

	/* EN7523 reuses EN7581 setup for base + phandle SCU mapping */
	ret = en7581_thermal_probe(dev, priv);
	if (ret)
		return ret;

	/* Now override SCU field descriptors with EN7523-specific ones */
	airoha_install_scu_fields(priv, en7523_chip_scu_fields_tmpl, false);

	/* Unlock PLLRG for writes */
	airoha_scu_write(priv, priv->pllrg_protect, priv->pllrg_protect_key);

	/* Sequenced TADC mux toggling (from original Linux driver) */
	val = airoha_scu_read(priv, EN7523_MUX_TADC);
	val &= ~((u32)0x1 << 7);
	val |= 0x0 << 7;
	airoha_scu_write(priv, EN7523_MUX_TADC, val);

	val = airoha_scu_read(priv, EN7523_MUX_TADC);
	val &= ~((u32)0x1 << 7);
	val |= 0x1 << 7;
	airoha_scu_write(priv, EN7523_MUX_TADC, val);

	val = airoha_scu_read(priv, EN7523_MUX_TADC);
	val &= ~((u32)0x7f << 0);
	val |= 0x70 << 0;
	airoha_scu_write(priv, EN7523_MUX_TADC, val);

	/* Package + calibration selection */
	pdid = get_pdid(priv);
	pkg = get_pkg_type(priv);
	/* Temperature calibration field is in bits [31:16] of the PTP
	 * spare register; adding the whole 32-bit value would corrupt the
	 * offset (the low 16 bits hold unrelated factory data).
	 */
	efuse_val = FIELD_GET(EN7581_EFUSE_TEMP_OFFSET,
			      airoha_base_read(priv, EN7581_EFUSE_TEMP_OFFSET_REG));

	if (pdid == 2) {
		priv->default_slope = EN7523_SLOPE_E2;
		priv->default_offset = (pkg == EN7523_PKG_BGA) ?
					EN7523_BIAS_E2BGA : EN7523_BIAS_E2QFP;

		if (efuse_val)
			priv->default_offset += efuse_val;
		else
			priv->default_offset += EN7523_CODE_30_DEFAULT_E2;
	} else {
		priv->default_slope = EN7523_SLOPE_E3;
		priv->default_offset = EN7523_BIAS_E3BGA;

		if (efuse_val)
			priv->default_offset += efuse_val;
		else
			priv->default_offset += EN7523_CODE_30_DEFAULT_E3;
	}

	return 0;
}

/* ---------- Common DM probe / ops ---------- */

static int airoha_thermal_probe(struct udevice *dev)
{
	const struct airoha_thermal_soc_data *soc_data;
	struct airoha_thermal_priv *priv;
	int ret;

	soc_data = (const struct airoha_thermal_soc_data *)dev_get_driver_data(dev);
	if (!soc_data)
		return -EINVAL;

	priv = dev_get_priv(dev);

	priv->pllrg_protect_key = soc_data->pllrg_protect_key;
	priv->pllrg_protect = soc_data->pllrg_protect;
	priv->current_adc = -1;

	if (!soc_data->probe)
		return -EINVAL;

	ret = soc_data->probe(dev, priv);
	if (ret)
		return ret;

	return 0;
}

/* ---------- SoC data tables ---------- */

static const struct airoha_thermal_soc_data en7581_data = {
	.pllrg_protect_key = EN7581_SCU_THERMAL_PROTECT_KEY,
	.pllrg_protect = EN7581_PLLRG_PROTECT,
	.has_monitor = true,
	.mfd_child = false,
	.probe = &en7581_thermal_probe,
	.get_temp = &en7581_thermal_get_temp,
};

static const struct airoha_thermal_soc_data an7583_data = {
	.pllrg_protect_key = AN7583_SCU_THERMAL_PROTECT_KEY,
	.pllrg_protect = EN7581_PLLRG_PROTECT,
	.has_monitor = false,
	.mfd_child = true,
	.probe = &an7583_thermal_probe,
	.get_temp = &an7583_thermal_get_temp,
};

static const struct airoha_thermal_soc_data en7523_data = {
	.pllrg_protect_key = AN7583_SCU_THERMAL_PROTECT_KEY,
	.pllrg_protect = EN7523_PLLRG_PROTECT,
	.has_monitor = true,
	.mfd_child = false,
	.probe = &en7523_thermal_probe,
	.get_temp = &en7523_thermal_get_temp,
};

static int airoha_thermal_wrapper_get_temp(struct udevice *dev, int *temp)
{
	const struct airoha_thermal_soc_data *soc_data =
		(const struct airoha_thermal_soc_data *)dev_get_driver_data(dev);

	if (!soc_data || !soc_data->get_temp)
		return -ENOSYS;

	return soc_data->get_temp(dev, temp);
}

static const struct dm_thermal_ops airoha_thermal_ops = {
	.get_temp = airoha_thermal_wrapper_get_temp,
};

static const struct udevice_id airoha_thermal_ids[] = {
	{
		.compatible = "airoha,en7581-thermal",
		.data = (ulong)&en7581_data,
	},
	{
		.compatible = "airoha,an7583-thermal",
		.data = (ulong)&an7583_data,
	},
	{
		.compatible = "airoha,en7523-thermal",
		.data = (ulong)&en7523_data,
	},
	{ /* sentinel */ }
};

U_BOOT_DRIVER(airoha_thermal) = {
	.name		= "airoha-thermal",
	.id		= UCLASS_THERMAL,
	.of_match	= airoha_thermal_ids,
	.ops		= &airoha_thermal_ops,
	.probe		= airoha_thermal_probe,
	.priv_auto	= sizeof(struct airoha_thermal_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};
