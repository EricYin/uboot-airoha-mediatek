// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: BL31 (ATF EL3 runtime) banner extraction,
 * shared by the Airoha / MediaTek board code.
 *
 * BL31 embeds the same version banner as the BL2 preloader, but the FIP
 * payload is compressed, in one of two containers:
 *   - Airoha    : LZMA-Alone stream (props 0x5D + uncompressed size),
 *                 exactly what tools/airoha_info_bl31.py decodes.
 *   - MediaTek  : the FIP comes in a plain flavour (BL31 raw) and an XZ
 *                 flavour (BL31 inside a .xz container, magic
 *                 FD 37 7A 58 5A 00).
 * A raw payload is scanned directly.
 */

#include <malloc.h>
#include <linux/string.h>

#include <failsafe/bl2.h>
#include <failsafe/bl31.h>

#if IS_ENABLED(CONFIG_LZMA)
#include <lzma/LzmaTools.h>
#endif

#if IS_ENABLED(CONFIG_XZ)
#include <xz/xz.h>
#endif

/* ------------------------------------------------------------------ */
/*  Payload container detection                                        */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_LZMA)
/*
 * LZMA-Alone stream detection: property byte 0x5D (lc=3, lp=0, pb=2)
 * followed by a dictionary size that fits in a 32-bit word.  The same
 * check is used by is_lzma_data() in tools/airoha_info_bl31.py.
 */
static bool bl31_is_lzma(const u8 *data, size_t size)
{
	u32 dict_size;

	if (size < 13 || data[0] != 0x5d)
		return false;

	dict_size = (u32)data[1] | ((u32)data[2] << 8) |
		    ((u32)data[3] << 16) | ((u32)data[4] << 24);

	return dict_size <= 0x800000;
}
#endif /* CONFIG_LZMA */

#if IS_ENABLED(CONFIG_XZ)
/* .xz container magic, see the .xz file format specification. */
static const u8 bl31_xz_magic[6] = { 0xfd, '7', 'z', 'X', 'Z', 0x00 };

static bool bl31_is_xz(const u8 *data, size_t size)
{
	return size >= sizeof(bl31_xz_magic) &&
	       !memcmp(data, bl31_xz_magic, sizeof(bl31_xz_magic));
}
#endif /* CONFIG_XZ */

/* ------------------------------------------------------------------ */
/*  Decompressors                                                      */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_XZ)
/*
 * lib/xz is the "userspace" flavour of XZ Embedded: it takes its memory
 * from xz_malloc() and its kfree()/vfree() are no-ops, so the decoder
 * state of every stream would leak.  Remember the handful of blocks the
 * decoder asks for (one xz_dec plus one xz_dec_lzma2 in single-call
 * mode) and release them once xz_dec_end() has run.
 */
#define BL31_XZ_MAX_ALLOCS	4

static void *bl31_xz_allocs[BL31_XZ_MAX_ALLOCS];
static unsigned int bl31_xz_nallocs;

/*
 * lib/xz is built with its own CRC32 implementation (-DXZ_INTERNAL_CRC32
 * in lib/xz/Makefile), whose lookup table starts out zeroed and has to be
 * initialised by the caller - without it every stream fails with
 * XZ_DATA_ERROR.  The library does not declare the function for that
 * build, so the prototype is repeated here.
 */
extern void xz_crc32_init(void);

static bool bl31_xz_ready;

void *xz_malloc(size_t size)
{
	void *p = malloc(size);

	if (p && bl31_xz_nallocs < BL31_XZ_MAX_ALLOCS)
		bl31_xz_allocs[bl31_xz_nallocs++] = p;

	return p;
}

static void bl31_xz_free_allocs(void)
{
	while (bl31_xz_nallocs)
		free(bl31_xz_allocs[--bl31_xz_nallocs]);
}

/*
 * Decompress a complete .xz stream into @out.  Single-call mode needs
 * the whole stream as input and decodes in one xz_dec_run(), using @out
 * as the LZMA2 dictionary, so @out must hold the complete result.
 *
 * Returns the number of bytes written, 0 on failure.
 */
static size_t bl31_unxz(const void *data, size_t size, void *out,
			size_t out_size)
{
	struct xz_buf buf = { 0 };
	struct xz_dec *dec;
	size_t out_len = 0;

	if (!bl31_xz_ready) {
		xz_crc32_init();
		bl31_xz_ready = true;
	}

	bl31_xz_nallocs = 0;
	dec = xz_dec_init(XZ_SINGLE, 0);
	if (!dec) {
		bl31_xz_free_allocs();
		return 0;
	}

	buf.in = data;
	buf.in_size = size;
	buf.out = out;
	buf.out_size = out_size;

	if (xz_dec_run(dec, &buf) == XZ_STREAM_END)
		out_len = buf.out_pos;

	xz_dec_end(dec);
	bl31_xz_free_allocs();

	return out_len;
}
#endif /* CONFIG_XZ */

bool failsafe_bl31_parse_banner(const void *data, size_t size,
				void *scratch, size_t scratch_size,
				struct failsafe_version_info *info)
{
	const u8 *buf = data;

	if (scratch && scratch_size) {
		size_t out_len = 0;

#if IS_ENABLED(CONFIG_XZ)
		if (bl31_is_xz(buf, size))
			out_len = bl31_unxz(buf, size, scratch, scratch_size);
#endif
#if IS_ENABLED(CONFIG_LZMA)
		if (!out_len && bl31_is_lzma(buf, size)) {
			SizeT len = scratch_size;

			if (!lzmaBuffToBuffDecompress(scratch, &len, buf, size))
				out_len = len;
		}
#endif
		if (out_len)
			return failsafe_bl2_parse_banner(scratch, out_len,
							 info);
	}

	/* Raw payload (or no decoder for its container). */
	return failsafe_bl2_parse_banner(buf, size, info);
}
