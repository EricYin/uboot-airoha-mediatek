/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 */

/*======================================================================================
 * MODULE NAME: nand
 * FILE NAME: parallel_nand_flash.h
 * VERSION: 3.00
 * PURPOSE: Public interface of the parallel (raw) NAND backend.
 *
 * The parallel NAND interface is driven through the same NFI block that serves
 * SPI-NAND; only the command/address/data register protocol differs (see the
 * PARALLEL_NFI_* helpers of airoha_spi_nfi.h).
 *
 * The device table (parallel_nand_flash_table.c) is a plain read-only array:
 * unlike the SPI-NAND table it is never relocated into a BL2 optimization blob,
 * so parallel_nand_scan_flash_table() always scans the array in place.
 *======================================================================================
 */

#ifndef __PARALLEL_NAND_FLASH_H__
#define __PARALLEL_NAND_FLASH_H__

#include "airoha_spi_nand_flash.h"

/* Chip selects wired on the parallel NAND interface. */
#define PARALLEL_NAND_CHIP0		(0)
#define PARALLEL_NAND_CHIP1		(1)
#define PARALLEL_NAND_MAX_CHIPS		(2)

/*
 * Geometry of the device the backend is bound to.
 *
 * This is the structure the vendor driver called _current_flash_info_t: it is
 * filled in by parallel_nand_probe() and read by every access helper through
 * the file scope ptr_dev_info_t.  There is exactly one parallel NAND interface
 * per SoC, so the caller must pass this very structure to
 * parallel_nand_probe(), otherwise the helpers would work with an unprobed
 * geometry (in particular addr_cycle, which drives the number of address
 * cycles).
 */
extern struct SPI_NAND_FLASH_INFO_T parallel_nand_info;

/* Device table, implemented in parallel_nand_flash_table.c. */
SPI_NAND_FLASH_RTN_T parallel_nand_scan_flash_table(struct SPI_NAND_FLASH_INFO_T *ptr_rtn_device_t);

/* Backend entry points consumed by the MTD glue driver. */
SPI_NAND_FLASH_RTN_T parallel_nand_probe(struct SPI_NAND_FLASH_INFO_T *ptr_rtn_device_t);
SPI_NAND_FLASH_RTN_T parallel_nand_dma_read(u32 read_addr, u32 page_number,
					    unsigned long *p_data);
SPI_NAND_FLASH_RTN_T parallel_nand_dma_write(u32 write_addr, u32 page_number,
					     unsigned long *p_data, u32 oob_len,
					     u8 *ptr_oob);
SPI_NAND_FLASH_RTN_T parallel_nand_erase(u32 page_number);
SPI_NAND_FLASH_RTN_T parallel_nand_protocol_get_status(u8 *p_status);
SPI_NAND_FLASH_RTN_T parallel_nand_protocol_reset(void);

/* AC timing (NFI_ACCCON) the device table asks for, valid after the probe. */
u32 parallel_nand_get_timing(void);

#endif /* __PARALLEL_NAND_FLASH_H__ */
