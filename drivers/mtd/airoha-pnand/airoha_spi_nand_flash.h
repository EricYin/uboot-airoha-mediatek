/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 */

/*======================================================================================
 * MODULE NAME: nand
 * FILE NAME: airoha_spi_nand_flash.h
 * VERSION: 3.00
 * PURPOSE: Device information of the Airoha/EcoNet raw NAND backend.
 *
 * NOTES:
 *   Ported from the EcoNet vendor driver, "spi_nand_flash.h" (u-boot-2014.04-rc1
 *   drivers/misc/ecnt/flash and TF-A 2.10 plat/ecnt/common/drivers/flash).
 *   Only what the parallel (raw) NAND backend needs is carried over; the
 *   SPI-NAND-only parts of the vendor header (feature registers, OTP, Linux MTD
 *   glue, BMT/BBT bookkeeping) are dropped because this tree drives SPI-NAND
 *   through drivers/spi/airoha_snfi_spi.c + drivers/mtd/nand/spi/.
 *
 *   The extra geometry fields (ext_id, timing_setting, min_ecc_req, addr_cycle)
 *   only exist in struct SPI_NAND_FLASH_INFO_T for the parallel interface; the
 *   vendor driver guarded them with TCSUPPORT_PARALLEL_NAND, this driver is
 *   parallel NAND only, so they are unconditional.
 *======================================================================================
 */

#ifndef __AIROHA_SPI_NAND_FLASH_H__
#define __AIROHA_SPI_NAND_FLASH_H__

#include <linux/types.h>

/* MACRO DECLARATIONS ---------------------------------------------------------------- */

/* SPI NAND size definitions (shared with the device tables) */
#define _SPI_NAND_PAGE_SIZE_512				0x0200
#define _SPI_NAND_PAGE_SIZE_2KBYTE			0x0800
#define _SPI_NAND_PAGE_SIZE_4KBYTE			0x1000
#define _SPI_NAND_OOB_SIZE_64BYTE			0x40
#define _SPI_NAND_OOB_SIZE_96BYTE			0x60
#define _SPI_NAND_OOB_SIZE_120BYTE			0x78
#define _SPI_NAND_OOB_SIZE_128BYTE			0x80
#define _SPI_NAND_OOB_SIZE_224BYTE			0xE0
#define _SPI_NAND_OOB_SIZE_232BYTE			0xE8
#define _SPI_NAND_OOB_SIZE_256BYTE			0x100
#define _SPI_NAND_BLOCK_SIZE_128KBYTE			0x20000
#define _SPI_NAND_BLOCK_SIZE_256KBYTE			0x40000
#define _SPI_NAND_BLOCK_SIZE_512KBYTE			0x80000
#define _SPI_NAND_CHIP_SIZE_512MBIT			0x04000000
#define _SPI_NAND_CHIP_SIZE_1GBIT			0x08000000
#define _SPI_NAND_CHIP_SIZE_2GBIT			0x10000000
#define _SPI_NAND_CHIP_SIZE_4GBIT			0x20000000
#define _SPI_NAND_CHIP_SIZE_8GBIT			0x40000000
#define _SPI_NAND_CHIP_SIZE_16GBIT			0x80000000

/* Manufacturer IDs used by the raw NAND device table */
#define _SPI_NAND_MANUFACTURER_ID_WINBOND		0xEF
#define _SPI_NAND_MANUFACTURER_ID_SPANSION		0x01
#define _SPI_NAND_MANUFACTURER_ID_ESMT			0xC8
#define _SPI_NAND_MANUFACTURER_ID_MXIC			0xC2
#define _SPI_NAND_MANUFACTURER_ID_TOSHIBA		0x98
#define _SPI_NAND_MANUFACTURER_ID_MICRON		0x2C

/* Bitwise device features -- the upper half is reserved for the parallel
 * interface so that both device tables can share the field.
 */
#define SPI_NAND_FLASH_FEATURE_NONE			(0x00)

#define PARALLEL_NAND_FLASH_FEATURE_SHIFT		(16)
/* The device stacks two dies behind one interface, second chip select in use */
#define PARALLEL_NAND_FLASH_2CE				(0x01 << PARALLEL_NAND_FLASH_FEATURE_SHIFT)

/*
 * AC timing of the parallel interface (NFI_ACCCON).  AN7581 moved clk_apb from
 * 112.5 MHz to 150 MHz and therefore needs the wider setting; the 0x44333
 * value carried by most entries of the device table was calibrated for the
 * legacy 112.5 MHz APB and has to be replaced by the 150 MHz setting before
 * it is programmed.
 */
#define PARALLEL_NAND_FLASH_TIMING_LEGACY		0x44333
#define PARALLEL_NAND_FLASH_TIMING			0x40044326

/* TYPE DECLARATIONS ----------------------------------------------------------------- */

typedef enum {
	SPI_NAND_FLASH_RTN_NO_ERROR = 0,
	SPI_NAND_FLASH_RTN_PROBE_ERROR,
	SPI_NAND_FLASH_RTN_ALIGNED_CHECK_FAIL,
	SPI_NAND_FLASH_RTN_DETECTED_BAD_BLOCK,
	SPI_NAND_FLASH_RTN_ERASE_FAIL,
	SPI_NAND_FLASH_RTN_PROGRAM_FAIL,
	SPI_NAND_FLASH_RTN_NFI_FAIL,
	SPI_NAND_FLASH_RTN_ECC_DECODE_FAIL,
	SPI_NAND_FLASH_RTN_ENABLE_ECC_FAIL,
	SPI_NAND_FLASH_RTN_DISABLE_ECC_FAIL,
	SPI_NAND_FLASH_RTN_TIMEOUT,
	SPI_NAND_FLASH_RTN_CMD_ABORT,
	SPI_NAND_FLASH_RTN_ECC_EXCEEDED_THRESHOLD,

	SPI_NAND_FLASH_RTN_DEF_NO
} SPI_NAND_FLASH_RTN_T;

/*------------------------------------------------------------------------------------
 * struct SPI_NAND_FLASH_INFO_T - geometry of a detected device.
 *
 * The table in parallel_nand_flash_table.c is the single source of truth for
 * every field below except mfr_id / dev_id / ext_id, which are filled in by
 * parallel_nand_probe() from the device itself before the table is scanned.
 *------------------------------------------------------------------------------------
 */
struct SPI_NAND_FLASH_INFO_T {
	u8						mfr_id;
	u8						dev_id;
	const char					*ptr_name;
	u32						device_size;	/* Flash total size */
	u32						page_size;	/* Page size */
	u32						erase_size;	/* Block size */
	u32						oob_size;	/* Spare area (OOB) size */
	u32						feature;
	u32						ext_id;
	u32						timing_setting;
	u8						min_ecc_req;
	u8						addr_cycle;
	u8						soc_ecc_ability;
};

#endif /* ifndef __AIROHA_SPI_NAND_FLASH_H__ */
