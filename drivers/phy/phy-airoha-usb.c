// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 AIROHA Inc
 * Airoha USB PHY driver
 *
 * Based on the Linux phy-airoha-usb.c but majorly reworked for U-Boot DM phy framework.
 *
 * Author: Christian Marangi <ansuelsmth@gmail.com>(original driver)
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Ported from the mainline Airoha USB PHY drivers (AN7581: phy-airoha-usb.c,
 * AN7583: phy-airoha-an7583-usb.c) together with the vendor mu3_phy drivers
 * (mtk-phy.c for EN7523, mtk-phy_7552.c for AN7552/AN7563). The controller
 * side is handled by the generic MTK xHCI driver (mediatek,mtk-xhci).
 *
 * Only what a u-boot needs is implemented: U2 setup, slew rate
 * calibration and power sequencing, the U3 power sequencing and per SoC PLL
 * settings, plus the SCU serdes selection. The ethernet (SGMII/HSGMII) modes
 * and the full U3 KBand calibration of the vendor drivers are not ported.
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dt-bindings/phy/phy.h>
#include <dm/read.h>
#include <generic-phy.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <asm/io.h>
#include <asm/types.h>

/* U2 registers, relative to the PHY block base + instance offset */
#define U2_FMCR0		0x100
#define U2_FMMONR0		0x10c
#define U2_FMMONR1		0x110
#define U2_USBPHYACR2		0x308
#define U2_USBPHYACR4		0x310
#define U2_USBPHYACR5		0x314
#define U2_USBPHYACR6		0x318
#define U2_U2PHYACR3		0x31c
#define U2_U2PHYDTM0		0x368

/* U3 registers, relative to the PHY block base */
#define U3_GPIO_CTLD		0x80c
#define U3_PHYA_REG0		0xb00
#define U3_PHYA_REG1		0xb04
#define U3_PHYA_REG6		0xb18
#define U3_PHYA_REG8		0xb20
#define U3_PHYA_DA_REG19	0xc38

#define AIROHA_U2_PORT_OFFSET	0x1000

/* U2 bit fields */
#define U2_MONCLK_SEL		GENMASK(27, 26)
#define U2_FREQDET_EN		BIT(24)
#define U2_CYCLECNT		GENMASK(23, 0)
#define U2_FRCK_EN		BIT(8)
#define U2_SIFSLV_MAC_BANDGAP_EN BIT(17)
#define U2_FS_CR		GENMASK(10, 8)
#define U2_FS_SR		GENMASK(2, 0)
#define U2_HSTX_SRCAL_EN	BIT(15)
#define U2_HSTX_SRCTRL		GENMASK(14, 12)
#define U2_DISC_FIT_EN		BIT(28)
#define U2_BC11_SW_EN		BIT(23)
#define U2_HSRX_BIAS_EN_SEL	GENMASK(10, 9)
#define U2_HSRX_BIAS_EN_SEL_NO_ENPLL_FS_BIAS_EN \
				FIELD_PREP(U2_HSRX_BIAS_EN_SEL, 0x2)
#define U2_DISCTH		GENMASK(7, 4)
#define U2_SQTH			GENMASK(3, 0)
#define U2_HSTX_I_EN_MODE	GENMASK(25, 24)
#define U2_HSTX_I_EN_MODE_TOGGLE FIELD_PREP(U2_HSTX_I_EN_MODE, 0x0)
#define U2_HSTX_I_EN_MODE_FORCE_DISABLE FIELD_PREP(U2_HSTX_I_EN_MODE, 0x2)
#define U2_USB11_TMODE_EN	BIT(19)
#define U2_TMODE_FS_LS_TX_EN	BIT(18)
#define U2_PUPD_BIST_EN		BIT(12)
#define U2_FORCE_XCVRSEL	BIT(19)
#define U2_FORCE_SUSPENDDM	BIT(18)
#define U2_FORCE_TERMSEL	BIT(17)
#define U2_XCVRSEL		GENMASK(5, 4)
#define U2_TERMSEL		BIT(2)

/* U3 bit fields */
#define U3_SSUSB_IP_SW_RST	BIT(31)
#define U3_MCU_BUS_CK_GATE_EN	BIT(30)
#define U3_FORCE_SSUSB_IP_SW_RST BIT(29)
#define U3_SSUSB_SW_RST		BIT(28)
#define U3_SSUSB_BG_DIV		GENMASK(29, 28)
#define U3_SSUSB_BG_DIV_4	FIELD_PREP(U3_SSUSB_BG_DIV, 0x1)
#define U3_SSUSB_XTAL_TOP_RESERVE GENMASK(25, 10)
#define U3_SSUSB_CDR_RESERVE	GENMASK(31, 24)
#define U3_SSUSB_CDR_RST_DLY	GENMASK(7, 6)
#define U3_SSUSB_PLL_SSC_DELTA1_U3 GENMASK(15, 0)

/* U3 blocks of the AN7583 (separate register regions) */
#define U3_ANA_TDC_FT_CK_EN	0x38
#define U3_ANA_VUSB10_ON	BIT(8)
#define U3_PMA_INTF_CTRL_5	0x314
#define U3_PMA_RX_HZ_SEL	BIT(5)
#define U3_PMA_RX_HZ_FORCE	BIT(4)

/* SCU serdes selection, relative to the SCU base */
#define AIROHA_SCU_SSR3		0x94
#define AIROHA_SCU_SSR3_HSGMII_SEL BIT(29)	/* 0 = HSGMII, 1 = USB */
#define AIROHA_SCU_SSTR		0x9c
#define AIROHA_SCU_SSTR_USB_PCIE_SEL BIT(3)	/* 0 = PCIe, 1 = USB */

/* EN7523 U3 PLL registers, written by u3phy_config_en7523() */
#define U3_PHYA_REG3		0xb0c
#define U3_PHYA_REG4		0xb10
#define U3_PHYA_REG5		0xb14
#define U3_PHYA_REG9		0xb38
#define U3_PHYA_REG10		0xb40

/* Chip-SCU crystal selection, same layout on the ARM SoCs */
#define AIROHA_CHIP_SCU_XTAL_SEL	0x254
#define AIROHA_CHIP_SCU_XTAL_SEL_20M	BIT(19)	/* 0 = 20 MHz, 1 = 40 MHz */

/* Slew rate calibration */
#define U2_FM_DET_CYCLE_CNT	1024
#define U2_REF_CK		20
#define U2_SR_COEF		28
#define U2_SR_COEF_DIVISOR	1000
#define U2_SR_CAL_DEFAULT	0x5
#define U2_FREQDET_TIMEOUT	10000	/* us */

#define AIROHA_USB_PHY_MAX_INSTANCE 3

/** struct airoha_usb_phy_instance - one USB port of a PHY block */
struct airoha_usb_phy_instance {
	unsigned int type;		/* PHY_TYPE_USB2 / PHY_TYPE_USB3 */
	unsigned int offset;		/* U2 register offset in the PHY block */
	unsigned int monclk_sel;	/* frequency meter monitor clock */
	void __iomem *scu;		/* SCU of this port, may be NULL */
	bool disabled;			/* serdes unknown: leave the PHY alone */
};

/** struct airoha_usb_phy_data - per SoC differences */
struct airoha_usb_phy_data {
	const char *name;
	/* U2 tuning */
	unsigned int fs_cr;		/* USB20_FS_CR field value */
	unsigned int fs_sr;		/* 0xff = leave untouched */
	unsigned int sqth;
	unsigned int discth;
	unsigned int monclk_base;	/* monitor clock of the first port  */
	bool u2_calibrate;		/* run the slew rate calibration */
	bool u2_full_power;		/* full U2 power sequence (AN7583) */
	/* U3 */
	bool has_u3;
	bool u3_pll_20m;		/* EN7523 U3 style: PLL + crystal */
	bool u3_full_init;		/* AN7583 U3 style: ana/pma/dig + KBand */
	unsigned int u3_cdr_reserve;	/* AN7581 style CDR reserve value */
	unsigned int u3_rst_mask;	/* GPIO_CTLD bits cleared on power on (0 = none) */
	bool u3_ana_pma;		/* separate ANA/PMA regions (AN7583) */
	/* SCU serdes selection */
	unsigned int scu_reg;		/* 0 = none */
	unsigned int scu_bit;
};

/** struct airoha_usb_phy_priv - driver private data */
struct airoha_usb_phy_priv {
	struct udevice *dev;
	void __iomem *phy;
	void __iomem *ana;
	void __iomem *pma;
	void __iomem *dig;
	void __iomem *scu;		/* default SCU of the PHY node */
	void __iomem *chip_scu;		/* crystal / package strap registers */
	const struct airoha_usb_phy_data *data;
	unsigned int serdes_port;	/* airoha,serdes-port value */
	bool has_serdes_port;
	unsigned int num_instances;
	struct airoha_usb_phy_instance instances[AIROHA_USB_PHY_MAX_INSTANCE];
};

/**
 * airoha_usb_phy_get_syscon() - resolve a syscon phandle of a node
 * @node: node carrying the property
 * @prop: phandle property name (e.g. "airoha,scu")
 *
 * Return: mapped syscon base, or NULL when the property is missing/invalid.
 */
static void __iomem *airoha_usb_phy_get_syscon(ofnode node, const char *prop)
{
	struct ofnode_phandle_args args;
	fdt_addr_t addr;

	if (ofnode_parse_phandle_with_args(node, prop, NULL, 0, 0, &args))
		return NULL;

	addr = ofnode_get_addr(args.node);

	return addr == FDT_ADDR_T_NONE ? NULL : (void __iomem *)addr;
}

static void airoha_usb_phy_u2_slew_rate_calibration(struct airoha_usb_phy_priv *priv,
						    struct airoha_usb_phy_instance *inst)
{
	void __iomem *phy = priv->phy + inst->offset;
	u32 fm_out = 0, srctrl;

	/* Enable HS TX SR calibration */
	setbits_le32(phy + U2_USBPHYACR5, U2_HSTX_SRCAL_EN);
	udelay(1000);

	/* Enable free run clock and select the monitor clock */
	setbits_le32(phy + U2_FMMONR1, U2_FRCK_EN);
	clrsetbits_le32(phy + U2_FMCR0, U2_MONCLK_SEL,
			FIELD_PREP(U2_MONCLK_SEL, inst->monclk_sel));

	/* Set cycle count and enable the frequency meter */
	clrsetbits_le32(phy + U2_FMCR0, U2_CYCLECNT,
			FIELD_PREP(U2_CYCLECNT, U2_FM_DET_CYCLE_CNT));
	setbits_le32(phy + U2_FMCR0, U2_FREQDET_EN);

	/* A timeout is expected and handled by the workaround below */
	readl_poll_timeout(phy + U2_FMMONR0, fm_out, fm_out, U2_FREQDET_TIMEOUT);

	clrbits_le32(phy + U2_FMCR0, U2_FREQDET_EN);
	clrbits_le32(phy + U2_FMMONR1, U2_FRCK_EN);
	clrbits_le32(phy + U2_USBPHYACR5, U2_HSTX_SRCAL_EN);
	udelay(1000);

	/* (1024 / FM_OUT) * REF_CK * U2_SR_COEF, rounded to the nearest digit */
	if (!fm_out) {
		srctrl = U2_SR_CAL_DEFAULT;
		dev_warn(priv->dev, "port %u: frequency not detected\n",
			 inst->offset / AIROHA_U2_PORT_OFFSET);
	} else {
		srctrl = U2_REF_CK * U2_SR_COEF * U2_FM_DET_CYCLE_CNT / fm_out;
		srctrl = DIV_ROUND_CLOSEST(srctrl, U2_SR_COEF_DIVISOR);
	}

	clrsetbits_le32(phy + U2_USBPHYACR5, U2_HSTX_SRCTRL,
			FIELD_PREP(U2_HSTX_SRCTRL, srctrl));
}

static void airoha_usb_phy_u2_init(struct airoha_usb_phy_priv *priv,
				   struct airoha_usb_phy_instance *inst)
{
	const struct airoha_usb_phy_data *data = priv->data;
	void __iomem *phy = priv->phy + inst->offset;

	mdelay(1);

	clrsetbits_le32(phy + U2_USBPHYACR4, U2_FS_CR,
			FIELD_PREP(U2_FS_CR, data->fs_cr));

	if (data->fs_sr != 0xff)
		clrsetbits_le32(phy + U2_USBPHYACR4, U2_FS_SR,
				FIELD_PREP(U2_FS_SR, data->fs_sr));

	if (data->sqth != 0xff)
		clrsetbits_le32(phy + U2_USBPHYACR6, U2_SQTH,
				FIELD_PREP(U2_SQTH, data->sqth));

	if (data->discth != 0xff)
		clrsetbits_le32(phy + U2_USBPHYACR6, U2_DISCTH,
				FIELD_PREP(U2_DISCTH, data->discth));

	/* Enable the USB port and disable it again after calibration */
	clrbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);

	if (data->u2_calibrate)
		airoha_usb_phy_u2_slew_rate_calibration(priv, inst);

	setbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);
	udelay(1000);
}

static void airoha_usb_phy_u2_power_on(struct airoha_usb_phy_priv *priv,
				       struct airoha_usb_phy_instance *inst)
{
	void __iomem *phy = priv->phy + inst->offset;

	if (!priv->data->u2_full_power) {
		/* EN7523/AN7563/AN7581: the port is enabled by BC11 only */
		clrbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);
		udelay(1000);
		return;
	}

	/* AN7583 */
	clrbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);
	setbits_le32(phy + U2_USBPHYACR2, U2_SIFSLV_MAC_BANDGAP_EN);
	clrsetbits_le32(phy + U2_U2PHYACR3, U2_HSTX_I_EN_MODE,
			U2_HSTX_I_EN_MODE_TOGGLE);
	clrbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_XCVRSEL | U2_XCVRSEL);
	clrsetbits_le32(phy + U2_USBPHYACR6, U2_HSRX_BIAS_EN_SEL,
			U2_HSRX_BIAS_EN_SEL_NO_ENPLL_FS_BIAS_EN);
	setbits_le32(phy + U2_USBPHYACR5, U2_DISC_FIT_EN);
	clrbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_SUSPENDDM);
	clrbits_le32(phy + U2_U2PHYACR3, U2_USB11_TMODE_EN);
	setbits_le32(phy + U2_U2PHYACR3, U2_TMODE_FS_LS_TX_EN);
	clrbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_TERMSEL | U2_TERMSEL);
	clrbits_le32(phy + U2_U2PHYACR3, U2_PUPD_BIST_EN);
}

static void airoha_usb_phy_u2_power_off(struct airoha_usb_phy_priv *priv,
					struct airoha_usb_phy_instance *inst)
{
	void __iomem *phy = priv->phy + inst->offset;

	if (!priv->data->u2_full_power) {
		setbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);
		udelay(1000);
		return;
	}

	/* AN7583 */
	setbits_le32(phy + U2_U2PHYACR3, U2_PUPD_BIST_EN);
	setbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_TERMSEL | U2_TERMSEL);
	clrbits_le32(phy + U2_U2PHYACR3, U2_TMODE_FS_LS_TX_EN);
	setbits_le32(phy + U2_U2PHYACR3, U2_USB11_TMODE_EN);
	setbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_SUSPENDDM);
	clrbits_le32(phy + U2_USBPHYACR5, U2_DISC_FIT_EN);
	clrsetbits_le32(phy + U2_USBPHYACR6, U2_HSRX_BIAS_EN_SEL,
			U2_HSRX_BIAS_EN_SEL);
	clrsetbits_le32(phy + U2_U2PHYDTM0, U2_FORCE_XCVRSEL | U2_XCVRSEL,
			U2_FORCE_XCVRSEL | FIELD_PREP(U2_XCVRSEL, 0x1));
	clrsetbits_le32(phy + U2_U2PHYACR3, U2_HSTX_I_EN_MODE,
			U2_HSTX_I_EN_MODE_FORCE_DISABLE);
	clrbits_le32(phy + U2_USBPHYACR2, U2_SIFSLV_MAC_BANDGAP_EN);
	setbits_le32(phy + U2_USBPHYACR6, U2_BC11_SW_EN);
}

/*
 * AN7583 SuperSpeed (U3) PHY
 *
 * Unlike the AN7581, whose SSUSB PHY is programmed through a handful of
 * registers in the PHY block itself, the AN7583 SSUSB PHY lives in three
 * separate blocks ("ana", "pma", "dig") and only comes up after a long
 * register sequence plus a KBand calibration: the calibration is what makes
 * the PLL lock, and only a locked PLL lets the SuperSpeed MAC out of reset -
 * the controller waits for that in IPPC STS1.U3_MAC_RST.
 * Ported from the mainline phy-airoha-an7583-usb.c driver
 * (an7583_usb_phy_u3_init(), an7583_usb_phy_u3_kband_is_calibrated(),
 * an7583_usb_phy_kband_calibrate() and an7583_usb_phy_tx_pll_enable()).
 */
enum airoha_u3_block {
	U3_BLK_ANA,
	U3_BLK_PMA,
	U3_BLK_DIG,
};

/* "ana" block registers */
#define U3_ANA_RXAFE_RESERVE		0x004
#define U3_ANA_CDR_LPF_MJV_LIM		0x00c
#define U3_ANA_CDR_PR_CKREF_DIV1	0x018
#define U3_ANA_CDR_PR_KBAND_DIV_PCIE_REG 0x01c
#define U3_ANA_CDR_FORCE_IBANDLPF_R_OFF	0x020
#define U3_ANA_QP_TX_MODE_16B_EN	0x028
#define U3_ANA_RXLBTX_EN		0x02c
#define U3_ANA_SSUSB_BGR_EN		0x030
#define U3_ANA_PLL_IPLL_DIG_PWR_SEL	0x03c
#define U3_ANA_PLL_SDM_ORD		0x040

/* "pma" block registers */
#define U3_PMA_TX_DA_CTRL_0		0x000
#define U3_PMA_TX_DA_CTRL_3		0x00c
#define U3_PMA_PON_RXFEDIG_CTRL_0	0x100
#define U3_PMA_SS_LCPLL_PWCTL_SETTING_2	0x208
#define U3_PMA_SS_LCPLL_TDC_FLT_2	0x230
#define U3_PMA_SS_LCPLL_TDC_PCW_1	0x248
#define U3_PMA_INTF_CTRL_6		0x318
#define U3_PMA_INTF_CTRL_7		0x31c
#define U3_PMA_INTF_CTRL_8		0x320
#define U3_PMA_INTF_STS_9		0x364
#define U3_PMA_PLL_CTRL_0		0x400
#define U3_PMA_PLL_CTRL_1		0x404
#define U3_PMA_PLL_CTRL_2		0x408
#define U3_PMA_PLL_CTRL_3		0x40c
#define U3_PMA_PLL_CTRL_4		0x410
#define U3_PMA_PLL_CK_CTRL_1		0x418
#define U3_PMA_PLL_CK_CTRL_2		0x41c
#define U3_PMA_RX_SYS_CTRL_0		0x600
#define U3_PMA_RX_DLY_0			0x614
#define U3_PMA_RX_CTRL_2		0x630
#define U3_PMA_RX_CTRL_5		0x63c
#define U3_PMA_RX_CTRL_6		0x640
#define U3_PMA_RX_CTRL_7		0x644
#define U3_PMA_RX_CTRL_11		0x654
#define U3_PMA_RX_CTRL_36		0x6b8
#define U3_PMA_RX_CTRL_38		0x6c0
#define U3_PMA_RX_CTRL_45		0x6dc
#define U3_PMA_RX_CTRL_46		0x6e0
#define U3_PMA_RX_CTRL_49		0x6ec
#define U3_PMA_RX_CTRL_50		0x6f0

/* "dig" block registers */
#define U3_DIG_CK_RST_CTRL_3		0x30c
#define U3_DIG_CK_RST_CTRL_7		0x340

/* ana fields */
#define U3_ANA_BIAS_V2V_CAL		GENMASK(26, 21)
#define U3_ANA_CDR_LPF_RATIO		GENMASK(5, 4)
#define U3_ANA_CDR_PD_10B_EN		BIT(11)
#define U3_ANA_CDR_PD_EDGE_DIS		BIT(10)
#define U3_ANA_CDR_PHYCK_RSTB		BIT(13)
#define U3_ANA_CDR_PR_DAC_BAND		GENMASK(12, 8)
#define U3_ANA_CDR_PR_KBAND_DIV		GENMASK(26, 24)
#define U3_ANA_CDR_PR_KBAND_DIV_PCIE	GENMASK(5, 0)
#define U3_ANA_CDR_PR_KBAND_PCIE_MODE	BIT(6)
#define U3_ANA_CDR_PR_XFICK_EN		BIT(30)
#define U3_ANA_PLL_DEBUG_SEL		BIT(19)
#define U3_ANA_PLL_LDOLPF_VSEL		GENMASK(6, 5)
#define U3_ANA_PLL_MONVC_EN		BIT(14)
#define U3_ANA_PLL_MON_LDO_SEL		GENMASK(18, 16)
#define U3_ANA_PLL_OSCAL_ENB		BIT(19)
#define U3_ANA_PLL_PREDIV		GENMASK(26, 25)
#define U3_ANA_PLL_SSC_PHASE_INI	BIT(3)
#define U3_ANA_PLL_SSC_TRI_EN		BIT(4)
#define U3_ANA_SSUSB_BG_DIV		GENMASK(3, 2)
#define U3_ANA_SSUSB_CHPEN		BIT(1)
#define U3_ANA_TX_DMEDGEGEN_EN		BIT(27)
#define U3_ANA_TX_RXDET_METHOD		BIT(25)
#define U3_ANA_TX_RESERVE_8		BIT(8)

/* pma fields */
#define U3_PMA_ADDR_INTF_STS_PLL_VCOCAL	GENMASK(23, 16)
#define U3_PMA_AUTO_INIT		BIT(0)
#define U3_PMA_EQ_EN_DLY		GENMASK(12, 0)
#define U3_PMA_EQ_EN_DLY_SHORT		GENMASK(25, 13)
#define U3_PMA_EQ_RX500M_CK_SEL		BIT(12)
#define U3_PMA_FORCE_SIGDET_5G		BIT(19)
#define U3_PMA_FREDET_CHK_CYCLE		GENMASK(29, 10)
#define U3_PMA_FREDET_GOLDEN_CYCLE	GENMASK(19, 0)
#define U3_PMA_FREDET_TOLERATE_CYCLE	GENMASK(19, 0)
#define U3_PMA_LCK2DATA_DLY_TIME	GENMASK(23, 16)
#define U3_PMA_LCPLL_NCPO_VALUE		GENMASK(30, 0)
#define U3_PMA_LCPLL_PCW_NCPO_GPON	GENMASK(30, 0)
#define U3_PMA_LFPS_FINISH_TIME		GENMASK(31, 21)
#define U3_PMA_NCPO_ANA_MSB		GENMASK(17, 16)
#define U3_PMA_P1_TO_P0_DO_EQ_USB	BIT(26)
#define U3_PMA_P2_TO_P0_DO_EQ_USB	BIT(27)
#define U3_PMA_P3_TO_P0_DO_EQ_USB	BIT(28)
#define U3_PMA_PCIE_MODE_PLL_AUTO_EN	BIT(0)
#define U3_PMA_PCIE_MODE_PLL_AUTO_OFF_EN BIT(2)
#define U3_PMA_PCIE_MODE_PLL_AUTO_ON_EN	BIT(1)
#define U3_PMA_PCIE_USB_BYPASS_EQ_P1_EN	BIT(27)
#define U3_PMA_PCIE_USB_BYPASS_EQ_P2_EN	BIT(28)
#define U3_PMA_PCIE_USB_BYPASS_EQ_P3_EN	BIT(29)
#define U3_PMA_PCIE_USB_SYSTEM		BIT(26)
#define U3_PMA_PLL_BC_INTF		GENMASK(1, 0)
#define U3_PMA_PLL_BPA_INTF		GENMASK(4, 2)
#define U3_PMA_PLL_BPB_INTF		GENMASK(7, 6)
#define U3_PMA_PLL_EN_FORCE		BIT(4)
#define U3_PMA_PLL_EN_SEL		BIT(5)
#define U3_PMA_PLL_FBKSEL_INTF		GENMASK(13, 12)
#define U3_PMA_PLL_FORCE_STABLE		BIT(18)
#define U3_PMA_PLL_FORCE_UNSTABLE	BIT(19)
#define U3_PMA_PLL_ICOIQ_EN_INTF	BIT(14)
#define U3_PMA_PLL_ICOLP_EN_INTF	BIT(2)
#define U3_PMA_PLL_IR_INTF		GENMASK(19, 16)
#define U3_PMA_PLL_KBAND_PREDIV_INTF	GENMASK(21, 20)
#define U3_PMA_PLL_PCK_SEL_INTF		BIT(22)
#define U3_PMA_PLL_PHY_CK_EN_INTF	BIT(27)
#define U3_PMA_PLL_RICO_SEL_INTF	BIT(29)
#define U3_PMA_PLL_SDM_HREN_INTF	GENMASK(4, 3)
#define U3_PMA_PLL_SDM_IFM_INTF		BIT(30)
#define U3_PMA_PLL_SSC_DELTA_INTF	GENMASK(15, 0)
#define U3_PMA_PLL_SSC_EN		BIT(30)
#define U3_PMA_PLL_SSC_PERIOD_INTF	GENMASK(31, 16)
#define U3_PMA_REBACK_P0_LCK2REF_EN	BIT(26)
#define U3_PMA_ROC_CK_EN		BIT(1)
#define U3_PMA_RXDET_EN_WINDOW		GENMASK(9, 4)
#define U3_PMA_RXDET_RD_WAIT_TIMER	GENMASK(15, 12)
#define U3_PMA_RX_EQ_EN_H_DLY		GENMASK(28, 16)
#define U3_PMA_RX_EQ_EN_H_DLY_SHORT	GENMASK(12, 0)
#define U3_PMA_RX_PI_CAL_EN_H_DLY	GENMASK(7, 0)
#define U3_PMA_TX_DATA_EN_FORCE		BIT(12)
#define U3_PMA_TX_DATA_EN_SEL		BIT(13)
#define U3_PMA_TX_DATA_RATE_SEL		GENMASK(30, 28)
#define U3_PMA_XTAL_EXT_EN_FORCE	GENMASK(12, 11)
#define U3_PMA_XTAL_EXT_EN_SEL		BIT(13)

/* dig fields */
#define U3_DIG_MULTI_PHY_USB_EN		BIT(24)
#define U3_DIG_MULTI_USB2P5_EN		BIT(26)
#define U3_DIG_MULTI_USB5_EN		BIT(25)
#define U3_DIG_NS_CK_DIV_SEL		BIT(25)
#define U3_DIG_US_CK_DIV_SEL		BIT(24)

#define U3_MAX_CALIB_TRY		50

/** struct airoha_u3_write - one register update of an AN7583 U3 sequence */
struct airoha_u3_write {
	u8 block;
	u16 offset;
	u32 mask;
	u32 val;
};

static const struct airoha_u3_write an7583_u3_init_seq[] = {
	/* Digital clock trigger reverse */
	{ U3_BLK_PMA, U3_PMA_PON_RXFEDIG_CTRL_0, U3_PMA_EQ_RX500M_CK_SEL, 0 },
	/* RX USB PCIe enable */
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_36, U3_PMA_PCIE_USB_SYSTEM,
	  U3_PMA_PCIE_USB_SYSTEM },
	{ U3_BLK_PMA, U3_PMA_RX_SYS_CTRL_0, U3_PMA_ROC_CK_EN, 0 },
	/* 50MHz XTAL */
	{ U3_BLK_ANA, U3_ANA_SSUSB_BGR_EN,
	  U3_ANA_SSUSB_BG_DIV | U3_ANA_SSUSB_CHPEN, 0x6 },
	{ U3_BLK_ANA, U3_ANA_PLL_IPLL_DIG_PWR_SEL, U3_ANA_PLL_PREDIV,
	  0x02000000 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_4, U3_PMA_PLL_ICOLP_EN_INTF, 0 },
	{ U3_BLK_DIG, U3_DIG_CK_RST_CTRL_7,
	  U3_DIG_MULTI_USB2P5_EN | U3_DIG_MULTI_USB5_EN, 0 },
	{ U3_BLK_DIG, U3_DIG_CK_RST_CTRL_7,
	  U3_DIG_MULTI_USB2P5_EN | U3_DIG_MULTI_USB5_EN |
	  U3_DIG_MULTI_PHY_USB_EN,
	  U3_DIG_MULTI_USB2P5_EN | U3_DIG_MULTI_USB5_EN |
	  U3_DIG_MULTI_PHY_USB_EN },
	/* PLL */
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_2,
	  U3_PMA_PLL_PHY_CK_EN_INTF | U3_PMA_PLL_PCK_SEL_INTF |
	  U3_PMA_PLL_KBAND_PREDIV_INTF | U3_PMA_PLL_IR_INTF |
	  U3_PMA_PLL_ICOIQ_EN_INTF | U3_PMA_PLL_FBKSEL_INTF |
	  U3_PMA_PLL_BPB_INTF | U3_PMA_PLL_BPA_INTF | U3_PMA_PLL_BC_INTF,
	  0x08444057 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_4, U3_PMA_PLL_SDM_HREN_INTF, 0x8 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_2, U3_PMA_PLL_SDM_IFM_INTF,
	  U3_PMA_PLL_SDM_IFM_INTF },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_3,
	  U3_PMA_PLL_SSC_PERIOD_INTF | U3_PMA_PLL_SSC_DELTA_INTF, 0x018c021e },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_1, U3_PMA_PLL_SSC_EN,
	  U3_PMA_PLL_SSC_EN },
	{ U3_BLK_PMA, U3_PMA_SS_LCPLL_TDC_PCW_1, U3_PMA_LCPLL_PCW_NCPO_GPON,
	  0x48000000 },
	{ U3_BLK_PMA, U3_PMA_SS_LCPLL_PWCTL_SETTING_2, U3_PMA_NCPO_ANA_MSB,
	  0x00010000 },
	{ U3_BLK_PMA, U3_PMA_SS_LCPLL_TDC_FLT_2, U3_PMA_LCPLL_NCPO_VALUE,
	  0x48000000 },
	/* RX 5G */
	{ U3_BLK_ANA, U3_ANA_CDR_LPF_MJV_LIM, U3_ANA_CDR_LPF_RATIO, 0 },
	{ U3_BLK_ANA, U3_ANA_RXAFE_RESERVE, U3_ANA_CDR_PD_10B_EN,
	  U3_ANA_CDR_PD_10B_EN },
	{ U3_BLK_ANA, U3_ANA_CDR_PR_CKREF_DIV1, U3_ANA_CDR_PR_DAC_BAND,
	  0x00000c00 },
	{ U3_BLK_ANA, U3_ANA_CDR_FORCE_IBANDLPF_R_OFF, U3_ANA_CDR_PHYCK_RSTB,
	  0 },
	{ U3_BLK_ANA, U3_ANA_CDR_PR_KBAND_DIV_PCIE_REG,
	  U3_ANA_CDR_PR_XFICK_EN | U3_ANA_CDR_PR_KBAND_PCIE_MODE,
	  U3_ANA_CDR_PR_KBAND_PCIE_MODE },
	{ U3_BLK_ANA, U3_ANA_CDR_PR_CKREF_DIV1, U3_ANA_CDR_PR_KBAND_DIV,
	  0x03000000 },
	{ U3_BLK_ANA, U3_ANA_CDR_PR_KBAND_DIV_PCIE_REG,
	  U3_ANA_CDR_PR_KBAND_DIV_PCIE, 0x19 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_46, U3_PMA_REBACK_P0_LCK2REF_EN,
	  U3_PMA_REBACK_P0_LCK2REF_EN },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_49, U3_PMA_LFPS_FINISH_TIME, 0x3e000000 },
	/* EQ all */
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_50,
	  U3_PMA_P3_TO_P0_DO_EQ_USB | U3_PMA_P2_TO_P0_DO_EQ_USB |
	  U3_PMA_P1_TO_P0_DO_EQ_USB, 0 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_46,
	  U3_PMA_PCIE_USB_BYPASS_EQ_P3_EN | U3_PMA_PCIE_USB_BYPASS_EQ_P2_EN |
	  U3_PMA_PCIE_USB_BYPASS_EQ_P1_EN,
	  U3_PMA_PCIE_USB_BYPASS_EQ_P3_EN | U3_PMA_PCIE_USB_BYPASS_EQ_P2_EN |
	  U3_PMA_PCIE_USB_BYPASS_EQ_P1_EN },
	/* mainline clears only FIELD_PREP(PMA_RESERVE_3, BIT(7)), i.e. bit 31 */
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_38, 0x80000000, 0 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_11, U3_PMA_FORCE_SIGDET_5G,
	  U3_PMA_FORCE_SIGDET_5G },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_36, U3_PMA_LCK2DATA_DLY_TIME,
	  0x00020000 },
	/* PI cal */
	{ U3_BLK_PMA, U3_PMA_RX_DLY_0, U3_PMA_RX_PI_CAL_EN_H_DLY, 0x10 },
	{ U3_BLK_ANA, U3_ANA_PLL_SDM_ORD,
	  U3_ANA_PLL_SSC_TRI_EN | U3_ANA_PLL_SSC_PHASE_INI,
	  U3_ANA_PLL_SSC_TRI_EN | U3_ANA_PLL_SSC_PHASE_INI },
	{ U3_BLK_PMA, U3_PMA_TX_DA_CTRL_3, U3_PMA_TX_DATA_RATE_SEL, 0 },
	{ U3_BLK_ANA, U3_ANA_RXAFE_RESERVE, U3_ANA_CDR_PD_EDGE_DIS, 0 },
	/* Common setting */
	{ U3_BLK_DIG, U3_DIG_CK_RST_CTRL_3,
	  U3_DIG_NS_CK_DIV_SEL | U3_DIG_US_CK_DIV_SEL,
	  U3_DIG_NS_CK_DIV_SEL | U3_DIG_US_CK_DIV_SEL },
	{ U3_BLK_PMA, U3_PMA_TX_DA_CTRL_0,
	  U3_PMA_RXDET_RD_WAIT_TIMER | U3_PMA_RXDET_EN_WINDOW, 0x000040a0 },
	{ U3_BLK_ANA, U3_ANA_RXLBTX_EN,
	  U3_ANA_TX_DMEDGEGEN_EN | U3_ANA_TX_RXDET_METHOD,
	  U3_ANA_TX_DMEDGEGEN_EN },
	{ U3_BLK_PMA, U3_PMA_PLL_CK_CTRL_2,
	  U3_PMA_PCIE_MODE_PLL_AUTO_OFF_EN | U3_PMA_PCIE_MODE_PLL_AUTO_ON_EN |
	  U3_PMA_PCIE_MODE_PLL_AUTO_EN,
	  U3_PMA_PCIE_MODE_PLL_AUTO_OFF_EN | U3_PMA_PCIE_MODE_PLL_AUTO_ON_EN |
	  U3_PMA_PCIE_MODE_PLL_AUTO_EN },
	/* RX speed up */
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_5, U3_PMA_FREDET_CHK_CYCLE, 0x0000a000 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_6, U3_PMA_FREDET_GOLDEN_CYCLE, 0x64 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_7, U3_PMA_FREDET_TOLERATE_CYCLE, 0x2710 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_2, U3_PMA_RX_EQ_EN_H_DLY, 0x09c40000 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_45, U3_PMA_EQ_EN_DLY, 0x9c4 },
	{ U3_BLK_PMA, U3_PMA_RX_CTRL_50,
	  U3_PMA_EQ_EN_DLY_SHORT | U3_PMA_RX_EQ_EN_H_DLY_SHORT, 0x013889c4 },
	/* TCL: avoid LT noise impact */
	{ U3_BLK_ANA, U3_ANA_QP_TX_MODE_16B_EN, U3_ANA_TX_RESERVE_8,
	  U3_ANA_TX_RESERVE_8 },
	/* PLL auto init */
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_0, U3_PMA_AUTO_INIT, U3_PMA_AUTO_INIT },
};

/* KBand readout preparation (an7583_usb_phy_u3_kband_is_calibrated()) */
static const struct airoha_u3_write an7583_u3_kband_read_seq[] = {
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_4, U3_PMA_PLL_ICOLP_EN_INTF, 0 },
	{ U3_BLK_ANA, U3_ANA_SSUSB_BGR_EN, U3_ANA_BIAS_V2V_CAL, 0x02a00000 },
	{ U3_BLK_ANA, U3_ANA_PLL_IPLL_DIG_PWR_SEL,
	  U3_ANA_PLL_OSCAL_ENB | U3_ANA_PLL_LDOLPF_VSEL, 0x00080020 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_2, U3_PMA_PLL_RICO_SEL_INTF,
	  U3_PMA_PLL_RICO_SEL_INTF },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_8,
	  U3_PMA_XTAL_EXT_EN_SEL | U3_PMA_XTAL_EXT_EN_FORCE, 0 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_2, U3_PMA_PLL_ICOIQ_EN_INTF,
	  U3_PMA_PLL_ICOIQ_EN_INTF },
	/* clear AIROHA_USB_ANA_PLL_DEBUG_SEL before reading KBand */
	{ U3_BLK_ANA, U3_ANA_TDC_FT_CK_EN, U3_ANA_PLL_DEBUG_SEL, 0 },
};

/* an7583_usb_phy_kband_calibrate() */
static const struct airoha_u3_write an7583_u3_kband_cal_seq[] = {
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_8,
	  U3_PMA_XTAL_EXT_EN_SEL | U3_PMA_XTAL_EXT_EN_FORCE, 0x3800 },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_7,
	  U3_PMA_PLL_EN_SEL | U3_PMA_PLL_EN_FORCE, U3_PMA_PLL_EN_SEL },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_4, U3_PMA_PLL_ICOLP_EN_INTF,
	  U3_PMA_PLL_ICOLP_EN_INTF },
	{ U3_BLK_ANA, U3_ANA_SSUSB_BGR_EN, U3_ANA_BIAS_V2V_CAL, 0 },
	{ U3_BLK_ANA, U3_ANA_PLL_IPLL_DIG_PWR_SEL,
	  U3_ANA_PLL_OSCAL_ENB | U3_ANA_PLL_LDOLPF_VSEL, 0x60 },
	{ U3_BLK_PMA, U3_PMA_PLL_CTRL_2,
	  U3_PMA_PLL_RICO_SEL_INTF | U3_PMA_PLL_ICOIQ_EN_INTF, 0 },
	{ U3_BLK_ANA, U3_ANA_PLL_IPLL_DIG_PWR_SEL,
	  U3_ANA_PLL_MON_LDO_SEL | U3_ANA_PLL_MONVC_EN, 0x00034000 },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_7,
	  U3_PMA_PLL_EN_SEL | U3_PMA_PLL_EN_FORCE,
	  U3_PMA_PLL_EN_SEL | U3_PMA_PLL_EN_FORCE },
};

/* an7583_usb_phy_tx_pll_enable() */
static const struct airoha_u3_write an7583_u3_tx_pll_seq[] = {
	{ U3_BLK_ANA, U3_ANA_PLL_IPLL_DIG_PWR_SEL,
	  U3_ANA_PLL_MON_LDO_SEL | U3_ANA_PLL_MONVC_EN, 0 },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_7,
	  U3_PMA_PLL_EN_SEL | U3_PMA_PLL_EN_FORCE, 0 },
	{ U3_BLK_PMA, U3_PMA_PLL_CK_CTRL_1, U3_PMA_PLL_FORCE_UNSTABLE,
	  U3_PMA_PLL_FORCE_UNSTABLE },
	{ U3_BLK_PMA, U3_PMA_PLL_CK_CTRL_1, U3_PMA_PLL_FORCE_STABLE,
	  U3_PMA_PLL_FORCE_STABLE },
	{ U3_BLK_PMA, U3_PMA_PLL_CK_CTRL_1, U3_PMA_PLL_FORCE_UNSTABLE, 0 },
	{ U3_BLK_PMA, U3_PMA_PLL_CK_CTRL_1, U3_PMA_PLL_FORCE_STABLE, 0 },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_6, U3_PMA_TX_DATA_EN_FORCE,
	  U3_PMA_TX_DATA_EN_FORCE },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_6, U3_PMA_TX_DATA_EN_SEL, 0 },
	{ U3_BLK_PMA, U3_PMA_INTF_CTRL_6, U3_PMA_TX_DATA_EN_FORCE, 0 },
};

static void airoha_u3_apply(struct airoha_usb_phy_priv *priv,
			    const struct airoha_u3_write *seq,
			    unsigned int count)
{
	void __iomem *base;
	unsigned int i;

	for (i = 0; i < count; i++) {
		switch (seq[i].block) {
		case U3_BLK_ANA:
			base = priv->ana;
			break;
		case U3_BLK_PMA:
			base = priv->pma;
			break;
		default:
			base = priv->dig;
			break;
		}

		clrsetbits_le32(base + seq[i].offset, seq[i].mask, seq[i].val);
	}
}

static bool airoha_an7583_u3_kband_is_calibrated(struct airoha_usb_phy_priv *priv)
{
	u32 val, res;

	airoha_u3_apply(priv, an7583_u3_kband_read_seq,
			ARRAY_SIZE(an7583_u3_kband_read_seq));

	mdelay(5);
	val = readl(priv->pma + U3_PMA_INTF_STS_9);
	res = FIELD_GET(U3_PMA_ADDR_INTF_STS_PLL_VCOCAL, val) << 4;

	setbits_le32(priv->ana + U3_ANA_TDC_FT_CK_EN, U3_ANA_PLL_DEBUG_SEL);

	mdelay(5);
	val = readl(priv->pma + U3_PMA_INTF_STS_9);
	res |= FIELD_GET(U3_PMA_ADDR_INTF_STS_PLL_VCOCAL, val);

	/* Calibrated when the KBand code is not 0xfff and "done" is set */
	return res != 0xfff && (res & 0x800);
}

static void airoha_an7583_u3_init(struct airoha_usb_phy_priv *priv)
{
	int i;

	airoha_u3_apply(priv, an7583_u3_init_seq,
			ARRAY_SIZE(an7583_u3_init_seq));

	/* PLL auto init has to settle before it is released again */
	udelay(200);
	clrbits_le32(priv->pma + U3_PMA_PLL_CK_CTRL_2,
		     U3_PMA_PCIE_MODE_PLL_AUTO_EN);

	if (!airoha_an7583_u3_kband_is_calibrated(priv)) {
		/* TX force disable and PLL force unstable before recalibrating */
		clrsetbits_le32(priv->pma + U3_PMA_INTF_CTRL_6,
				U3_PMA_TX_DATA_EN_SEL | U3_PMA_TX_DATA_EN_FORCE,
				U3_PMA_TX_DATA_EN_SEL);
		setbits_le32(priv->pma + U3_PMA_PLL_CK_CTRL_1,
			     U3_PMA_PLL_FORCE_UNSTABLE);

		for (i = 0; i < U3_MAX_CALIB_TRY; i++) {
			airoha_u3_apply(priv, an7583_u3_kband_cal_seq,
					ARRAY_SIZE(an7583_u3_kband_cal_seq));

			/* Exit as soon as we manage to calibrate */
			if (airoha_an7583_u3_kband_is_calibrated(priv))
				break;
		}
	}

	airoha_u3_apply(priv, an7583_u3_tx_pll_seq,
			ARRAY_SIZE(an7583_u3_tx_pll_seq));

	/* Keep going without USB3 rather than failing the whole controller */
	if (!airoha_an7583_u3_kband_is_calibrated(priv))
		dev_warn(priv->dev, "U3 PLL KBand calibration failed\n");
}

/* EN7523 U3 PHY PLL settings, applied when the SoC runs from the 20 MHz
 * crystal: the vendor u3phy_config_en7523() does this when
 * get_xtal_sel() == 0, i.e. chip-SCU 0x254 bit 19 cleared.
 */
static void airoha_usb_phy_u3_pll_20m(struct airoha_usb_phy_priv *priv)
{
	void __iomem *phy = priv->phy;

	if (!priv->chip_scu) {
		dev_warn(priv->dev,
			 "no 'airoha,chip-scu', crystal unknown, U3 PLL left as is\n");
		return;
	}

	if (readl(priv->chip_scu + AIROHA_CHIP_SCU_XTAL_SEL) &
	    AIROHA_CHIP_SCU_XTAL_SEL_20M)
		return;		/* 40 MHz crystal */

	dev_dbg(priv->dev, "U3 PHY 20 MHz setting\n");

	clrsetbits_le32(phy + U3_PHYA_REG3, GENMASK(31, 24), 0x7d << 24);
	clrsetbits_le32(phy + U3_PHYA_REG4, GENMASK(31, 24), 0xf9 << 24);
	clrsetbits_le32(phy + U3_PHYA_REG5, GENMASK(7, 0), 0x40);
	clrsetbits_le32(phy + U3_PHYA_REG9, GENMASK(7, 0), 0x38);
	clrsetbits_le32(phy + U3_PHYA_REG10, GENMASK(23, 16), 0x36 << 16);
}

static void airoha_usb_phy_u3_init(struct airoha_usb_phy_priv *priv,
				   struct airoha_usb_phy_instance *inst)
{
	const struct airoha_usb_phy_data *data = priv->data;
	void __iomem *phy = priv->phy;

	/* Switch the serdes shared with PCIe/HSGMII/WiFi to USB. The AN7583
	 * describes the SCU in the USB3 subnode, the AN7581 in the PHY node
	 * itself (which is also where 'airoha,serdes-port' is described).
	 */
	if (inst->scu && data->scu_reg)
		setbits_le32(inst->scu + data->scu_reg, data->scu_bit);

	if (data->u3_full_init) {
		/* AN7583: SSUSB PHY in the ana/pma/dig blocks, brought up by
		 * the KBand calibrated PLL sequence.
		 */
		airoha_an7583_u3_init(priv);
		return;
	}

	if (data->u3_pll_20m) {
		/* EN7523: the U3 PHY keeps its reset values, only the PLL
		 * depends on the crystal of the board.
		 */
		airoha_usb_phy_u3_pll_20m(priv);
		return;
	}

	clrsetbits_le32(phy + U3_PHYA_REG8, U3_SSUSB_CDR_RST_DLY, 0);
	clrsetbits_le32(phy + U3_PHYA_REG6, U3_SSUSB_CDR_RESERVE,
			FIELD_PREP(U3_SSUSB_CDR_RESERVE, data->u3_cdr_reserve));
	clrsetbits_le32(phy + U3_PHYA_REG0, U3_SSUSB_BG_DIV,
			U3_SSUSB_BG_DIV_4);
	clrsetbits_le32(phy + U3_PHYA_REG1, U3_SSUSB_XTAL_TOP_RESERVE,
			FIELD_PREP(U3_SSUSB_XTAL_TOP_RESERVE, 0x600));
	clrsetbits_le32(phy + U3_PHYA_DA_REG19, U3_SSUSB_PLL_SSC_DELTA1_U3,
			FIELD_PREP(U3_SSUSB_PLL_SSC_DELTA1_U3, 0x43));
}

static void airoha_usb_phy_u3_power_on(struct airoha_usb_phy_priv *priv,
				       struct airoha_usb_phy_instance *inst)
{
	void __iomem *phy = priv->phy;

	if (priv->data->u3_ana_pma) {
		/* AN7583: power up the shared serdes blocks */
		setbits_le32(priv->ana + U3_ANA_TDC_FT_CK_EN, U3_ANA_VUSB10_ON);
		udelay(1000);
		clrbits_le32(phy + U3_GPIO_CTLD, U3_SSUSB_IP_SW_RST);
		udelay(1000);
		clrbits_le32(phy + U3_GPIO_CTLD, U3_FORCE_SSUSB_IP_SW_RST);
		udelay(1000);
		clrbits_le32(priv->pma + U3_PMA_INTF_CTRL_5, U3_PMA_RX_HZ_FORCE);
		udelay(1000);
		clrbits_le32(priv->pma + U3_PMA_INTF_CTRL_5, U3_PMA_RX_HZ_SEL);
		return;
	}

	/* EN7523 has no PHY level power gate for the U3 port, the controller
	 * resets the IP over IPPC instead (SSUSB_IP_SW_RST).
	 */
	if (!priv->data->u3_rst_mask)
		return;

	clrbits_le32(phy + U3_GPIO_CTLD, priv->data->u3_rst_mask);
	udelay(1000);
}

static void airoha_usb_phy_u3_power_off(struct airoha_usb_phy_priv *priv)
{
	void __iomem *phy = priv->phy;

	if (priv->data->u3_ana_pma) {
		setbits_le32(phy + U3_GPIO_CTLD, U3_FORCE_SSUSB_IP_SW_RST);
		udelay(1000);
		setbits_le32(phy + U3_GPIO_CTLD, U3_SSUSB_IP_SW_RST);
		udelay(1000);
		setbits_le32(priv->pma + U3_PMA_INTF_CTRL_5, U3_PMA_RX_HZ_FORCE);
		udelay(1000);
		setbits_le32(priv->pma + U3_PMA_INTF_CTRL_5, U3_PMA_RX_HZ_SEL);
		udelay(1000);
		clrbits_le32(priv->ana + U3_ANA_TDC_FT_CK_EN, U3_ANA_VUSB10_ON);
		return;
	}

	if (!priv->data->u3_rst_mask)
		return;

	setbits_le32(phy + U3_GPIO_CTLD, U3_SSUSB_IP_SW_RST |
		     U3_FORCE_SSUSB_IP_SW_RST);
	udelay(1000);
}

static struct airoha_usb_phy_instance *
airoha_usb_phy_get_instance(struct airoha_usb_phy_priv *priv, unsigned int type,
			    unsigned int offset, bool create)
{
	unsigned int i;

	for (i = 0; i < priv->num_instances; i++) {
		struct airoha_usb_phy_instance *inst = &priv->instances[i];

		if (type == PHY_TYPE_USB2 && (inst->type != type ||
					      inst->offset != offset))
			continue;
		if (type == PHY_TYPE_USB3 && inst->type != type)
			continue;

		return inst;
	}

	if (!create || priv->num_instances >= AIROHA_USB_PHY_MAX_INSTANCE)
		return NULL;

	/* Fallback for the SoCs whose PHY block has no subnodes: create the
	 * instances on demand, the DT only tells us which types are wanted.
	 */
	if (type == PHY_TYPE_USB2) {
		if (offset >= AIROHA_USB_PHY_MAX_INSTANCE * AIROHA_U2_PORT_OFFSET)
			return NULL;
	} else if (type == PHY_TYPE_USB3 && !priv->data->has_u3) {
		return NULL;
	}

	/* One instance per U2 port. The U3 port shares the offset 0 with the
	 * first U2 port of the same block, so it must never be mapped onto
	 * that instance.
	 */
	if (type == PHY_TYPE_USB2) {
		for (i = 0; i < priv->num_instances; i++) {
			if (priv->instances[i].type == PHY_TYPE_USB2 &&
			    priv->instances[i].offset == offset)
				return &priv->instances[i];
		}
	}

	i = priv->num_instances++;
	priv->instances[i].type = type;
	priv->instances[i].offset = type == PHY_TYPE_USB2 ? offset : 0;
	/* The SoCs where the instances are created on demand (AN7581) describe
	 * the monitor clock of their single U2 port on the PHY node itself.
	 */
	if (type == PHY_TYPE_USB2)
		priv->instances[i].monclk_sel =
			dev_read_u32_default(priv->dev,
					     "airoha,usb2-monitor-clk-sel",
					     priv->data->monclk_base +
					     offset / AIROHA_U2_PORT_OFFSET);
	priv->instances[i].scu = priv->scu;

	/* The USB3 port only works when the serdes it shares with PCIe / WiFi
	 * is described ('airoha,serdes-port', see AIROHA_SCU_SERDES_*). The
	 * upstream Linux driver refuses to register the USB3 instance when it
	 * is missing; a bootloader keeps the instance but leaves the serdes
	 * alone, so that the USB2 ports of the same controller still work.
	 */
	if (type == PHY_TYPE_USB3 && priv->data->scu_reg &&
	    (!priv->has_serdes_port || !priv->scu)) {
		priv->instances[i].disabled = true;
		dev_warn(priv->dev,
			 "USB3 port disabled: 'airoha,serdes-port' and 'airoha,scu' are needed to switch the serdes\n");
	}

	return &priv->instances[i];
}

static int airoha_usb_phy_of_xlate(struct phy *phy,
				   struct ofnode_phandle_args *args)
{
	struct airoha_usb_phy_priv *priv = dev_get_priv(phy->dev);
	struct airoha_usb_phy_instance *inst;
	unsigned int type, offset = 0;
	bool has_offset = false;

	if (!args->args_count)
		return -EINVAL;

	if (args->args_count > 1) {
		/* The phandle points at a child node of the PHY block: the
		 * phy core prepends the child 'reg' value, which is the U2
		 * register offset. The U3 child has no 'reg'.
		 */
		offset = args->args[0];
		type = args->args[1];
		has_offset = true;
		if (offset == (unsigned int)-1) {
			offset = 0;
			has_offset = false;
		}
	} else {
		type = args->args[0];
	}

	if (type != PHY_TYPE_USB2 && type != PHY_TYPE_USB3)
		return -EINVAL;

	inst = airoha_usb_phy_get_instance(priv, type, offset, true);
	if (!inst) {
		dev_err(phy->dev, "no PHY instance for type %u offset %#x\n",
			type, offset);
		return -ENODEV;
	}

	if (has_offset && inst->offset != offset)
		return -EINVAL;

	phy->id = inst - priv->instances;

	dev_dbg(phy->dev, "type %u offset 0x%x -> instance %lu\n", type, offset,
		phy->id);

	return 0;
}

static int airoha_usb_phy_init(struct phy *phy)
{
	struct airoha_usb_phy_priv *priv = dev_get_priv(phy->dev);
	struct airoha_usb_phy_instance *inst = &priv->instances[phy->id];

	if (inst->disabled)
		return 0;

	switch (inst->type) {
	case PHY_TYPE_USB2:
		airoha_usb_phy_u2_init(priv, inst);
		break;
	case PHY_TYPE_USB3:
		airoha_usb_phy_u3_init(priv, inst);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int airoha_usb_phy_power_on(struct phy *phy)
{
	struct airoha_usb_phy_priv *priv = dev_get_priv(phy->dev);
	struct airoha_usb_phy_instance *inst = &priv->instances[phy->id];

	if (inst->disabled)
		return 0;

	switch (inst->type) {
	case PHY_TYPE_USB2:
		airoha_usb_phy_u2_power_on(priv, inst);
		break;
	case PHY_TYPE_USB3:
		airoha_usb_phy_u3_power_on(priv, inst);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int airoha_usb_phy_power_off(struct phy *phy)
{
	struct airoha_usb_phy_priv *priv = dev_get_priv(phy->dev);
	struct airoha_usb_phy_instance *inst = &priv->instances[phy->id];

	if (inst->disabled)
		return 0;

	switch (inst->type) {
	case PHY_TYPE_USB2:
		airoha_usb_phy_u2_power_off(priv, inst);
		break;
	case PHY_TYPE_USB3:
		airoha_usb_phy_u3_power_off(priv);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct phy_ops airoha_usb_phy_ops = {
	.of_xlate = airoha_usb_phy_of_xlate,
	.init = airoha_usb_phy_init,
	.power_on = airoha_usb_phy_power_on,
	.power_off = airoha_usb_phy_power_off,
};

static int airoha_usb_phy_parse_subnodes(struct airoha_usb_phy_priv *priv)
{
	ofnode sub;

	dev_for_each_subnode(sub, priv->dev) {
		struct airoha_usb_phy_instance *inst;
		u32 offset = 0;

		if (priv->num_instances >= AIROHA_USB_PHY_MAX_INSTANCE)
			break;

		inst = &priv->instances[priv->num_instances];
		inst->scu = priv->scu;

		if (ofnode_read_u32(sub, "reg", &offset)) {
			u32 tmp;

			/* No register offset: this is the USB3 port, which the
			 * upstream binding marks with 'airoha,scu'.
			 */
			inst->type = PHY_TYPE_USB3;
			inst->offset = 0;
			if (!priv->data->has_u3) {
				dev_err(priv->dev,
					"%s: this SoC has no USB3 port\n",
					ofnode_get_name(sub));
				return -EINVAL;
			}
			if (!ofnode_read_u32(sub, "airoha,usb2-monitor-clk-sel",
					     &tmp))
				dev_warn(priv->dev,
					 "%s: USB2 properties on a USB3 port\n",
					 ofnode_get_name(sub));
		} else {
			inst->type = PHY_TYPE_USB2;
			inst->offset = offset;
			if (ofnode_read_u32(sub, "airoha,usb2-monitor-clk-sel",
					    &inst->monclk_sel))
				inst->monclk_sel = priv->data->monclk_base +
						   priv->num_instances;
		}

		if (ofnode_read_bool(sub, "airoha,scu")) {
			inst->scu = airoha_usb_phy_get_syscon(sub,
							      "airoha,scu");
			if (inst->type != PHY_TYPE_USB3)
				dev_warn(priv->dev,
					 "%s: SCU on a USB2 port is ignored\n",
					 ofnode_get_name(sub));
		} else if (inst->type == PHY_TYPE_USB3 &&
			   priv->data->scu_reg) {
			/* Do not touch a USB3 PHY whose serdes cannot be
			 * switched, it may be driving PCIe / HSGMII instead.
			 */
			inst->disabled = true;
			dev_warn(priv->dev,
				 "%s: no SCU, serdes left untouched\n",
				 ofnode_get_name(sub));
		}

		priv->num_instances++;
	}

	return priv->num_instances ? 0 : -ENODEV;
}

static int airoha_usb_phy_probe(struct udevice *dev)
{
	struct airoha_usb_phy_priv *priv = dev_get_priv(dev);
	int ret;

	priv->dev = dev;
	priv->data = (const struct airoha_usb_phy_data *)dev_get_driver_data(dev);
	if (!priv->data)
		return -EINVAL;

	priv->phy = dev_remap_addr_index(dev, 0);
	if (!priv->phy) {
		dev_err(dev, "missing PHY register block\n");
		return -EINVAL;
	}

	if (priv->data->u3_ana_pma) {
		priv->ana = dev_remap_addr_name(dev, "ana");
		priv->pma = dev_remap_addr_name(dev, "pma");
		priv->dig = dev_remap_addr_name(dev, "dig");
		if (!priv->ana || !priv->pma || !priv->dig) {
			dev_err(dev, "missing ana/pma/dig register block\n");
			return -EINVAL;
		}
	}

	/* The SCU is needed to switch the shared serdes to USB. The AN7583
	 * describes it in the USB3 subnode instead, which parse_subnodes()
	 * picks up per instance.
	 */
	priv->scu = airoha_usb_phy_get_syscon(dev_ofnode(dev), "airoha,scu");

	/* Chip-SCU carries the crystal / package straps used by the EN7523 U3
	 * PLL workaround.
	 */
	priv->chip_scu = airoha_usb_phy_get_syscon(dev_ofnode(dev),
						   "airoha,chip-scu");

	/* 'airoha,serdes-port' (AIROHA_SCU_SERDES_*) tells which serdes is
	 * shared with the USB3 port of this PHY. Without it the serdes must
	 * not be touched, so a USB3 instance is only usable when it is
	 * described (and an SCU phandle is available).
	 */
	priv->has_serdes_port = !dev_read_u32(dev, "airoha,serdes-port",
					      &priv->serdes_port);

	ret = airoha_usb_phy_parse_subnodes(priv);
	if (ret)
		/* No subnodes: the instances are created on demand in
		 * of_xlate(), driven by the client's 'phys' property.
		 */
		priv->num_instances = 0;

	dev_dbg(dev, "%s: phy %p, instances %u, scu %p, chip-scu %p, serdes %d\n",
		priv->data->name, priv->phy, priv->num_instances, priv->scu,
		priv->chip_scu, priv->has_serdes_port ? (int)priv->serdes_port : -1);

	return 0;
}

/* AN7583: one PHY block driving two USB2 ports and one USB3 port */
static const struct airoha_usb_phy_data airoha_an7583_usb_phy_data = {
	.name = "an7583",
	.fs_cr = 0x6,
	.fs_sr = 0x2,
	.sqth = 0x8,
	.discth = 0x9,
	.monclk_base = 1,
	.u2_calibrate = true,
	.u2_full_power = true,
	.has_u3 = true,
	/* The AN7583 SSUSB PHY is configured through the ana/pma/dig blocks
	 * (KBand calibrated PLL), it does not use the AN7581 style U3
	 * registers inside the PHY block.
	 */
	.u3_full_init = true,
	.u3_ana_pma = true,
	.scu_reg = AIROHA_SCU_SSR3,
	.scu_bit = AIROHA_SCU_SSR3_HSGMII_SEL,
};

/* AN7581: two controllers, one PHY block per port */
static const struct airoha_usb_phy_data airoha_an7581_usb_phy_data = {
	.name = "an7581",
	.fs_cr = 0x6,
	.fs_sr = 0x2,
	.sqth = 0x9,
	.discth = 0xa,
	.monclk_base = 1,
	.u2_calibrate = true,
	.has_u3 = true,
	.u3_cdr_reserve = 0xe,
	.u3_rst_mask = U3_SSUSB_IP_SW_RST | U3_MCU_BUS_CK_GATE_EN |
		       U3_FORCE_SSUSB_IP_SW_RST | U3_SSUSB_SW_RST,
	.scu_reg = AIROHA_SCU_SSTR,
	.scu_bit = AIROHA_SCU_SSTR_USB_PCIE_SEL,
};

/* AN7552/AN7563: two USB2 ports and one USB3 port in a single block */
static const struct airoha_usb_phy_data airoha_an7563_usb_phy_data = {
	.name = "an7563",
	.fs_cr = 0x6,
	.fs_sr = 0x2,
	.sqth = 0x9,
	.discth = 0x9,
	.monclk_base = 1,
	.u2_calibrate = true,
	.has_u3 = true,
	.u3_cdr_reserve = 0x8,
	.u3_rst_mask = U3_SSUSB_IP_SW_RST | U3_FORCE_SSUSB_IP_SW_RST,
};

/* EN7523: two USB2 ports plus a USB3 port in the same register block. The
 * vendor runs u3phy_config_en7523() unconditionally on ARMv8: the U3 PLL is
 * patched for a 20 MHz crystal and no GPIO_CTLD power gate is used (the
 * controller resets the IP over IPPC). The PDIDR (package) tuning of that
 * function is not ported, GET_PDIDR() is not available here.
 * Whether the USB3 port is wired is a board property - the vendor keeps a
 * u3_port_num per package (7523DU/GU/SU/DT/DTM have none) and checks
 * get_serdes_interface_sel() on the host side, so the port is only brought
 * up when the board describes it in 'phys'.
 */
static const struct airoha_usb_phy_data airoha_en7523_usb_phy_data = {
	.name = "en7523",
	.fs_cr = 0x5,
	.fs_sr = 0xff,
	.sqth = 0xff,
	.discth = 0xff,
	.monclk_base = 0,
	.u2_calibrate = true,
	.has_u3 = true,
	.u3_pll_20m = true,
};

static const struct udevice_id airoha_usb_phy_ids[] = {
	{ .compatible = "airoha,an7583-usb-phy",
	  .data = (ulong)&airoha_an7583_usb_phy_data },
	{ .compatible = "airoha,an7581-usb-phy",
	  .data = (ulong)&airoha_an7581_usb_phy_data },
	{ .compatible = "airoha,an7563-usb-phy",
	  .data = (ulong)&airoha_an7563_usb_phy_data },
	{ .compatible = "airoha,en7523-usb-phy",
	  .data = (ulong)&airoha_en7523_usb_phy_data },
	{ }
};

U_BOOT_DRIVER(airoha_usb_phy) = {
	.name = "airoha-usb-phy",
	.id = UCLASS_PHY,
	.of_match = airoha_usb_phy_ids,
	.probe = airoha_usb_phy_probe,
	.ops = &airoha_usb_phy_ops,
	.priv_auto = sizeof(struct airoha_usb_phy_priv),
};
