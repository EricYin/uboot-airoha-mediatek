// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha eFuse access through the secure monitor (ATF BL31).、
 *
 * Copyright (C) 2026 Yuzhii0718 <yuzhii0718@outlook.com>
 *
 * The eFuse controller is not reachable from the non-secure world: BL31
 * exposes it with the ecnt_efuse_handler() "READ" opcode, which fills a
 * non-secure buffer with a dump of the eFuse bits.
 */

#include <cpu_func.h>
#include <linux/arm-smccc.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <malloc.h>
#include <asm/io.h>
#include <soc/airoha/efuse.h>

int airoha_efuse_smc_call(unsigned long fid, unsigned long r1,
			  unsigned long r2, unsigned long r3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fid, r1, r2, r3, 0, 0, 0, 0, &res);

	return (int)res.a0;
}

void *airoha_efuse_load(void)
{
	void *mem;

	/* A whole page of its own: the secure monitor maps the pages spanning
	 * the buffer, it must not be a slice of the malloc heap.
	 */
	mem = memalign(SZ_4K, SZ_4K);
	if (!mem)
		return NULL;

	memset(mem, 0, SZ_4K);
	flush_dcache_range((ulong)mem, (ulong)mem + SZ_4K);

	if (airoha_efuse_smc_call(AIROHA_EFUSE_SMC_FID,
				  AIROHA_EFUSE_SMC_SUBID_READ,
				  (unsigned long)virt_to_phys(mem),
				  AIROHA_EFUSE_BUF_SIZE)) {
		free(mem);
		return NULL;
	}

	/* BL31 wrote below the caches, drop whatever we may still hold */
	invalidate_dcache_range((ulong)mem, (ulong)mem + SZ_4K);

	return mem;
}

void airoha_efuse_free(void *mem)
{
	free(mem);
}

u32 airoha_efuse_get(const void *mem, u32 start_bit, u32 len)
{
	u32 bit;
	u32 data = 0;
	u8 *pdata = (u8 *)&data;
	const u8 *buf = mem;

	if (!mem)
		return AIROHA_EFUSE_INVALID_VAL;

	if (len < 1 || len > 32 ||
	    (start_bit + len) > AIROHA_EFUSE_BUF_SIZE * 8)
		return AIROHA_EFUSE_INVALID_VAL;

	/* The eFuse is read as a bit stream: any field may straddle bytes */
	for (bit = 0; bit < len; bit++) {
		u32 ofs = start_bit + bit;
		u32 d8_idx = bit >> 3;		/* bit / 8 */
		u32 d1_idx = bit & 0x7;		/* bit % 8 */
		u32 e8_idx = ofs >> 3;		/* ofs / 8 */
		u32 e1_idx = ofs & 0x7;		/* ofs % 8 */

		pdata[d8_idx] |= ((buf[e8_idx] >> e1_idx) & 0x1) << d1_idx;
	}

	return data;
}
