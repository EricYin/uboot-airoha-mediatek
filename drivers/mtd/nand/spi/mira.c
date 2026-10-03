// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 */

#ifndef __UBOOT__
#include <linux/device.h>
#include <linux/kernel.h>
#endif
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_MIRA		0xC8

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

static int mira_ooblayout_ecc(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	if (section == 0) {
		region->offset = 1;
		region->length = 7;
	} else {
		region->offset = 8 * (2 * section + 2);
		region->length = 8;
	}

	return 0;
}

static int mira_ooblayout_free(struct mtd_info *mtd, int section,
				struct mtd_oob_region *region)
{
	if (section > 4)
		return -ERANGE;

	switch (section) {
	case 0:
		region->offset = 0;
		region->length = 1;
		break;
	case 1:
		region->offset = 8;
		region->length = 8;
		break;
	case 2:
		region->offset = 24;
		region->length = 8;
		break;
	case 3:
		region->offset = 40;
		region->length = 8;
		break;
	case 4:
		region->offset = 56;
		region->length = 8;
		break;
	}

	return 0;
}

static const struct mtd_ooblayout_ops mira_ooblayout = {
	.ecc = mira_ooblayout_ecc,
	.rfree = mira_ooblayout_free,
};

static const struct spinand_info mira_spinand_table[] = {
	SPINAND_INFO("PSU1GS20BN",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x21),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&mira_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops mira_spinand_manuf_ops = {
};

const struct spinand_manufacturer mira_spinand_manufacturer = {
	.id = SPINAND_MFR_MIRA,
	.name = "Mira",
	.chips = mira_spinand_table,
	.nchips = ARRAY_SIZE(mira_spinand_table),
	.ops = &mira_spinand_manuf_ops,
};
