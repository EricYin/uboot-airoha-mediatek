// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * SPI NAND flash driver for TYM (同源微半导体) devices.
 */

#ifndef __UBOOT__
#include <linux/device.h>
#include <linux/kernel.h>
#endif
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_TYM			0x19

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

static int tym_ooblayout_ecc(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = (16 * section) + 3;
	region->length = 13;

	return 0;
}

static int tym_ooblayout_free(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = (16 * section) + 0;
	region->length = 3;

	return 0;
}

static const struct mtd_ooblayout_ops tym_ooblayout = {
	.ecc = tym_ooblayout_ecc,
	.rfree = tym_ooblayout_free,
};

static const struct spinand_info tym_spinand_table[] = {
	/* 1Gbit */
	SPINAND_INFO("TYM25D1GA03",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x03),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&tym_ooblayout, NULL)),
	/* 2Gbit */
	SPINAND_INFO("TYM25D2GA01",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x01),
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&tym_ooblayout, NULL)),
	SPINAND_INFO("TYM25D2GA02",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x02),
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&tym_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops tym_spinand_manuf_ops = {
};

const struct spinand_manufacturer tym_spinand_manufacturer = {
	.id = SPINAND_MFR_TYM,
	.name = "TYM",
	.chips = tym_spinand_table,
	.nchips = ARRAY_SIZE(tym_spinand_table),
	.ops = &tym_spinand_manuf_ops,
};
