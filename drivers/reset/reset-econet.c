// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on Linux drivers/clk/clk-en7523.c reworked
 * and detached to a dedicated driver
 *
 * Author: Lorenzo Bianconi <lorenzo@kernel.org> (original driver)
 *	   Christian Marangi <ansuelsmth@gmail.com>
 */

#include <dm.h>
#include <linux/io.h>
#include <reset-uclass.h>
#include <regmap.h>
#include <soc/airoha/scu-regmap.h>

#include <dt-bindings/reset/econet,en751221-scu.h>
#include <dt-bindings/reset/econet,en7528-scu.h>

#define RST_NR_PER_BANK			32

#define REG_RESET_CONTROL2		0x830
#define REG_RESET_CONTROL1		0x834
#define EN751221_REG_RST_DMT		0x084
#define EN751221_REG_RST_USB		0x0ec

struct econet_reset_priv {
	const u16 *bank_ofs;
	const u16 *idx_map;
	int num_rsts;
	struct regmap *map;
};

static const u16 en751221_rst_ofs[] = {
	REG_RESET_CONTROL2,
	REG_RESET_CONTROL1,
	EN751221_REG_RST_DMT,
	EN751221_REG_RST_USB,
};

static const u16 en751221_rst_map[] = {
	/* RST_CTRL2 */
	[EN751221_XPON_PHY_RST]		= 0,
	[EN751221_GFAST_RST]		= 1,
	[EN751221_CPU_TIMER2_RST]	= 2,
	[EN751221_UART3_RST]		= 3,
	[EN751221_UART4_RST]		= 4,
	[EN751221_UART5_RST]		= 5,
	[EN751221_I2C2_RST]		= 6,
	[EN751221_XSI_MAC_RST]		= 7,
	[EN751221_XSI_PHY_RST]		= 8,

	/* RST_CTRL1 */
	[EN751221_PCM1_ZSI_ISI_RST]	= RST_NR_PER_BANK + 0,
	[EN751221_FE_QDMA1_RST]		= RST_NR_PER_BANK + 1,
	[EN751221_FE_QDMA2_RST]		= RST_NR_PER_BANK + 2,
	[EN751221_FE_UNZIP_RST]		= RST_NR_PER_BANK + 3,
	[EN751221_PCM2_RST]		= RST_NR_PER_BANK + 4,
	[EN751221_PTM_MAC_RST]		= RST_NR_PER_BANK + 5,
	[EN751221_CRYPTO_RST]		= RST_NR_PER_BANK + 6,
	[EN751221_SAR_RST]		= RST_NR_PER_BANK + 7,
	[EN751221_TIMER_RST]		= RST_NR_PER_BANK + 8,
	[EN751221_INTC_RST]		= RST_NR_PER_BANK + 9,
	[EN751221_BONDING_RST]		= RST_NR_PER_BANK + 10,
	[EN751221_PCM1_RST]		= RST_NR_PER_BANK + 11,
	[EN751221_UART_RST]		= RST_NR_PER_BANK + 12,
	[EN751221_GPIO_RST]		= RST_NR_PER_BANK + 13,
	[EN751221_GDMA_RST]		= RST_NR_PER_BANK + 14,
	[EN751221_I2C_MASTER_RST]	= RST_NR_PER_BANK + 16,
	[EN751221_PCM2_ZSI_ISI_RST]	= RST_NR_PER_BANK + 17,
	[EN751221_SFC_RST]		= RST_NR_PER_BANK + 18,
	[EN751221_UART2_RST]		= RST_NR_PER_BANK + 19,
	[EN751221_GDMP_RST]		= RST_NR_PER_BANK + 20,
	[EN751221_FE_RST]			= RST_NR_PER_BANK + 21,
	[EN751221_USB_HOST_P0_RST]	= RST_NR_PER_BANK + 22,
	[EN751221_GSW_RST]		= RST_NR_PER_BANK + 23,
	[EN751221_SFC2_PCM_RST]		= RST_NR_PER_BANK + 25,
	[EN751221_PCIE0_RST]		= RST_NR_PER_BANK + 26,
	[EN751221_PCIE1_RST]		= RST_NR_PER_BANK + 27,
	[EN751221_CPU_TIMER_RST]	= RST_NR_PER_BANK + 28,
	[EN751221_PCIE_HB_RST]		= RST_NR_PER_BANK + 29,
	[EN751221_SIMIF_RST]		= RST_NR_PER_BANK + 30,
	[EN751221_XPON_MAC_RST]		= RST_NR_PER_BANK + 31,

	/* RST_DMT */
	[EN751221_DMT_RST]		= 2 * RST_NR_PER_BANK + 0,

	/* RST_USB */
	[EN751221_USB_PHY_P0_RST]	= 3 * RST_NR_PER_BANK + 6,
	[EN751221_USB_PHY_P1_RST]	= 3 * RST_NR_PER_BANK + 7,
};

static int econet_reset_update(struct econet_reset_priv *priv,
			       unsigned long id, bool assert)
{
	u16 offset = priv->bank_ofs[id / RST_NR_PER_BANK];

	return regmap_update_bits(priv->map, offset,
				  BIT(id % RST_NR_PER_BANK),
				  assert ? BIT(id % RST_NR_PER_BANK) : 0);
}

static int econet_reset_assert(struct reset_ctl *reset_ctl)
{
	struct econet_reset_priv *priv = dev_get_priv(reset_ctl->dev);
	int id = reset_ctl->id;

	return econet_reset_update(priv, id, true);
}

static int econet_reset_deassert(struct reset_ctl *reset_ctl)
{
	struct econet_reset_priv *priv = dev_get_priv(reset_ctl->dev);
	int id = reset_ctl->id;

	return econet_reset_update(priv, id, false);
}

static int econet_reset_status(struct reset_ctl *reset_ctl)
{
	struct econet_reset_priv *priv = dev_get_priv(reset_ctl->dev);
	int id = reset_ctl->id;
	u16 offset;
	u32 val;
	int ret;

	offset = priv->bank_ofs[id / RST_NR_PER_BANK];
	ret = regmap_read(priv->map, offset, &val);
	if (ret)
		return ret;

	return !!(val & BIT(id % RST_NR_PER_BANK));
}

static int econet_reset_xlate(struct reset_ctl *reset_ctl,
			      struct ofnode_phandle_args *args)
{
	struct econet_reset_priv *priv = dev_get_priv(reset_ctl->dev);

	if (args->args[0] >= priv->num_rsts)
		return -EINVAL;

	reset_ctl->id = priv->idx_map[args->args[0]];

	return 0;
}

static struct reset_ops econet_reset_ops = {
	.of_xlate = econet_reset_xlate,
	.rst_assert = econet_reset_assert,
	.rst_deassert = econet_reset_deassert,
	.rst_status = econet_reset_status,
};

static int reset_init(struct udevice *dev, const u16 *rst_map,
		      const u16 *rst_ofs, int num_rsts)
{
	struct econet_reset_priv *priv = dev_get_priv(dev);

	priv->map = airoha_get_scu_regmap();
	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	priv->bank_ofs = rst_ofs;
	priv->idx_map = rst_map;
	priv->num_rsts = num_rsts;

	return 0;
}

static int econet_reset_probe(struct udevice *dev)
{
	ofnode node = dev_ofnode(dev);

	if (ofnode_device_is_compatible(node, "econet,en751221-scu") ||
	    ofnode_device_is_compatible(node, "econet,en7528-scu"))
		return reset_init(dev, en751221_rst_map, en751221_rst_ofs,
				  ARRAY_SIZE(en751221_rst_map));

	return -ENODEV;
}

U_BOOT_DRIVER(econet_reset) = {
	.name = "econet-reset",
	.id = UCLASS_RESET,
	.probe = econet_reset_probe,
	.ops = &econet_reset_ops,
	.priv_auto = sizeof(struct econet_reset_priv),
};
