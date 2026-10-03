// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 */

#ifndef __UBOOT__
#include <linux/device.h>
#include <linux/kernel.h>
#endif
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_FISON			0x6B

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

static int fison_ooblayout_64_ecc(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	return -ERANGE;
}

static int fison_ooblayout_64_free(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	/* Reserve byte 0 for BBM, the rest is free (on-die ECC) */
	region->offset = 1;
	region->length = 63;

	return 0;
}

static const struct mtd_ooblayout_ops fison_ooblayout_64 = {
	.ecc = fison_ooblayout_64_ecc,
	.rfree = fison_ooblayout_64_free,
};

static int fison_ooblayout_128_ecc(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *region)
{
	return -ERANGE;
}

static int fison_ooblayout_128_free(struct mtd_info *mtd, int section,
				     struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	/* Reserve byte 0 for BBM, the rest is free (on-die ECC) */
	region->offset = 1;
	region->length = 127;

	return 0;
}

static const struct mtd_ooblayout_ops fison_ooblayout_128 = {
	.ecc = fison_ooblayout_128_ecc,
	.rfree = fison_ooblayout_128_free,
};

static const struct spinand_info fison_spinand_table[] = {
	/* 1Gbit 64-OOB (S variant) */
	SPINAND_INFO("CS11G0S0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x20),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_64, NULL)),
	/* 1Gbit 128-OOB (T/G variants) */
	SPINAND_INFO("CS11G0T0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x00),
		     NAND_MEMORG(1, 2048, 128, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_128, NULL)),
	SPINAND_INFO("CS11G0G0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x10),
		     NAND_MEMORG(1, 2048, 128, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_128, NULL)),
	/* 2Gbit 64-OOB (S variant) */
	SPINAND_INFO("CS11G1S0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x21),
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_64, NULL)),
	/* 2Gbit 128-OOB (T variant) */
	SPINAND_INFO("CS11G1T0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x01),
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_128, NULL)),
	/* 4Gbit 64-OOB (S variant) */
	SPINAND_INFO("CS11G2S0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x22),
		     NAND_MEMORG(1, 2048, 64, 64, 4096, 80, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_64, NULL)),
	/* 4Gbit 128-OOB (T variant) */
	SPINAND_INFO("CS11G2T0A0AA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x02),
		     NAND_MEMORG(1, 2048, 128, 64, 4096, 80, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&fison_ooblayout_128, NULL)),
};

static const struct spinand_manufacturer_ops fison_spinand_manuf_ops = {
};

const struct spinand_manufacturer fison_spinand_manufacturer = {
	.id = SPINAND_MFR_FISON,
	.name = "Fison",
	.chips = fison_spinand_table,
	.nchips = ARRAY_SIZE(fison_spinand_table),
	.ops = &fison_spinand_manuf_ops,
};
