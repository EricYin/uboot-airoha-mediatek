/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 AIROHA Inc
 */

/*======================================================================================
 * MODULE NAME: nand
 * FILE NAME: airoha_spi_ecc.h
 * VERSION: 3.00
 * PURPOSE: Interface of the Airoha/EcoNet ECC block that sits next to the NFI.
 *
 * NOTES:
 *   Ported from the EcoNet vendor driver, "spi_ecc.h" (u-boot-2014.04-rc1
 *   drivers/misc/ecnt/flash and TF-A 2.10 plat/ecnt/common/drivers/flash).
 *   The register base is resolved at runtime (airoha_ecc_base) instead of being
 *   a compile time constant.
 *======================================================================================
 */

#ifndef __AIROHA_SPI_ECC_H__
#define __AIROHA_SPI_ECC_H__

#include <linux/types.h>

/* Register base of the ECC block, resolved by the U-Boot glue driver. */
extern unsigned long airoha_ecc_base;

typedef enum {
	SPI_ECC_ENCODE_DISABLE = 0,
	SPI_ECC_ENCODE_ENABLE
} SPI_ECC_ENCODE_T;

typedef enum {
	SPI_ECC_ENCODE_ABILITY_4BITS	= 4,
	SPI_ECC_ENCODE_ABILITY_6BITS	= 6,
	SPI_ECC_ENCODE_ABILITY_8BITS	= 8,
	SPI_ECC_ENCODE_ABILITY_10BITS	= 10,
	SPI_ECC_ENCODE_ABILITY_12BITS	= 12,
	SPI_ECC_ENCODE_ABILITY_14BITS	= 14,
	SPI_ECC_ENCODE_ABILITY_16BITS	= 16,
} SPI_ECC_ENCODE_ABILITY_T;

typedef enum {
	SPI_ECC_ENCODE_STATUS_IDLE = 0,
	SPI_ECC_ENCODE_STATUS_PROCESSING,
	SPI_ECC_ENCODE_STATUS_DONE,
} SPI_ECC_ENCODE_STATUS_T;

typedef struct SPI_ECC_ENCODE_CONF {
	SPI_ECC_ENCODE_T			encode_en;	/* enable encode or not */
	u32					encode_block_size;
	SPI_ECC_ENCODE_ABILITY_T		encode_ecc_abiliry;
} SPI_ECC_ENCODE_CONF_T;

typedef enum {
	SPI_ECC_DECODE_DISABLE = 0,
	SPI_ECC_DECODE_ENABLE
} SPI_ECC_DECODE_T;

typedef enum {
	SPI_ECC_DECODE_ABILITY_4BITS	= 4,
	SPI_ECC_DECODE_ABILITY_6BITS	= 6,
	SPI_ECC_DECODE_ABILITY_8BITS	= 8,
	SPI_ECC_DECODE_ABILITY_10BITS	= 10,
	SPI_ECC_DECODE_ABILITY_12BITS	= 12,
	SPI_ECC_DECODE_ABILITY_14BITS	= 14,
	SPI_ECC_DECODE_ABILITY_16BITS	= 16,
} SPI_ECC_DECODE_ABILITY_T;

typedef struct SPI_ECC_DECODE_CONF {
	SPI_ECC_DECODE_T			decode_en;	/* enable decode or not */
	u32					decode_block_size;
	SPI_ECC_DECODE_ABILITY_T		decode_ecc_abiliry;
} SPI_ECC_DECODE_CONF_T;

typedef enum {
	SPI_ECC_DECODE_STATUS_IDLE = 0,
	SPI_ECC_DECODE_STATUS_PROCESSING,
	SPI_ECC_DECODE_STATUS_DONE,
	SPI_ECC_DECODE_STATUS_TIMEOUT
} SPI_ECC_DECODE_STATUS_T;

typedef enum {
	SPI_ECC_DECODE_CORRECTION_FAIL = 0,
	SPI_ECC_DECODE_CORRECTION_OK,
} SPI_ECC_DECODE_CORRECTION_T;

typedef enum {
	SPI_ECC_RTN_NO_ERROR = 0,
	SPI_ECC_RTN_CORRECTION_ERROR,

	SPI_ECC_RTN_DEF_NO
} SPI_ECC_RTN_T;

SPI_ECC_RTN_T SPI_ECC_Encode_Check_Idle(SPI_ECC_ENCODE_STATUS_T *prt_rtn_encode_status_t);
SPI_ECC_RTN_T SPI_ECC_Encode_Check_Done(SPI_ECC_ENCODE_STATUS_T *prt_rtn_encode_status_t);
SPI_ECC_RTN_T SPI_ECC_Encode_Get_Configure(SPI_ECC_ENCODE_CONF_T *ptr_rtn_encode_conf_t);
SPI_ECC_RTN_T SPI_ECC_Encode_Set_Configure(SPI_ECC_ENCODE_CONF_T *ptr_encode_conf_t);
SPI_ECC_RTN_T SPI_ECC_Encode_Enable(void);
SPI_ECC_RTN_T SPI_ECC_Encode_Disable(void);
SPI_ECC_RTN_T SPI_ECC_Encode_Init(void);
SPI_ECC_RTN_T SPI_ECC_Decode_Check_Idle(SPI_ECC_DECODE_STATUS_T *prt_rtn_decode_status_t);
SPI_ECC_RTN_T SPI_ECC_Decode_Check_Done(SPI_ECC_DECODE_STATUS_T *prt_rtn_decode_status_t, u8 sec_num);
SPI_ECC_RTN_T SPI_ECC_DECODE_Check_Correction_Status(void);
u32 SPI_ECC_DECODE_Get_Corrected_Bits(void);
SPI_ECC_RTN_T SPI_ECC_Decode_Get_Configure(SPI_ECC_DECODE_CONF_T *ptr_rtn_decode_conf_t);
SPI_ECC_RTN_T SPI_ECC_Decode_Set_Configure(SPI_ECC_DECODE_CONF_T *ptr_decode_conf_t);
SPI_ECC_RTN_T SPI_ECC_Decode_Enable(void);
SPI_ECC_RTN_T SPI_ECC_Decode_Disable(void);
SPI_ECC_RTN_T SPI_ECC_Decode_Init(void);
u32 get_spi_ecc_deccnfg(void);

#endif /* ifndef __AIROHA_SPI_ECC_H__ */
