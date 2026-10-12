/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 AIROHA Inc
 * Author: Cassia Lin <cassialin@airoha.com>
 * Author: Zhengping Zhang <zhengping.zhang@airoha.com>
 *
 * The IDs are the reset controller (SCU) register bits in declaration
 * order: RST_CTRL2 occupies IDs 0..24, RST_CTRL1 IDs 25..48 and
 * RST_CTRL3 IDs 49..55.  The driver keeps that same numbering, the ID is
 * mapped onto (bank << 5) | bit by an7589_rst_map[].
 */

#ifndef __DT_BINDINGS_RESET_CONTROLLER_AIROHA_AN7589_H_
#define __DT_BINDINGS_RESET_CONTROLLER_AIROHA_AN7589_H_

/* RST_CTRL2 */
#define AN7589_XPON_PHY_RST		 0
#define AN7589_GPON_OLT_RST		 1
#define AN7589_CPU_TIMER2_RST		 2
#define AN7589_HSUART_RST		 3
#define AN7589_UART4_RST		 4
#define AN7589_UART5_RST		 5
#define AN7589_I2C2_RST			 6
#define AN7589_XSI_MAC_RST		 7
#define AN7589_XSI_PHY_RST		 8
#define AN7589_NPU_RST			 9
#define AN7589_TRNG_MSTART_RST		10
#define AN7589_DUAL_HSI0_RST		11
#define AN7589_DUAL_HSI1_RST		12
#define AN7589_DUAL_HSI0_MAC_RST	13
#define AN7589_DUAL_HSI1_MAC_RST	14
#define AN7589_XPON_XFI_RST			15
#define AN7589_WDMA_RST			16
#define AN7589_WOE0_RST			17
#define AN7589_HSDMA_RST		18
#define AN7589_TDMA_RST			19
#define AN7589_EMMC_RST			20
#define AN7589_SOE_RST			21
#define AN7589_XFP_MAC_RST		22
#define AN7589_MDIO0                    23
#define AN7589_MDIO1                    24
/* RST_CTRL1 */
#define AN7589_PCM1_ZSI_ISI_RST		25
#define AN7589_FE_PDMA_RST		26
#define AN7589_FE_QDMA_RST		27
#define AN7589_PCM_SPIWP_RST		28
#define AN7589_CRYPTO_RST		29
#define AN7589_TIMER_RST		30
#define AN7589_PCM1_RST			31
#define AN7589_UART_RST			32
#define AN7589_GPIO_RST			33
#define AN7589_GDMA_RST			34
#define AN7589_I2C_MASTER_RST		35
#define AN7589_PCM2_ZSI_ISI_RST		36
#define AN7589_SFC_RST			37
#define AN7589_UART2_RST		38
#define AN7589_GDMP_RST			39
#define AN7589_FE_RST			40
#define AN7589_USB_HOST_P0_RST		41
#define AN7589_GSW_RST			42
#define AN7589_SFC2_PCM_RST		43
#define AN7589_PCIE0_RST		44
#define AN7589_PCIE1_RST		45
#define AN7589_CPU_TIMER_RST		46
#define AN7589_PCIE_HB_RST		47
#define AN7589_XPON_MAC_RST		48

/* RST_CTRL3 */
#define AN7589_I2C3_RST		49
#define AN7589_I2C4_RST		50
#define AN7589_I2C5_RST		51
#define AN7589_OPTL_SYS_RST		52
#define AN7589_MDIO2		53
#define AN7589_I2S_RST		54
#define AN7589_PCIE2_RST		55

#endif /* __DT_BINDINGS_RESET_CONTROLLER_AIROHA_AN7589_H_ */
