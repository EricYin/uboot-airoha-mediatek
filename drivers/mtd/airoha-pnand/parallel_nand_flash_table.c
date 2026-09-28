// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 AIROHA Inc
 *
 * Ported from the EcoNet vendor driver parallel_nand_flash_table.c
 * (u-boot-2014.04-rc1 drivers/misc/ecnt/flash and TF-A 2.10
 * plat/ecnt/common/drivers/flash).
 *
 * The obsolete GNU "field:" initializer extension was replaced by C99
 * designated initializers, the entries themselves are unchanged.
 *
 * timings:     0x44333      -> legacy 112.5 MHz APB
 *              PARALLEL_NAND_FLASH_TIMING -> 150 MHz APB (AN7581)
 * addr_cycle:  4 = 1 column + 3 row bytes, 5 = 2 column + 3 row bytes
 *
 * The vendor table also carries an "oob_free_layout" pointer used by the
 * SPI-NAND erase-statistics feature and by the BL2 optimization blob; neither
 * exists in this port, so the field is not carried over.  The user OOB layout
 * of the raw interface is built by the MTD glue from the controller FDM
 * configuration instead.
 */

/* INCLUDE FILE DECLARATIONS --------------------------------------------------------- */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "airoha_spi_nand_flash.h"
#include "parallel_nand_flash.h"

/* STATIC VARIABLE DECLARATIONS ------------------------------------------------------ */
static const struct SPI_NAND_FLASH_INFO_T parallel_nand_flash_tables[] = {
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MXIC,
		.dev_id		= 0xF1,
		.ptr_name	= "MX30LF1G18AC",
		.device_size	= _SPI_NAND_CHIP_SIZE_1GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x029580,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 4,
		.addr_cycle	= 4,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MXIC,
		.dev_id		= 0xDA,
		.ptr_name	= "MX30LF2G18AC",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x069590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MXIC,
		.dev_id		= 0xF1,
		.ptr_name	= "MX30LF1G08AA",
		.device_size	= _SPI_NAND_CHIP_SIZE_1GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0xC21D80,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 1,
		.addr_cycle	= 4,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_ESMT,
		.dev_id		= 0xDA,
		.ptr_name	= "F59L2G81A",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x449590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_ESMT,
		.dev_id		= 0xDC,
		.ptr_name	= "F59L4G81A",
		.device_size	= _SPI_NAND_CHIP_SIZE_4GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x549590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_WINBOND,
		.dev_id		= 0xDC,
		.ptr_name	= "W29N08GVSIAD",
		.device_size	= _SPI_NAND_CHIP_SIZE_8GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= PARALLEL_NAND_FLASH_2CE,
		.ext_id		= 0x549590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 1,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_WINBOND,
		.dev_id		= 0xD3,
		.ptr_name	= "W29N08GVSIAA",
		.device_size	= _SPI_NAND_CHIP_SIZE_8GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x589591,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 1,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MICRON,
		.dev_id		= 0xF1,
		.ptr_name	= "MT29F01G08ABAEA",
		.device_size	= _SPI_NAND_CHIP_SIZE_1GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x049580,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MICRON,
		.dev_id		= 0xDA,
		.ptr_name	= "MT29F02G08ABAGA",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x069590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 8,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 12,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_MICRON,
		.dev_id		= 0xD3,
		.ptr_name	= "MT29F08G08ABACA",
		.device_size	= _SPI_NAND_CHIP_SIZE_8GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_4KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_256KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_224BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x64A690,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 8,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 12,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_TOSHIBA,
		.dev_id		= 0xD3,
		.ptr_name	= "TC58NVG4S0HTA20",
		.device_size	= _SPI_NAND_CHIP_SIZE_16GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_4KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_256KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_256BYTE,
		.feature	= PARALLEL_NAND_FLASH_2CE,
		.ext_id		= 0x762691,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 8,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 12,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_TOSHIBA,
		.dev_id		= 0xDA,
		.ptr_name	= "TC58NVG1S3HTAI0",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x761590,
		.timing_setting	= 0x44333,
		.min_ecc_req	= 8,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 8,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDA,
		.ptr_name	= "S34ML02G300TFI00",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x469500,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDC,
		.ptr_name	= "S34ML04G200TFI00",
		.device_size	= _SPI_NAND_CHIP_SIZE_4GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x569590,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xF1,
		.ptr_name	= "S34ML01G1",
		.device_size	= _SPI_NAND_CHIP_SIZE_1GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x1D00,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 1,
		.addr_cycle	= 4,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDA,
		.ptr_name	= "S34ML02G1",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x449590,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 1,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDC,
		.ptr_name	= "S34ML04G1",
		.device_size	= _SPI_NAND_CHIP_SIZE_4GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_64BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x549590,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 1,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDA,
		.ptr_name	= "S34ML02G2_1",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x469590,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
	{
		.mfr_id		= _SPI_NAND_MANUFACTURER_ID_SPANSION,
		.dev_id		= 0xDA,
		.ptr_name	= "S34ML02G2_2",
		.device_size	= _SPI_NAND_CHIP_SIZE_2GBIT,
		.page_size	= _SPI_NAND_PAGE_SIZE_2KBYTE,
		.erase_size	= _SPI_NAND_BLOCK_SIZE_128KBYTE,
		.oob_size	= _SPI_NAND_OOB_SIZE_128BYTE,
		.feature	= SPI_NAND_FLASH_FEATURE_NONE,
		.ext_id		= 0x909546,
		.timing_setting	= PARALLEL_NAND_FLASH_TIMING,
		.min_ecc_req	= 4,
		.addr_cycle	= 5,
		.soc_ecc_ability	= 4,
	},
};

/* EXPORTED SUBPROGRAM BODIES -------------------------------------------------------- */

/*------------------------------------------------------------------------------------
 * FUNCTION: SPI_NAND_FLASH_RTN_T parallel_nand_scan_flash_table(
 *                                  struct SPI_NAND_FLASH_INFO_T *ptr_rtn_device_t )
 * PURPOSE : Look up a probed device by (mfr_id, dev_id, ext_id) and copy its
 *           geometry over the probed information.
 * RETURNS : SPI_NAND_FLASH_RTN_NO_ERROR on match, SPI_NAND_FLASH_RTN_PROBE_ERROR
 *           otherwise.
 *------------------------------------------------------------------------------------
 */
SPI_NAND_FLASH_RTN_T parallel_nand_scan_flash_table(struct SPI_NAND_FLASH_INFO_T *ptr_rtn_device_t)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(parallel_nand_flash_tables); i++) {
		if ((ptr_rtn_device_t->mfr_id == parallel_nand_flash_tables[i].mfr_id) &&
		    (ptr_rtn_device_t->dev_id == parallel_nand_flash_tables[i].dev_id) &&
		    (ptr_rtn_device_t->ext_id == parallel_nand_flash_tables[i].ext_id)) {
			memcpy(ptr_rtn_device_t, &parallel_nand_flash_tables[i],
			       sizeof(struct SPI_NAND_FLASH_INFO_T));

			return SPI_NAND_FLASH_RTN_NO_ERROR;
		}
	}

	return SPI_NAND_FLASH_RTN_PROBE_ERROR;
}

/* End of [parallel_nand_flash_table.c] package */
