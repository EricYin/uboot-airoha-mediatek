// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd.
 *
 * SPI NAND flash driver for BIWIN (佰维存储) devices.
 *
 */

#ifndef __UBOOT__
#include <linux/device.h>
#include <linux/kernel.h>
#endif
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_BIWIN		0xBC

#define BIWIN_CFG_BUF_READ		BIT(3)
#define BIWIN_STATUS_ECC_HAS_BITFLIPS_T	(3 << 4)

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
		SPINAND_PROG_LOAD_1S_1S_4S_OP(false, 0, NULL, 0),
		SPINAND_PROG_LOAD_1S_1S_1S_OP(false, 0, NULL, 0));

static int bwjx08k_ooblayout_ecc(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = (16 * section) + 12;
	region->length = 4;

	return 0;
}

static int bwjx08k_ooblayout_free(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = (16 * section) + 2;
	region->length = 10;

	return 0;
}

static const struct mtd_ooblayout_ops bwjx08k_ooblayout = {
	.ecc = bwjx08k_ooblayout_ecc,
	.rfree = bwjx08k_ooblayout_free,
};

static int bwjx08u_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	if (section < 3) {
		region->offset = 12 + (16 * section);
		region->length = 8;
	} else {
		region->offset = 60;
		region->length = 4;
	}

	return 0;
}

static int bwjx08u_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	if (section > 4)
		return -ERANGE;

	switch (section) {
	case 0:
		region->offset = 0;
		region->length = 4;
		break;
	case 1:
		region->offset = 4;
		region->length = 8;
		break;
	case 2:
		region->offset = 20;
		region->length = 8;
		break;
	case 3:
		region->offset = 36;
		region->length = 8;
		break;
	case 4:
		region->offset = 52;
		region->length = 8;
		break;
	}

	return 0;
}

static const struct mtd_ooblayout_ops bwjx08u_ooblayout = {
	.ecc = bwjx08u_ooblayout_ecc,
	.rfree = bwjx08u_ooblayout_free,
};

static int bwet08u_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = 18 + (32 * section);
	region->length = 14;

	return 0;
}

static int bwet08u_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = 32 * section;
	region->length = 18;

	return 0;
}

static const struct mtd_ooblayout_ops bwet08u_ooblayout = {
	.ecc = bwet08u_ooblayout_ecc,
	.rfree = bwet08u_ooblayout_free,
};

static int bwjx08k_ecc_get_status(struct spinand_device *spinand,
				  u8 status)
{
	struct nand_device *nand = spinand_to_nand(spinand);

	switch (status & STATUS_ECC_MASK) {
	case STATUS_ECC_NO_BITFLIPS:
		return 0;

	case STATUS_ECC_UNCOR_ERROR:
		return -EBADMSG;

	case STATUS_ECC_HAS_BITFLIPS:
		return 1;

	default:
		return nand->eccreq.strength;
	}

	return -EINVAL;
}

/* Another set for the same id[2] devices in one series */
static const struct spinand_info biwin_spinand_table[] = {
	SPINAND_INFO("BWJX08K",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0xB3),
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&bwjx08k_ooblayout,
				     bwjx08k_ecc_get_status)),
	SPINAND_INFO("BWJX08U",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0xB1),
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&bwjx08u_ooblayout, NULL)),
	SPINAND_INFO("BWET08U",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0xB2),
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&bwet08u_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops biwin_spinand_manuf_ops = {
};

const struct spinand_manufacturer biwin_spinand_manufacturer = {
	.id = SPINAND_MFR_BIWIN,
	.name = "BIWIN",
	.chips = biwin_spinand_table,
	.nchips = ARRAY_SIZE(biwin_spinand_table),
	.ops = &biwin_spinand_manuf_ops,
};
