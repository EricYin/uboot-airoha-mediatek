// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 AIROHA Inc
 *
 * Ported from the EcoNet vendor driver spi_nfi.c (u-boot-2014.04-rc1
 * drivers/misc/ecnt/flash and TF-A 2.10 plat/ecnt/common/drivers/flash).
 *
 * Adaptations:
 *   - the register base comes from airoha_nfi_base, filled in by the U-Boot
 *     glue driver from the device tree,
 *   - register access uses readl()/writel(),
 *   - the SPI-NAND / SPI-NOR page helpers and the register dump are dropped,
 *     they belong to the SPI-NAND backend (drivers/spi/airoha_snfi_spi.c),
 *   - the print shims are replaced by pr_err().
 */

/*======================================================================================
 * MODULE NAME: nand
 * FILE NAME: spi_nfi.c
 * VERSION: 3.00
 * PURPOSE: To provide the NFI(DMA) access interface of the parallel NAND mode.
 *======================================================================================
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>

#include "airoha_spi_nfi.h"

/* NAMING CONSTANT DECLARATIONS ------------------------------------------------------ */

/*******************************************************************************
 * NFI Register Definition
 *******************************************************************************/
#define _SPI_NFI_REGS_BASE		airoha_nfi_base

#define _SPI_NFI_REGS_CNFG		(_SPI_NFI_REGS_BASE + 0x0000)
#define _SPI_NFI_REGS_PAGEFMT		(_SPI_NFI_REGS_BASE + 0x0004)
#define _SPI_NFI_REGS_CON		(_SPI_NFI_REGS_BASE + 0x0008)
#define _SPI_NFI_REGS_INTR_EN		(_SPI_NFI_REGS_BASE + 0x0010)
#define _SPI_NFI_REGS_INTR		(_SPI_NFI_REGS_BASE + 0x0014)
#define _SPI_NFI_REGS_CMD		(_SPI_NFI_REGS_BASE + 0x0020)
#define _SPI_NFI_REGS_STA		(_SPI_NFI_REGS_BASE + 0x0060)
#define _SPI_NFI_REGS_FIFOSTA		(_SPI_NFI_REGS_BASE + 0x0064)
#define _SPI_NFI_REGS_STRADDR		(_SPI_NFI_REGS_BASE + 0x0080)
#define _SPI_NFI_REGS_FDM0L		(_SPI_NFI_REGS_BASE + 0x00A0)
#define _SPI_NFI_REGS_FDM0M		(_SPI_NFI_REGS_BASE + 0x00A4)
#define _SPI_NFI_REGS_FDM7L		(_SPI_NFI_REGS_BASE + 0x00D8)
#define _SPI_NFI_REGS_FDM7M		(_SPI_NFI_REGS_BASE + 0x00DC)
#define _SPI_NFI_REGS_MASTERSTA		(_SPI_NFI_REGS_BASE + 0x0224)
#define _SPI_NFI_REGS_SECCUS_SIZE	(_SPI_NFI_REGS_BASE + 0x022C)
#define _SPI_NFI_REGS_SNF_MISC_CTL	(_SPI_NFI_REGS_BASE + 0x0538)
#define _SPI_NFI_REGS_SNF_STA_CTL1	(_SPI_NFI_REGS_BASE + 0x0550)
#define _SPI_NFI_REGS_SNF_STA_CTL2	(_SPI_NFI_REGS_BASE + 0x0554)
#define _SPI_NFI_REGS_SNF_NFI_CNFG	(_SPI_NFI_REGS_BASE + 0x055C)

#define _PARALLEL_NFI_REGS_ACCCON	(_SPI_NFI_REGS_BASE + 0x000C)
#define _PARALLEL_NFI_REGS_ADDRNOB	(_SPI_NFI_REGS_BASE + 0x0030)
#define _PARALLEL_NFI_REGS_COLADDR	(_SPI_NFI_REGS_BASE + 0x0034)
#define _PARALLEL_NFI_REGS_ROWADDR	(_SPI_NFI_REGS_BASE + 0x0038)
#define _PARALLEL_NFI_REGS_CNRNB	(_SPI_NFI_REGS_BASE + 0x0044)
#define _PARALLEL_NFI_REGS_DATAW	(_SPI_NFI_REGS_BASE + 0x0050)
#define _PARALLEL_NFI_REGS_DATAR	(_SPI_NFI_REGS_BASE + 0x0054)
#define _PARALLEL_NFI_REGS_PIO_DRDY	(_SPI_NFI_REGS_BASE + 0x0058)
#define _PARALLEL_NFI_REGS_LOCKSTA	(_SPI_NFI_REGS_BASE + 0x0068)
#define _PARALLEL_NFI_REGS_CSEL		(_SPI_NFI_REGS_BASE + 0x0090)
#define _PARALLEL_NFI_REGS_LOCK		(_SPI_NFI_REGS_BASE + 0x0100)
#define _PARALLEL_NFI_REGS_RD_CNT	(_SPI_NFI_REGS_BASE + 0x0568)
#define _PARALLEL_NFI_REGS_WR_CNT	(_SPI_NFI_REGS_BASE + 0x056C)
#define _PARALLEL_NFI_REGS_CNT_CLR	(_SPI_NFI_REGS_BASE + 0x0578)

/*******************************************************************************
 * NFI Register Field Definition
 *******************************************************************************/

/* NFI_CNFG */
#define _SPI_NFI_REGS_CNFG_AHB				(0x0001)
#define _SPI_NFI_REGS_CNFG_READ_EN			(0x0002)
#define _SPI_NFI_REGS_CNFG_DMA_BURST_EN			(0x0004)
/* Flash -> SRAM */
#define _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_EN		(0x0008)
/* SRAM -> Flash */
#define _SPI_NFI_REGS_CNFG_DMA_RD_SWAP_EN		(0x0010)
#define _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_EN	(0x0020)
#define _SPI_NFI_REGS_CNFG_HW_ECC_EN			(0x0100)
#define _SPI_NFI_REGS_CNFG_AUTO_FMT_EN			(0x0200)

#define _SPI_NFI_REGS_CONF_OP_IDEL			(0x00)
#define _SPI_NFI_REGS_CONF_OP_SINGLE_READ		(0x02)
#define _SPI_NFI_REGS_CONF_OP_ERASE			(0x04)
#define _SPI_NFI_REGS_CONF_OP_PRGM			(3)
#define _SPI_NFI_REGS_CONF_OP_READ			(6)
#define _SPI_NFI_REGS_CONF_OP_MASK			(0x7000)
#define _SPI_NFI_REGS_CONF_OP_SHIFT			(12)

#define _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_SHIFT		(0x0003)
#define _SPI_NFI_REGS_CNFG_DMA_RD_SWAP_SHIFT		(0x0004)
#define _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_MASK		(1 << _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_SHIFT)
#define _SPI_NFI_REGS_CNFG_DMA_RD_SWAP_MASK		(1 << _SPI_NFI_REGS_CNFG_DMA_RD_SWAP_SHIFT)

#define _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_SHIFT	(0x0005)
#define _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_MASK	(1 << _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_SHIFT)

/* NFI_PAGEFMT */
#define _SPI_NFI_REGS_PAGEFMT_PAGE_512		(0x0000)
#define _SPI_NFI_REGS_PAGEFMT_PAGE_2K		(0x0001)
#define _SPI_NFI_REGS_PAGEFMT_PAGE_4K		(0x0002)
#define _SPI_NFI_REGS_PAGEFMT_PAGE_MASK		(0x0003)
#define _SPI_NFI_REGS_PAGEFMT_PAGE_SHIFT	(0x0000)

#define _SPI_NFI_REGS_PAGEFMT_SPARE_16		(0x0000)
#define _SPI_NFI_REGS_PAGEFMT_SPARE_26		(0x0001)
#define _SPI_NFI_REGS_PAGEFMT_SPARE_27		(0x0002)
#define _SPI_NFI_REGS_PAGEFMT_SPARE_28		(0x0003)
#define _SPI_NFI_REGS_PAGEFMT_SPARE_MASK	(0x0030)
#define _SPI_NFI_REGS_PAGEFMT_SPARE_SHIFT	(4)

#define _SPI_NFI_REGS_PAGEFMT_FDM_MASK		(0x0F00)
#define _SPI_NFI_REGS_PAGEFMT_FDM_SHIFT		(8)
#define _SPI_NFI_REGS_PAGEFMT_FDM_ECC_MASK	(0xF000)
#define _SPI_NFI_REGS_PAGEFMT_FDM_ECC_SHIFT	(12)

/* NFI_CON */
#define _SPI_NFI_REGS_CON_SEC_MASK		(0xF000)
#define _SPI_NFI_REGS_CON_WR_TRIG		(0x0200)
#define _SPI_NFI_REGS_CON_RD_TRIG		(0x0100)
#define _SPI_NFI_REGS_CON_SEC_SHIFT		(12)
#define _SPI_NFI_REGS_CON_RESET_VALUE		(0x3)

/* NFI_INTR_EN */
#define _SPI_NFI_REGS_INTR_EN_AHB_DONE_EN	(0x0040)

/* NFI_INTR */
#define _SPI_NFI_REGS_INTR_AHB_DONE_CHECK	(0x0040)

/* NFI_SECCUS_SIZE */
#define _SPI_NFI_REGS_SECCUS_SIZE_EN		(0x00010000)
#define _SPI_NFI_REGS_SECCUS_SIZE_MASK		(0x00001FFF)
#define _SPI_NFI_REGS_SECCUS_SIZE_SHIFT		(0)

/* NFI_SNF_MISC_CTL */
#define _SPI_NFI_REGS_SNF_MISC_CTL_DATA_RW_MODE_SHIFT	(16)

/* NFI_SNF_MISC_CTL2 */
#define _SPI_NFI_REGS_SNF_MISC_CTL2_WR_MASK	(0x1FFF0000)
#define _SPI_NFI_REGS_SNF_MISC_CTL2_WR_SHIFT	(16)
#define _SPI_NFI_REGS_SNF_MISC_CTL2_RD_MASK	(0x00001FFF)
#define _SPI_NFI_REGS_SNF_MISC_CTL2_RD_SHIFT	(0)

/* NFI_CMD */
#define _SPI_NFI_REGS_CMD_READ_VALUE		(0x00)
#define _SPI_NFI_REGS_CMD_WRITE_VALUE		(0x80)

/* SNF_STA_CTL1 */
#define _SPI_NFI_REGS_LOAD_TO_CACHE_DONE	(0x04000000)
#define _SPI_NFI_REGS_READ_FROM_CACHE_DONE	(0x02000000)

#define _PARALLEL_NFI_REGS_CNT_CLR_WR		(0x04)
#define _PARALLEL_NFI_REGS_CNT_CLR_RD		(0x08)
#define _PARALLEL_NFI_REGS_CNRNB_TIME		(0x0F)	/* units is 16T */

/* MACRO DECLARATIONS ---------------------------------------------------------------- */

#define INREG32(x)		readl((void __iomem *)(uintptr_t)(x))
#define OUTREG32(x, y)		writel((u32)(y), (void __iomem *)(uintptr_t)(x))

#define SETREG32(x, y)		OUTREG32(x, INREG32(x) | (y))
#define CLRREG32(x, y)		OUTREG32(x, INREG32(x) & ~(y))
#define MASKREG32(x, y, z)	OUTREG32(x, (INREG32(x) & ~(y)) | (z))

#define _SPI_NFI_REG32_READ(addr)			INREG32(addr)
#define _SPI_NFI_REG32_WRITE(addr, data)		OUTREG32(addr, data)
#define _SPI_NFI_REG32_SETBITS(addr, data)		SETREG32(addr, data)
#define _SPI_NFI_REG32_CLRBITS(addr, data)		CLRREG32(addr, data)
#define _SPI_NFI_REG32_SETMASKBITS(addr, mask, data)	MASKREG32(addr, mask, data)

#define _SPI_NFI_REG16_READ(addr)			INREG32(addr)
#define _SPI_NFI_REG16_WRITE(addr, data)		OUTREG32(addr, data)
#define _SPI_NFI_REG16_SETBITS(addr, data)		SETREG32(addr, data)
#define _SPI_NFI_REG16_CLRBITS(addr, data)		CLRREG32(addr, data)
#define _SPI_NFI_REG16_SETMASKBITS(addr, mask, data)	MASKREG32(addr, mask, data)

#define _SPI_NFI_GET_CONF_PTR				&(_spi_nfi_conf_info_t)
#define _SPI_NFI_GET_FDM_PTR				&(_spi_nfi_fdm_value)
#define _SPI_NFI_DATA_SIZE_WITH_ECC			(512)
#define _SPI_NFI_CHECK_DONE_MAX_TIMES			(1000000)
#define _SPI_NFI_MEMCPY					memcpy
#define _SPI_NFI_MEMSET					memset
#define _SPI_NFI_MAX_FDM_NUMBER				(64)
#define _SPI_NFI_MAX_FDM_PER_SEC			(8)

/* TYPE DECLARATIONS ----------------------------------------------------------------- */

typedef union {
	struct NFI_CNFG {
		u32 dma_mode		:1;
		u32 read_mode		:1;
		u32 dma_bust		:1;
		u32 rd_sram_swap	:1;
		u32 wr_sram_swap	:1;
		u32 ecc_data_inv	:1;
		u32 byte_mode		:1;
		u32 unused0		:1;
		u32 hw_ecc		:1;
		u32 auto_fdm		:1;
		u32 unused1		:2;
		u32 opmode		:3;
		u32 unused2		:17;
	} nfi_cnfg;

	struct NFI_CON {
		u32 fifo_flush	:1;
		u32 nfi_reset	:1;
		u32 unused0	:2;
		u32 srd		:1;
		u32 nob		:3;
		u32 rd_trig	:1;
		u32 wr_trig	:1;
		u32 unused1	:2;
		u32 sec_num	:4;
		u32 unused2	:16;
	} nfi_con;

	struct NFI_ADDR_NOB {
		u32 col_nob	:3;
		u32 unused0	:1;
		u32 row_nob	:3;
		u32 unused1	:25;
	} nfi_addrnob;

	struct NFI_CHK_NAND_RB {
		u32 chk_trig	:1;
		u32 unused0	:3;
		u32 timeout	:4;
		u32 unused1	:24;
	} nfi_cnrnb;

	u32 reg;
} nfi_reg;

/* STATIC VARIABLE DECLARATIONS ------------------------------------------------------ */
static SPI_NFI_CONF_T	_spi_nfi_conf_info_t;
static u8		_spi_nfi_fdm_value[_SPI_NFI_MAX_FDM_NUMBER];

/* LOCAL SUBPROGRAM BODIES------------------------------------------------------------ */

/*------------------------------------------------------------------------------------
 * FUNCTION: void SPI_NFI_TRIGGER( SPI_NFI_CONF_DMA_TRIGGER_T rw )
 * PURPOSE : Trigger a DMA read or write transfer.
 *------------------------------------------------------------------------------------
 */
static void SPI_NFI_TRIGGER(SPI_NFI_CONF_DMA_TRIGGER_T rw)
{
	if (rw == SPI_NFI_CON_DMA_TRIGGER_READ) {
		_SPI_NFI_REG16_CLRBITS(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_RD_TRIG);
		_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_RD_TRIG);
	} else {
		_SPI_NFI_REG16_CLRBITS(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_WR_TRIG);
		_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_WR_TRIG);
	}
}

/*------------------------------------------------------------------------------------
 * FUNCTION: static SPI_NFI_RTN_T spi_nfi_get_fdm_from_register( void )
 * PURPOSE : Collect the FDM (spare user) bytes the controller extracted from the
 *           spare area during the last page read.
 *------------------------------------------------------------------------------------
 */
static SPI_NFI_RTN_T spi_nfi_get_fdm_from_register(void)
{
	u32 idx, i, j, reg_addr, val;
	u8 *fdm_value;
	SPI_NFI_CONF_T *spi_nfi_conf_info_t;
	u8 spi_nfi_mapping_fdm_value[_SPI_NFI_MAX_FDM_NUMBER];

	fdm_value = (u8 *)_SPI_NFI_GET_FDM_PTR;
	spi_nfi_conf_info_t = _SPI_NFI_GET_CONF_PTR;

	_SPI_NFI_MEMSET(spi_nfi_mapping_fdm_value, 0xff, _SPI_NFI_MAX_FDM_NUMBER);
	_SPI_NFI_MEMSET(fdm_value, 0xff, _SPI_NFI_MAX_FDM_NUMBER);

	idx = 0;
	for (reg_addr = _SPI_NFI_REGS_FDM0L; reg_addr <= _SPI_NFI_REGS_FDM7M; reg_addr += 4) {
		val = _SPI_NFI_REG32_READ(reg_addr);
		spi_nfi_mapping_fdm_value[idx++] = (val & 0xFF);
		spi_nfi_mapping_fdm_value[idx++] = ((val >> 8) & 0xFF);
		spi_nfi_mapping_fdm_value[idx++] = ((val >> 16) & 0xFF);
		spi_nfi_mapping_fdm_value[idx++] = ((val >> 24) & 0xFF);
	}

	j = 0;
	for (idx = 0; idx < spi_nfi_conf_info_t->sec_num; idx++) {
		for (i = 0; i < spi_nfi_conf_info_t->fdm_num; i++) {
			fdm_value[j] = spi_nfi_mapping_fdm_value[(idx * _SPI_NFI_MAX_FDM_PER_SEC) + i];
			j++;
		}
	}

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: static SPI_NFI_RTN_T spi_nfi_set_fdm_into_register( void )
 * PURPOSE : Hand the FDM (spare user) bytes over to the controller before a page
 *           program.
 *------------------------------------------------------------------------------------
 */
static SPI_NFI_RTN_T spi_nfi_set_fdm_into_register(void)
{
	u32 idx, i, j, reg_addr, val;
	u8 *fdm_value;
	SPI_NFI_CONF_T *spi_nfi_conf_info_t;
	u8 spi_nfi_mapping_fdm_value[_SPI_NFI_MAX_FDM_NUMBER];

	fdm_value = (u8 *)_SPI_NFI_GET_FDM_PTR;
	spi_nfi_conf_info_t = _SPI_NFI_GET_CONF_PTR;

	_SPI_NFI_MEMSET(spi_nfi_mapping_fdm_value, 0xff, _SPI_NFI_MAX_FDM_NUMBER);

	j = 0;
	for (idx = 0; idx < spi_nfi_conf_info_t->sec_num; idx++) {
		for (i = 0; i < spi_nfi_conf_info_t->fdm_num; i++) {
			spi_nfi_mapping_fdm_value[(idx * _SPI_NFI_MAX_FDM_PER_SEC) + i] = fdm_value[j];
			j++;
		}
	}

	idx = 0;
	for (reg_addr = _SPI_NFI_REGS_FDM0L; reg_addr <= _SPI_NFI_REGS_FDM7M; reg_addr += 4) {
		val = 0;

		val |= (spi_nfi_mapping_fdm_value[idx++] & (0xFF));
		val |= ((spi_nfi_mapping_fdm_value[idx++] & (0xFF)) << 8);
		val |= ((spi_nfi_mapping_fdm_value[idx++] & (0xFF)) << 16);
		val |= ((spi_nfi_mapping_fdm_value[idx++] & (0xFF)) << 24);

		_SPI_NFI_REG32_WRITE(reg_addr, val);
	}

	return SPI_NFI_RTN_NO_ERROR;
}

/* EXPORTED SUBPROGRAM BODIES -------------------------------------------------------- */

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T SPI_NFI_Read_SPI_NAND_FDM( u8 *ptr_rtn_oob, u32 oob_len )
 * PURPOSE : Copy the FDM bytes collected from the controller into the caller OOB
 *           buffer.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T SPI_NFI_Read_SPI_NAND_FDM(u8 *ptr_rtn_oob, u32 oob_len)
{
	u8 *fdm_value;

	/*
	 * Only _SPI_NFI_MAX_FDM_NUMBER bytes are ever collected, the caller may
	 * ask for more than the controller can report (a device with a large
	 * spare area), so clamp instead of reading past the FDM array.
	 */
	if (oob_len > _SPI_NFI_MAX_FDM_NUMBER)
		oob_len = _SPI_NFI_MAX_FDM_NUMBER;

	spi_nfi_get_fdm_from_register();
	fdm_value = (u8 *)_SPI_NFI_GET_FDM_PTR;

	_SPI_NFI_MEMCPY(ptr_rtn_oob, fdm_value, oob_len);

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T SPI_NFI_Write_SPI_NAND_FDM( u8 *ptr_oob, u32 oob_len )
 * PURPOSE : Load the caller OOB bytes into the controller FDM registers.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T SPI_NFI_Write_SPI_NAND_FDM(u8 *ptr_oob, u32 oob_len)
{
	u8 *fdm_value;

	fdm_value = (u8 *)_SPI_NFI_GET_FDM_PTR;

	if (oob_len > _SPI_NFI_MAX_FDM_NUMBER)
		_SPI_NFI_MEMCPY(fdm_value, ptr_oob, _SPI_NFI_MAX_FDM_NUMBER);
	else
		_SPI_NFI_MEMCPY(fdm_value, ptr_oob, oob_len);

	spi_nfi_set_fdm_into_register();

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T SPI_NFI_Get_Configure( SPI_NFI_CONF_T *ptr_rtn_nfi_conf_t )
 * PURPOSE : Return the cached NFI configuration.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T SPI_NFI_Get_Configure(SPI_NFI_CONF_T *ptr_rtn_nfi_conf_t)
{
	SPI_NFI_CONF_T *ptr_spi_nfi_conf_info_t;

	ptr_spi_nfi_conf_info_t = _SPI_NFI_GET_CONF_PTR;
	_SPI_NFI_MEMCPY(ptr_rtn_nfi_conf_t, ptr_spi_nfi_conf_info_t, sizeof(SPI_NFI_CONF_T));

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T SPI_NFI_Set_Configure( SPI_NFI_CONF_T *ptr_nfi_conf_t )
 * PURPOSE : Cache the configuration and push it into the NFI registers.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T SPI_NFI_Set_Configure(SPI_NFI_CONF_T *ptr_nfi_conf_t)
{
	SPI_NFI_CONF_T *ptr_spi_nfi_conf_info_t;

	/* Store new setting */
	ptr_spi_nfi_conf_info_t = _SPI_NFI_GET_CONF_PTR;
	_SPI_NFI_MEMCPY(ptr_spi_nfi_conf_info_t, ptr_nfi_conf_t, sizeof(SPI_NFI_CONF_T));

	/* Set Auto FDM */
	if (ptr_nfi_conf_t->auto_fdm_t == SPI_NFI_CON_AUTO_FDM_Disable)
		_SPI_NFI_REG16_CLRBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_AUTO_FMT_EN);
	if (ptr_nfi_conf_t->auto_fdm_t == SPI_NFI_CON_AUTO_FDM_Enable)
		_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_AUTO_FMT_EN);

	/* Set Hardware ECC */
	if (ptr_nfi_conf_t->hw_ecc_t == SPI_NFI_CON_HW_ECC_Disable)
		_SPI_NFI_REG16_CLRBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_HW_ECC_EN);
	if (ptr_nfi_conf_t->hw_ecc_t == SPI_NFI_CON_HW_ECC_Enable)
		_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_HW_ECC_EN);

	/* Set DMA BURST */
	if (ptr_nfi_conf_t->dma_burst_t == SPI_NFI_CON_DMA_BURST_Disable)
		_SPI_NFI_REG16_CLRBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_DMA_BURST_EN);
	if (ptr_nfi_conf_t->dma_burst_t == SPI_NFI_CON_DMA_BURST_Enable)
		_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_DMA_BURST_EN);

	/* Set FDM Number */
	_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_FDM_MASK,
				   (ptr_nfi_conf_t->fdm_num) << _SPI_NFI_REGS_PAGEFMT_FDM_SHIFT);

	/* Set FDM ECC Number */
	_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_FDM_ECC_MASK,
				   (ptr_nfi_conf_t->fdm_ecc_num) << _SPI_NFI_REGS_PAGEFMT_FDM_ECC_SHIFT);

	/* Set SPARE Size */
	switch (ptr_nfi_conf_t->spare_size_t) {
	case SPI_NFI_CONF_SPARE_SIZE_16BYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_SPARE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_SPARE_16 << _SPI_NFI_REGS_PAGEFMT_SPARE_SHIFT);
		break;
	case SPI_NFI_CONF_SPARE_SIZE_26BYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_SPARE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_SPARE_26 << _SPI_NFI_REGS_PAGEFMT_SPARE_SHIFT);
		break;
	case SPI_NFI_CONF_SPARE_SIZE_27BYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_SPARE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_SPARE_27 << _SPI_NFI_REGS_PAGEFMT_SPARE_SHIFT);
		break;
	case SPI_NFI_CONF_SPARE_SIZE_28BYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_SPARE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_SPARE_28 << _SPI_NFI_REGS_PAGEFMT_SPARE_SHIFT);
		break;
	default:
		break;
	}

	/* Set PAGE Size */
	switch (ptr_nfi_conf_t->page_size_t) {
	case SPI_NFI_CONF_PAGE_SIZE_512BYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_PAGE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_PAGE_512 << _SPI_NFI_REGS_PAGEFMT_PAGE_SHIFT);
		break;
	case SPI_NFI_CONF_PAGE_SIZE_2KBYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_PAGE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_PAGE_2K << _SPI_NFI_REGS_PAGEFMT_PAGE_SHIFT);
		break;
	case SPI_NFI_CONF_PAGE_SIZE_4KBYTE:
		_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_PAGEFMT, _SPI_NFI_REGS_PAGEFMT_PAGE_MASK,
					   _SPI_NFI_REGS_PAGEFMT_PAGE_4K << _SPI_NFI_REGS_PAGEFMT_PAGE_SHIFT);
		break;
	default:
		break;
	}

	/* Set sector number */
	_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_SEC_MASK,
				   (ptr_nfi_conf_t->sec_num) << _SPI_NFI_REGS_CON_SEC_SHIFT);

	/* Enable Customer setting sector size or not */
	if (ptr_nfi_conf_t->cus_sec_size_en_t == SPI_NFI_CONF_CUS_SEC_SIZE_Disable)
		_SPI_NFI_REG32_CLRBITS(_SPI_NFI_REGS_SECCUS_SIZE, _SPI_NFI_REGS_SECCUS_SIZE_EN);
	if (ptr_nfi_conf_t->cus_sec_size_en_t == SPI_NFI_CONF_CUS_SEC_SIZE_Enable)
		_SPI_NFI_REG32_SETBITS(_SPI_NFI_REGS_SECCUS_SIZE, _SPI_NFI_REGS_SECCUS_SIZE_EN);

	/* Set Customer sector size */
	_SPI_NFI_REG32_SETMASKBITS(_SPI_NFI_REGS_SECCUS_SIZE, _SPI_NFI_REGS_SECCUS_SIZE_MASK,
				   (ptr_nfi_conf_t->sec_size) << _SPI_NFI_REGS_SECCUS_SIZE_SHIFT);

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void SPI_NFI_Reset( void )
 * PURPOSE : Reset the NFI state machine and flush the FIFO.
 *------------------------------------------------------------------------------------
 */
void SPI_NFI_Reset(void)
{
	_SPI_NFI_REG16_WRITE(_SPI_NFI_REGS_CON, _SPI_NFI_REGS_CON_RESET_VALUE);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T SPI_NFI_Init( void )
 * PURPOSE : Enable the AHB done interrupt used to poll DMA completion.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T SPI_NFI_Init(void)
{
	_SPI_NFI_REG16_SETBITS(_SPI_NFI_REGS_INTR_EN, _SPI_NFI_REGS_INTR_EN_AHB_DONE_EN);

	return SPI_NFI_RTN_NO_ERROR;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void SPI_NFI_DMA_WR_BYTE_SWAP( SPI_NFI_CONF_DMA_WR_BYTE_SWAP_T enable )
 * PURPOSE : Set DMA (flash -> SRAM) byte swap.
 *------------------------------------------------------------------------------------
 */
void SPI_NFI_DMA_WR_BYTE_SWAP(SPI_NFI_CONF_DMA_WR_BYTE_SWAP_T enable)
{
	_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_MASK,
				   enable << _SPI_NFI_REGS_CNFG_DMA_WR_SWAP_SHIFT);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void SPI_NFI_ECC_DATA_SOURCE_INV( SPI_NFI_CONF_ECC_DATA_SOURCE_INV_T enable )
 * PURPOSE : Set ECC decode invert.
 *------------------------------------------------------------------------------------
 */
void SPI_NFI_ECC_DATA_SOURCE_INV(SPI_NFI_CONF_ECC_DATA_SOURCE_INV_T enable)
{
	_SPI_NFI_REG16_SETMASKBITS(_SPI_NFI_REGS_CNFG,
				   _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_MASK,
				   enable << _SPI_NFI_REGS_CNFG_ECC_DATA_SOURCE_INV_SHIFT);
}

/*******************************************************************************
 * Parallel NAND mode
 *******************************************************************************/

/*------------------------------------------------------------------------------------
 * FUNCTION: static SPI_NFI_RTN_T SPI_NFI_START_DMA( SPI_NFI_CONF_DMA_TRIGGER_T rw )
 * PURPOSE : Trigger a transfer and poll the AHB done interrupt.
 *------------------------------------------------------------------------------------
 */
static SPI_NFI_RTN_T SPI_NFI_START_DMA(SPI_NFI_CONF_DMA_TRIGGER_T rw)
{
	u32 check_cnt = 0;
	SPI_NFI_RTN_T rtn_status = SPI_NFI_RTN_NO_ERROR;

	/* Trigger DMA active */
	SPI_NFI_TRIGGER(rw);

	/* Check DMA done or not */
	for (check_cnt = 0; check_cnt < _SPI_NFI_CHECK_DONE_MAX_TIMES; check_cnt++) {
		if ((_SPI_NFI_REG16_READ(_SPI_NFI_REGS_INTR) & (_SPI_NFI_REGS_INTR_AHB_DONE_CHECK)) != 0)
			break;
	}

	if (check_cnt == _SPI_NFI_CHECK_DONE_MAX_TIMES) {
		pr_err("[Error] %s DMA : Check AHB Done Timeout !\n",
		       (rw == SPI_NFI_CON_DMA_TRIGGER_WRITE) ? "WRITE" : "READ");
		rtn_status = SPI_NFI_RTN_CHECK_AHB_DONE_TIMEOUT;
	} else {
		udelay(1);
	}

	return rtn_status;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: static void PARALLEL_NFI_CLR_CNFG( void )
 * PURPOSE : Return the configuration register to its idle state.
 *------------------------------------------------------------------------------------
 */
static void PARALLEL_NFI_CLR_CNFG(void)
{
	nfi_reg reg;

	reg.reg = 0;

	reg.nfi_cnfg.byte_mode = 1;
	reg.nfi_cnfg.read_mode = 1;
	reg.nfi_cnfg.dma_mode = 1;
	_SPI_NFI_REG32_SETMASKBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CONF_OP_MASK,
				   (_SPI_NFI_REGS_CONF_OP_IDEL << _SPI_NFI_REGS_CONF_OP_SHIFT));
	_SPI_NFI_REG32_CLRBITS(_SPI_NFI_REGS_CNFG, reg.reg);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T PARALLEL_NFI_CHECK_INT_STATUS( u16 intr_check )
 * PURPOSE : Trigger the NAND R/B check and wait for the expected interrupt bits.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T PARALLEL_NFI_CHECK_INT_STATUS(u16 intr_check)
{
	SPI_NFI_RTN_T status = SPI_NFI_RTN_NO_ERROR;
	u32 timeout = _SPI_NFI_CHECK_DONE_MAX_TIMES;
	nfi_reg reg;

	reg.reg = _SPI_NFI_REG32_READ(_SPI_NFI_REGS_INTR_EN);
	if (((reg.reg & intr_check) == intr_check)) {
		reg.reg = 0;
		reg.nfi_cnrnb.timeout = _PARALLEL_NFI_REGS_CNRNB_TIME;
		reg.nfi_cnrnb.chk_trig = 1;

		_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_CNRNB, reg.reg);

		do {
			reg.reg = _SPI_NFI_REG32_READ(_SPI_NFI_REGS_INTR);
			timeout--;
		} while ((timeout && ((reg.reg & intr_check) != intr_check)));

		if ((reg.reg & NFI_INTR_ACCESS_LOCK))
			pr_err("Access Lock Area: intr_status=0x%x\n", reg.reg);

		if ((reg.reg & NFI_INTR_CB2R_TIMEOUT))
			pr_err("Check B2R Timeout: intr_status=0x%x\n", reg.reg);

		if (timeout == 0) {
			pr_err("Unexpected Interrupt Loss: intr_status=0x%x\n", reg.reg);
			status = SPI_NFI_RTN_WAIT_TIMEOUT;
		}
	} else {
		pr_err("No Interrupt and delay 1ms: inter_en=0x%x intr_check=0x%x\n",
		       reg.reg, intr_check);
		udelay(1000);
		pr_err("intr_status=0x%x\n", _SPI_NFI_REG32_READ(_SPI_NFI_REGS_INTR));
	}
	return status;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T PARALLEL_NFI_START_DMA( unsigned long *p_data, u8 dirt )
 * PURPOSE : Move one page between the controller and the caller buffer.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T PARALLEL_NFI_START_DMA(unsigned long *p_data, u8 dirt)
{
	SPI_NFI_RTN_T status = SPI_NFI_RTN_NO_ERROR;
	u32 timeout = _SPI_NFI_CHECK_DONE_MAX_TIMES;
	u32 len = 0, cnt = 0;
	SPI_NFI_CONF_DMA_TRIGGER_T trig = SPI_NFI_CON_DMA_TRIGGER_READ;
	u32 cnt_reg = _PARALLEL_NFI_REGS_RD_CNT;
	u32 cnt_clr = _PARALLEL_NFI_REGS_CNT_CLR_RD;
	SPI_NFI_CONF_T *spi_nfi_conf_info_t = _SPI_NFI_GET_CONF_PTR;

	if (dirt == SPI_NFI_WRITE_DATA) {
		trig = SPI_NFI_CON_DMA_TRIGGER_WRITE;
		cnt_reg = _PARALLEL_NFI_REGS_WR_CNT;
		cnt_clr = _PARALLEL_NFI_REGS_CNT_CLR_WR;
	}

	if (spi_nfi_conf_info_t->cus_sec_size_en_t == SPI_NFI_CONF_CUS_SEC_SIZE_Disable)
		len = ((_SPI_NFI_DATA_SIZE_WITH_ECC + (spi_nfi_conf_info_t->spare_size_t)) *
		       (spi_nfi_conf_info_t->sec_num));
	if (spi_nfi_conf_info_t->cus_sec_size_en_t == SPI_NFI_CONF_CUS_SEC_SIZE_Enable)
		len = ((spi_nfi_conf_info_t->sec_size) * (spi_nfi_conf_info_t->sec_num));

	_SPI_NFI_REG32_WRITE(_SPI_NFI_REGS_STRADDR, ((u32)(uintptr_t)p_data));

	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_CNT_CLR, cnt_clr);

	status = SPI_NFI_START_DMA(trig);

	if (status == SPI_NFI_RTN_NO_ERROR) {
		do {
			cnt = _SPI_NFI_REG32_READ(cnt_reg);
			timeout--;
		} while ((timeout && (cnt != len)));
	}

	if (timeout == 0) {
		pr_err("Transmission Loss: len=0x%x, cnt=0x%x\n", len, cnt);
		status = SPI_NFI_RTN_WAIT_TIMEOUT;
	}

	PARALLEL_NFI_CLR_CNFG();

	return status;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T PARALLEL_NFI_START_BYTE_READ( u8 len, u8 *p_data )
 * PURPOSE : PIO read of up to 7 bytes (status register / device id).
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T PARALLEL_NFI_START_BYTE_READ(u8 len, u8 *p_data)
{
	SPI_NFI_RTN_T status = SPI_NFI_RTN_NO_ERROR;
	u32 timeout = _SPI_NFI_CHECK_DONE_MAX_TIMES;
	nfi_reg reg;

	if ((len == 0) || (len > 7) || (p_data == NULL))
		return SPI_NFI_RTN_INVAILD_PARAM;

	reg.reg = 0;

	reg.nfi_con.srd = 1;
	reg.nfi_con.nob = len;
	_SPI_NFI_REG32_SETBITS(_SPI_NFI_REGS_CON, reg.reg);

	while ((timeout && len)) {
		reg.reg = _SPI_NFI_REG32_READ(_PARALLEL_NFI_REGS_PIO_DRDY);
		if ((reg.reg & NFI_PIO_DI_RDY)) {
			*(p_data) = ((u8)(_SPI_NFI_REG32_READ(_PARALLEL_NFI_REGS_DATAR) & 0xFF));
			len--;
			p_data++;
			timeout = _SPI_NFI_CHECK_DONE_MAX_TIMES;
		} else {
			timeout--;
		}
	}

	if (timeout == 0) {
		pr_err("NFI PIO isn't ready: pio=0x%x\n", reg.reg);
		status = SPI_NFI_RTN_WAIT_TIMEOUT;
	}

	PARALLEL_NFI_CLR_CNFG();

	return status;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_CMD_2( u8 cmd2 )
 * PURPOSE : Issue the second cycle of a two command sequence and wait for the
 *           matching completion interrupt.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_CMD_2(u8 cmd2)
{
	u16 intr_check = 0;

	_SPI_NFI_REG32_WRITE(_SPI_NFI_REGS_CMD, cmd2);

	switch (cmd2) {
	case NAND_CMD_RESET:
		intr_check = NFI_INTR_RESET_DONE;
		break;
	case NAND_CMD_ERASE2:
		intr_check = NFI_INTR_ERASE_DONE;
		break;
	case NAND_CMD_PAGEPROG:
		intr_check = NFI_INTR_WRITE_DONE;
		break;
	case NAND_CMD_READSTART:
	default:
		intr_check = NFI_INTR_BUSY_RETURN;
		break;
	}

	return PARALLEL_NFI_CHECK_INT_STATUS(intr_check);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_ADDR( u32 col_addr, u32 row_addr,
 *                                                  u8 col_nob, u8 row_nob )
 * PURPOSE : Program the column / row address registers and let the state machine
 *           emit the address cycles.
 *------------------------------------------------------------------------------------
 */
SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_ADDR(u32 col_addr, u32 row_addr, u8 col_nob, u8 row_nob)
{
	SPI_NFI_RTN_T status = SPI_NFI_RTN_NO_ERROR;
	u32 timeout = _SPI_NFI_CHECK_DONE_MAX_TIMES;
	nfi_reg reg;

	reg.reg = col_addr;
	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_COLADDR, reg.reg);

	reg.reg = row_addr;
	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_ROWADDR, reg.reg);

	/* The vendor driver reuses the register variable here and lets the row
	 * address bits beyond the (col_nob, row_nob) fields leak into ADDRNOB.
	 * Those bits are reserved, so start from a clean value instead.
	 */
	reg.reg = 0;
	reg.nfi_addrnob.col_nob = col_nob;
	reg.nfi_addrnob.row_nob = row_nob;
	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_ADDRNOB, reg.reg);

	do {
		reg.reg = _SPI_NFI_REG32_READ(_SPI_NFI_REGS_STA);
		timeout--;
	} while ((timeout && (reg.reg & (NFI_STA_CMD_MODE | NFI_STA_ADDR_MODE |
					 NFI_STA_DATAR_MODE | NFI_STA_DATAW_MODE))));

	if (timeout == 0) {
		pr_err("NFI core is busy: sta=0x%x\n", reg.reg);
		status = SPI_NFI_RTN_WAIT_TIMEOUT;
	}

	if (_SPI_NFI_REG32_READ(_PARALLEL_NFI_REGS_LOCK)) {
		reg.reg = _SPI_NFI_REG32_READ(_SPI_NFI_REGS_INTR);
		if ((reg.reg & NFI_INTR_ACCESS_LOCK)) {
			pr_err("Access Lock Area: intr_status=0x%x\n", reg.reg);

			if (!(reg.reg & NFI_INTR_RESET_DONE))
				PARALLEL_NFI_CHECK_INT_STATUS(NFI_INTR_RESET_DONE);

			pr_debug("Access Lock Area: sta=0x%x\n",
				 _SPI_NFI_REG32_READ(_SPI_NFI_REGS_STA));
			pr_debug("Access Lock Area: lock_sta=0x%x\n",
				 _SPI_NFI_REG32_READ(_PARALLEL_NFI_REGS_LOCKSTA));

			return SPI_NFI_RTN_ACCESS_LOCK;
		}
	}

	return status;
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void PARALLEL_NFI_ISSUE_CMD_1( u8 cmd1 )
 * PURPOSE : Configure the operation mode and issue the first command cycle.
 *------------------------------------------------------------------------------------
 */
void PARALLEL_NFI_ISSUE_CMD_1(u8 cmd1)
{
	nfi_reg reg;
	u8 opmode = _SPI_NFI_REGS_CONF_OP_IDEL;

	reg.reg = 0;
	switch (cmd1) {
	case NAND_CMD_READID:
	case NAND_CMD_STATUS:
		opmode = _SPI_NFI_REGS_CONF_OP_SINGLE_READ;
		reg.nfi_cnfg.byte_mode = 1;
		reg.nfi_cnfg.read_mode = 1;
		reg.nfi_cnfg.dma_mode = 0;
		break;
	case NAND_CMD_ERASE1:
		opmode = _SPI_NFI_REGS_CONF_OP_ERASE;
		reg.nfi_cnfg.byte_mode = 0;
		reg.nfi_cnfg.read_mode = 0;
		reg.nfi_cnfg.dma_mode = 0;
		break;
	case NAND_CMD_READ:
		opmode = _SPI_NFI_REGS_CONF_OP_READ;
		reg.nfi_cnfg.byte_mode = 0;
		reg.nfi_cnfg.read_mode = 1;
		reg.nfi_cnfg.dma_mode = 1;
		break;
	case NAND_CMD_SEQIN:
		opmode = _SPI_NFI_REGS_CONF_OP_PRGM;
		reg.nfi_cnfg.byte_mode = 0;
		reg.nfi_cnfg.read_mode = 0;
		reg.nfi_cnfg.dma_mode = 1;
		break;
	default:
		opmode = _SPI_NFI_REGS_CONF_OP_IDEL;
		reg.nfi_cnfg.byte_mode = 0;
		reg.nfi_cnfg.read_mode = 0;
		reg.nfi_cnfg.dma_mode = 0;
		break;
	}
	_SPI_NFI_REG32_SETMASKBITS(_SPI_NFI_REGS_CNFG, _SPI_NFI_REGS_CONF_OP_MASK,
				   (opmode << _SPI_NFI_REGS_CONF_OP_SHIFT));
	_SPI_NFI_REG32_SETBITS(_SPI_NFI_REGS_CNFG, reg.reg);

	_SPI_NFI_REG32_WRITE(_SPI_NFI_REGS_CMD, cmd1);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void PARALLEL_NFI_RESET( void )
 * PURPOSE : Reset the state machine and clear the ECC / auto FDM enables that a
 *           previous operation may have left behind.
 *------------------------------------------------------------------------------------
 */
void PARALLEL_NFI_RESET(void)
{
	nfi_reg reg;

	SPI_NFI_Reset();

	reg.reg = 0;
	reg.nfi_cnfg.hw_ecc = 1;
	reg.nfi_cnfg.auto_fdm = 1;
	_SPI_NFI_REG32_CLRBITS(_SPI_NFI_REGS_CNFG, reg.reg);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void PARALLEL_NFI_SET_TIMING( u32 timing )
 * PURPOSE : Program the AC timing register, only used on SoCs whose hardware
 *           default does not match the calibrated APB timing.
 *------------------------------------------------------------------------------------
 */
void PARALLEL_NFI_SET_TIMING(u32 timing)
{
	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_ACCCON, timing);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void PARALLEL_NFI_SET_CHIP_SELECT( u8 chip )
 * PURPOSE : Drive the chip select line of the parallel interface.
 *------------------------------------------------------------------------------------
 */
void PARALLEL_NFI_SET_CHIP_SELECT(u8 chip)
{
	_SPI_NFI_REG32_WRITE(_PARALLEL_NFI_REGS_CSEL, chip);
}

/*------------------------------------------------------------------------------------
 * FUNCTION: void PARALLEL_NFI_INIT( void )
 * PURPOSE : Bring the NFI up in parallel NAND mode and enable all interrupts.
 *------------------------------------------------------------------------------------
 */
void PARALLEL_NFI_INIT(void)
{
	PARALLEL_NFI_CLR_CNFG();

	/* Enable All Interrupt Function */
	_SPI_NFI_REG32_WRITE(_SPI_NFI_REGS_INTR_EN, NFI_INTR_ALL);
}

/* End of [spi_nfi.c] package */
