/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_CLOCK_AIROHA_EN7523_H_
#define _DT_BINDINGS_CLOCK_AIROHA_EN7523_H_

/*
 * based on dts/upstream/include/dt-bindings/clock/en7523-clk.h
 *
 * The AN7589 clock IDs are not (yet) part of the upstream binding;  and
 * are appended here so the U-Boot device trees can reference them.
 */

#define EN7523_CLK_GSW		0
#define EN7523_CLK_EMI		1
#define EN7523_CLK_BUS		2
#define EN7523_CLK_SLIC		3
#define EN7523_CLK_SPI		4
#define EN7523_CLK_NPU		5
#define EN7523_CLK_CRYPTO	6
#define EN7523_CLK_PCIE		7

#define EN7581_CLK_EMMC		8

#define AN7583_CLK_MDIO0	9
#define AN7583_CLK_MDIO1	10

/* AN7589 */
#define AN7589_CLK_SOE			11
#define AN7589_CLK_TODGEN		12
#define AN7589_CLK_EFUSE		13
#define AN7589_CLK_TINY_NPU		14
#define AN7589_CLK_ECC			15
#define AN7589_CLK_PM_UPS		16
#define AN7589_CLK_DQ_UPS		17
#define AN7589_CLK_TM_UPS		18
#define AN7589_CLK_ACP			19
#define AN7589_CLK_MDIO2		20

#define AN7589_CLK_TOP_CB_M_D2		21
#define AN7589_CLK_TOP_SPI2_SEL		22
#define AN7589_CLK_INFRA_104M_SPI2	23
#define AN7589_CLK_INFRA_66M_SPI2_HCK	24

#endif /* _DT_BINDINGS_CLOCK_AIROHA_EN7523_H_ */
