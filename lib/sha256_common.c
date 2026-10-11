// SPDX-License-Identifier: GPL-2.0+
/*
 * FIPS-180-2 compliant SHA-256 implementation
 *
 * Copyright (C) 2001-2003  Christophe Devine
 */

#ifndef USE_HOSTCC
#include <u-boot/schedule.h>
#endif /* USE_HOSTCC */
#include <string.h>
#include <u-boot/sha256.h>

#include <linux/compiler_attributes.h>

/*
 * Output = SHA-256( input buffer ). Trigger the watchdog every 'chunk_sz'
 * bytes of input processed.
 */
void sha256_csum_wd(const unsigned char *input, unsigned int ilen,
		    unsigned char *output, unsigned int chunk_sz)
{
	sha256_context ctx;
#if !defined(USE_HOSTCC) && \
	(defined(CONFIG_HW_WATCHDOG) || defined(CONFIG_WATCHDOG))
	const unsigned char *end;
	unsigned char *curr;
	int chunk;
#endif

	sha256_starts(&ctx);

#if !defined(USE_HOSTCC) && \
	(defined(CONFIG_HW_WATCHDOG) || defined(CONFIG_WATCHDOG))
	/* A chunk size of 0 means "no chunking": see md5_wd() in lib/md5.c -
	 * the loop below would otherwise never make progress.
	 */
	if (!chunk_sz)
		chunk_sz = ilen;

	curr = (unsigned char *)input;
	end = input + ilen;
	while (curr < end) {
		chunk = end - curr;
		if (chunk > chunk_sz)
			chunk = chunk_sz;
		sha256_update(&ctx, curr, chunk);
		curr += chunk;
		schedule();
	}
#else
	sha256_update(&ctx, input, ilen);
#endif

	sha256_finish(&ctx, output);
}
