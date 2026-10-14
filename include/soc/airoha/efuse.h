/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Airoha eFuse access (secure-monitor SMC).
 *
 * The eFuse controller of the Airoha ARM SoCs is only reachable from the
 * secure world, so the data is fetched through a SiP call to BL31: the
 * ecnt_efuse_handler() "READ" opcode fills a non-secure buffer with a dump of
 * the eFuse bits.  The bit offsets of the calibration fields are therefore the
 * same as in the vendor Linux header include/soc/airoha/efuse.h, which uses
 * the exact same interface.
 *
 * Copyright (C) 2026 Yuzhii0718 <yuzhii0718@outlook.com>
 */

#ifndef _SOC_AIROHA_EFUSE_H_
#define _SOC_AIROHA_EFUSE_H_

#include <linux/types.h>

/* SiP function IDs, as handled by this secure-monitor build */
#define AIROHA_EFUSE_SMC_FID		0x82000001	/* ecnt_efuse_handler() */
#define AIROHA_EFUSE_SMC_FID_PHY	0x82000303	/* phy_efuse_handler()  */

/* x1 of the eFuse call: the four bytes of "READ", little-endian */
#define AIROHA_EFUSE_SMC_SUBID_READ	0x44414552

/* Bytes the "READ" call writes into the non-secure buffer */
#define AIROHA_EFUSE_BUF_SIZE		256

/* Returned by airoha_efuse_get() for a missing / out of range field */
#define AIROHA_EFUSE_INVALID_VAL	0xffffffff

/*
 * AN7583 SerDes and PHY calibration fields, as bit offsets into the eFuse
 * dump.  The validity field gates the ones that follow it: bit 0 covers the
 * USB3 / USB2 port 0 data, bit 1 the USB2 port 1 data.
 */
#define AN7583_EFUSE_SDS_USB_VALID_LSB		34	/* [2:0] 3 bits  */
#define AN7583_EFUSE_SSUSB_TX_TERM_SEL_LSB	47	/* [1:0] 2 bits  */
#define AN7583_EFUSE_SSUSB_RX_IMP_SEL_LSB	49	/* [4:0] 5 bits  */
#define AN7583_EFUSE_USB20_P0_INTR_CTRL_LSB	89	/* [4:0] 5 bits  */
#define AN7583_EFUSE_USB20_P1_INTR_CTRL_LSB	94	/* [4:0] 5 bits  */

/**
 * airoha_efuse_smc_call() - forward a call to the secure monitor
 * @fid: SiP function ID
 * @r1: first argument (opcode for the eFuse handler)
 * @r2: second argument
 * @r3: third argument
 *
 * Return: the value the secure monitor returned in x0 (0 on success, a
 * negative error code otherwise).
 */
int airoha_efuse_smc_call(unsigned long fid, unsigned long r1,
			  unsigned long r2, unsigned long r3);

/**
 * airoha_efuse_load() - read the eFuse dump into a buffer
 *
 * The buffer is a whole page, because the secure monitor maps the page(s)
 * covering it, and is allocated with the alignment the point-to-point SMC
 * needs.  The caller has to hand it back with airoha_efuse_free().
 *
 * Return: the buffer, or NULL when the eFuse could not be read.
 */
void *airoha_efuse_load(void);

/**
 * airoha_efuse_free() - release a buffer from airoha_efuse_load()
 * @mem: buffer to free, NULL is ignored
 */
void airoha_efuse_free(void *mem);

/**
 * airoha_efuse_get() - extract a bit field from an eFuse dump
 * @mem: buffer from airoha_efuse_load()
 * @start_bit: first bit of the field
 * @len: number of bits (1..32)
 *
 * Return: the field value, or AIROHA_EFUSE_INVALID_VAL when the arguments are
 * out of range or @mem is NULL.
 */
u32 airoha_efuse_get(const void *mem, u32 start_bit, u32 len);

#endif /* _SOC_AIROHA_EFUSE_H_ */
