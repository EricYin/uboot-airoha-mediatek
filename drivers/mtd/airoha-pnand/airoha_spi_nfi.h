/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 */

/*======================================================================================
 * MODULE NAME: nand
 * FILE NAME: airoha_spi_nfi.h
 * VERSION: 3.00
 * PURPOSE: Register level interface of the Airoha/EcoNet NFI block when it is
 *          driven in parallel (raw) NAND mode.
 *
 * NOTES:
 *   Ported from the EcoNet vendor driver, "spi_nfi.h" (u-boot-2014.04-rc1
 *   drivers/misc/ecnt/flash and TF-A 2.10 plat/ecnt/common/drivers/flash).
 *   Differences against the vendor version:
 *     - the register base is resolved at runtime (airoha_nfi_base) instead of
 *       being a compile time constant, so the driver can be probed through the
 *       device tree,
 *     - the SPI-NAND / SPI-NOR specific helpers (SPI_NFI_Read_SPI_NAND_Page(),
 *       SPI_NFI_Write_SPI_NAND_page(), SPI_NFI_Read_SPI_NOR(),
 *       SPI_NFI_Write_SPI_NOR(), SPI_NFI_Regs_Dump(), nfi_type()) are not
 *       carried over: this tree already has an SPI-NAND backend in
 *       drivers/spi/airoha_snfi_spi.c and carrying the dead code would only
 *       duplicate it,
 *     - the Linux / BOOTROM / SPRAM print shims are gone, logging goes through
 *       pr_err() / pr_info() / dev_dbg().
 *
 *   The register flows (command, address and data phase, chip select, DMA
 *   trigger and interrupt polling) are unchanged.
 *======================================================================================
 */

#ifndef __AIROHA_SPI_NFI_H__
#define __AIROHA_SPI_NFI_H__

#include <linux/types.h>

/* NAND command set as understood by the NFI state machine. */
#define NAND_CMD_READ			(0x00)
#define NAND_CMD_READSTART		(0x30)
#define NAND_CMD_SEQIN			(0x80)
#define NAND_CMD_PAGEPROG		(0x10)
#define NAND_CMD_ERASE1			(0x60)
#define NAND_CMD_ERASE2			(0xD0)
#define NAND_CMD_STATUS			(0x70)
#define NAND_CMD_READID			(0x90)
#define NAND_CMD_RESET			(0xFF)

/* NFI_INTR / NFI_INTR_EN */
#define NFI_INTR_READ_DONE		(0x0001)
#define NFI_INTR_WRITE_DONE		(0x0002)
#define NFI_INTR_RESET_DONE		(0x0004)
#define NFI_INTR_ERASE_DONE		(0x0008)
#define NFI_INTR_BUSY_RETURN		(0x0010)
#define NFI_INTR_ACCESS_LOCK		(0x0020)
#define NFI_INTR_AHB_DONE		(0x0040)
#define NFI_INTR_CB2R_TIMEOUT		(0x1000)
#define NFI_INTR_ALL			(NFI_INTR_CB2R_TIMEOUT | NFI_INTR_AHB_DONE | \
					 NFI_INTR_ACCESS_LOCK | NFI_INTR_BUSY_RETURN | \
					 NFI_INTR_ERASE_DONE | NFI_INTR_RESET_DONE | \
					 NFI_INTR_WRITE_DONE)

/* NFI_STA */
#define NFI_STA_CMD_MODE		(0x0001)
#define NFI_STA_ADDR_MODE		(0x0002)
#define NFI_STA_DATAR_MODE		(0x0004)
#define NFI_STA_DATAW_MODE		(0x0008)
#define NFI_STA_ACCESS_LOCK		(0x0010)
#define NFI_STA_BUSY_RETURN		(0x0040)

/* NFI PIO data register ready */
#define NFI_PIO_DI_RDY			(0x0001)

/* Data direction of the parallel NFI DMA */
#define SPI_NFI_WRITE_DATA		(0)
#define SPI_NFI_READ_DATA		(1)

/* Register base of the NFI block, resolved by the U-Boot glue driver. */
extern unsigned long airoha_nfi_base;

/* TYPE DECLARATIONS ----------------------------------------------------------------- */

typedef enum {
	SPI_NFI_CON_AUTO_FDM_Disable = 0,
	SPI_NFI_CON_AUTO_FDM_Enable,
} SPI_NFI_CONF_AUTO_FDM_T;

typedef enum {
	SPI_NFI_CON_HW_ECC_Disable = 0,
	SPI_NFI_CON_HW_ECC_Enable,
} SPI_NFI_CONF_HW_ECC_T;

typedef enum {
	SPI_NFI_CON_DMA_TRIGGER_READ = 0,
	SPI_NFI_CON_DMA_TRIGGER_WRITE,
} SPI_NFI_CONF_DMA_TRIGGER_T;

typedef enum {
	SPI_NFI_CON_DMA_BURST_Disable = 0,
	SPI_NFI_CON_DMA_BURST_Enable,
} SPI_NFI_CONF_DMA_BURST_T;

typedef enum {
	SPI_NFI_CONF_SPARE_SIZE_16BYTE = 16,
	SPI_NFI_CONF_SPARE_SIZE_26BYTE = 26,
	SPI_NFI_CONF_SPARE_SIZE_27BYTE = 27,
	SPI_NFI_CONF_SPARE_SIZE_28BYTE = 28,
} SPI_NFI_CONF_SPARE_SIZE_T;

typedef enum {
	SPI_NFI_CONF_PAGE_SIZE_512BYTE = 512,
	SPI_NFI_CONF_PAGE_SIZE_2KBYTE = 2048,
	SPI_NFI_CONF_PAGE_SIZE_4KBYTE = 4096,
} SPI_NFI_CONF_PAGE_SIZE_T;

typedef enum {
	SPI_NFI_CONF_CUS_SEC_SIZE_Disable = 0,
	SPI_NFI_CONF_CUS_SEC_SIZE_Enable,
} SPI_NFI_CONF_CUS_SEC_SIZE_T;

typedef enum {
	SPI_NFI_CONF_DMA_WR_BYTE_SWAP_DISABLE = 0,
	SPI_NFI_CONF_DMA_WR_BYTE_SWAP_ENABLE,
} SPI_NFI_CONF_DMA_WR_BYTE_SWAP_T;

typedef enum {
	SPI_NFI_CONF_ECC_DATA_SOURCE_INV_DISABLE = 0,
	SPI_NFI_CONF_ECC_DATA_SOURCE_INV_ENABLE,
} SPI_NFI_CONF_ECC_DATA_SOURCE_INV_T;

typedef struct SPI_NFI_CONFIGURE {
	SPI_NFI_CONF_AUTO_FDM_T		auto_fdm_t;	/* auto padding oob behind data, or not */
	SPI_NFI_CONF_HW_ECC_T		hw_ecc_t;	/* enable hw ecc or not */
	SPI_NFI_CONF_DMA_BURST_T	dma_burst_t;	/* dma burst */
	u8				fdm_num;	/* value range : 0 ~ 8 */
	u8				fdm_ecc_num;	/* fdm byte under ecc protection */
	SPI_NFI_CONF_SPARE_SIZE_T	spare_size_t;	/* spare size of each sector */
	SPI_NFI_CONF_PAGE_SIZE_T	page_size_t;	/* page size (not including oob) */
	u8				sec_num;	/* number of sector, 1 ~ 8 */
	SPI_NFI_CONF_CUS_SEC_SIZE_T	cus_sec_size_en_t;
						/*
						 * Disable : sector size = 512 bytes and
						 *           the ECC function works
						 * Enable  : user defined sector size,
						 *           ECC is not applied
						 */
	u32				sec_size;	/* only used if cus_sec_size_en is enabled */
} SPI_NFI_CONF_T;

typedef enum {
	SPI_NFI_RTN_NO_ERROR = 0,
	SPI_NFI_RTN_CHECK_AHB_DONE_TIMEOUT,
	SPI_NFI_RTN_LOAD_TO_CACHE_DONE_TIMEOUT,
	SPI_NFI_RTN_READ_FROM_CACHE_DONE_TIMEOUT,
	SPI_NFI_RTN_WAIT_TIMEOUT,
	SPI_NFI_RTN_ACCESS_LOCK,
	SPI_NFI_RTN_INVAILD_PARAM,

	SPI_NFI_RTN_DEF_NO
} SPI_NFI_RTN_T;

/* EXPORTED SUBPROGRAM SPECIFICATION ------------------------------------------------- */

SPI_NFI_RTN_T SPI_NFI_Read_SPI_NAND_FDM(u8 *ptr_rtn_oob, u32 oob_len);
SPI_NFI_RTN_T SPI_NFI_Write_SPI_NAND_FDM(u8 *ptr_oob, u32 oob_len);
SPI_NFI_RTN_T SPI_NFI_Get_Configure(SPI_NFI_CONF_T *ptr_rtn_nfi_conf_t);
SPI_NFI_RTN_T SPI_NFI_Set_Configure(SPI_NFI_CONF_T *ptr_nfi_conf_t);
void SPI_NFI_Reset(void);
SPI_NFI_RTN_T SPI_NFI_Init(void);
/* Set DMA(flash -> SRAM) byte swap */
void SPI_NFI_DMA_WR_BYTE_SWAP(SPI_NFI_CONF_DMA_WR_BYTE_SWAP_T enable);
/* Set ECC decode invert */
void SPI_NFI_ECC_DATA_SOURCE_INV(SPI_NFI_CONF_ECC_DATA_SOURCE_INV_T enable);

/* Parallel NAND specific register level helpers. */
SPI_NFI_RTN_T PARALLEL_NFI_CHECK_INT_STATUS(u16 intr_check);
SPI_NFI_RTN_T PARALLEL_NFI_START_DMA(unsigned long *p_data, u8 dirt);
SPI_NFI_RTN_T PARALLEL_NFI_START_BYTE_READ(u8 len, u8 *p_data);
SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_CMD_2(u8 cmd2);
SPI_NFI_RTN_T PARALLEL_NFI_ISSUE_ADDR(u32 col_addr, u32 row_addr, u8 col_nob, u8 row_nob);
void PARALLEL_NFI_ISSUE_CMD_1(u8 cmd1);
void PARALLEL_NFI_RESET(void);
void PARALLEL_NFI_SET_TIMING(u32 timing);
void PARALLEL_NFI_SET_CHIP_SELECT(u8 chip);
void PARALLEL_NFI_INIT(void);

#endif /* ifndef __AIROHA_SPI_NFI_H__ */
