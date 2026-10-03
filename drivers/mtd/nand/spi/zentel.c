// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 */

#ifndef __UBOOT__
#include <linux/device.h>
#include <linux/kernel.h>
#endif
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_ZENTEL		0xC8

static SPINAND_OP_VARIANTS(read_cache_variants,
		SPINAND_PAGE_READ_FROM_CACHE_1S_4S_4S_OP(0, 2, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_1S_1S_4S_OP(0, 1, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_1S_2S_2S_OP(0, 1, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_1S_1S_2S_OP(0, 1, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
		SPINAND_PROG_LOAD_1S_1S_4S_OP(true, 0, NULL, 0),
		SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
		SPINAND_PROG_LOAD_1S_1S_4S_OP(true, 0, NULL, 0),
		SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL, 0));

static int zentel_ooblayout_ecc(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = (16 * section) + 1;
	region->length = 7;

	return 0;
}

static int zentel_ooblayout_free(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	if (section > 7)
		return -ERANGE;

	if (section & 1) {
		/* Odd sections: 8 bytes at offset 8, 24, 40, 56 */
		region->offset = 8 * (section + 1);
		region->length = 8;
	} else {
		/* Even sections: 1 byte at offset 0, 16, 32, 48 */
		region->offset = 8 * section;
		region->length = 1;
	}

	return 0;
}

static const struct mtd_ooblayout_ops zentel_ooblayout = {
	.ecc = zentel_ooblayout_ecc,
	.rfree = zentel_ooblayout_free,
};

static const struct spinand_info zentel_spinand_table[] = {
	/* 512Mbit */
	SPINAND_INFO("A5U12A21ASC",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x20),
		     NAND_MEMORG(1, 2048, 64, 64, 512, 10, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&zentel_ooblayout, NULL)),
	/* 1Gbit */
	SPINAND_INFO("A5U1GA21BWS",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x21),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&zentel_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops zentel_spinand_manuf_ops = {
};

const struct spinand_manufacturer zentel_spinand_manufacturer = {
	.id = SPINAND_MFR_ZENTEL,
	.name = "Zentel",
	.chips = zentel_spinand_table,
	.nchips = ARRAY_SIZE(zentel_spinand_table),
	.ops = &zentel_spinand_manuf_ops,
};
