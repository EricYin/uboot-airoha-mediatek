// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: FIT / legacy uImage validation and the
 * RAM-boot path, shared by the Airoha / MediaTek board code and by the
 * per-platform image validators.
 */

#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <image.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <malloc.h>
#include <vsprintf.h>

#include <failsafe/image.h>
#include <failsafe/compress.h>
#include <failsafe/cprint.h>
#include <failsafe/error.h>

#if CONFIG_IS_ENABLED(FIT)
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	if (size < 4) {
		return failsafe_error(-EINVAL, "'%s' image too small (%zu)",
			what, size);
	}

	if (fit_check_format(data, size)) {
		return failsafe_error(-EINVAL, "'%s' image is not a valid FIT",
			what);
	}

	return 0;
}
#else
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	(void)data;
	(void)size;
	return failsafe_error(-EINVAL, "'%s' image rejected (no FIT support "
		"built in)", what);
}
#endif

bool failsafe_image_is_legacy(const void *data, size_t size)
{
	return size >= image_get_header_size() &&
	       image_check_magic((const struct legacy_img_hdr *)data);
}

int failsafe_image_validate_legacy(const void *data, size_t size,
				   const char *what)
{
	const struct legacy_img_hdr *hdr = data;
	size_t dsize;

	if (size < image_get_header_size()) {
		return failsafe_error(-EINVAL,
			"'%s' legacy image too small (%zu)", what, size);
	}
	if (!image_check_magic(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' has no legacy uImage magic", what);
	}
	if (!image_check_hcrc(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' legacy header CRC mismatch", what);
	}

	dsize = image_get_data_size(hdr);
	if (dsize > size - image_get_header_size()) {
		return failsafe_error(-EINVAL, "'%s' legacy payload (0x%zx) "
			"exceeds image (%zu)", what, dsize, size);
	}
	if (!image_check_dcrc(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' legacy payload CRC mismatch", what);
	}

	return 0;
}

int failsafe_image_validate_firmware(const void *data, size_t size,
				     const char *what)
{
	if (failsafe_image_validate_fit(data, size, what))
		return -EINVAL;

	/* Opt-in strict board-model gate (env 'failsafe_strict_model'). */
	return failsafe_firmware_check_model(data, size, what);
}

/* ------------------------------------------------------------------ */
/*  Strict board-model validation (env 'failsafe_strict_model')        */
/* ------------------------------------------------------------------ */

/* Number of root 'compatible' entries compared; mirrors the reference
 * implementation (struct compat_list in the MediaTek tree). */
#define FAILSAFE_MODEL_COMPAT_MAX	10

/*
 * Strict board-model validation is on by default: only an explicit "0"
 * in the environment disables it.  It rejects a boot image that was
 * built for a different board than the one this recovery runs on.
 */
#if CONFIG_IS_ENABLED(FIT) || CONFIG_IS_ENABLED(OF_LIBFDT)
static bool failsafe_strict_model_enabled(void)
{
	const char *strict = env_get("failsafe_strict_model");

	return !(strict && !strcmp(strict, "0"));
}
#endif

#if CONFIG_IS_ENABLED(FIT)
/*
 * Strict model validation of a firmware (FIT) image.
 *
 * The recovery runs on one specific board, identified by the 'compatible'
 * string in this U-Boot's control device tree (e.g. "nokia,xg-040g-md").
 * In strict mode a FIT firmware is only accepted when it declares a
 * 'compatible' that matches the running board - using the same matching
 * logic U-Boot itself uses to pick a FIT configuration
 * (fit_conf_find_compat()).  This blocks, for example, an image built
 * for a different board from being flashed on this one.
 *
 * The check is a no-op (returns 0) when:
 *   - the switch is off,
 *   - the running board DT has no 'compatible' (cannot decide),
 *   - the image is not a FIT (nothing to match against; the structural
 *     validator handles non-FIT firmware).
 */
int failsafe_firmware_check_model(const void *data, size_t size,
				  const char *what)
{
	DECLARE_GLOBAL_DATA_PTR;
	const void *fdt = gd_fdt_blob();
	const char *board_compat;
	int off;

	if (!failsafe_strict_model_enabled())
		return 0;

	if (!fdt || fdt_check_header(fdt)) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT unavailable)", what);
		return 0;
	}

	board_compat = fdt_getprop(fdt, 0, "compatible", NULL);
	if (!board_compat) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT has no compatible)", what);
		return 0;
	}

	if (fit_check_format(data, size)) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(not a FIT image)", what);
		return 0;
	}

	off = fit_conf_find_compat(data, fdt);
	if (off < 0) {
		return failsafe_error(-EINVAL, "%s rejected by strict-model "
			"check (firmware does not match board '%s')", what,
			board_compat);
	}

	cprintln(SUCCESS, "Failsafe: %s strict-model check OK (matches "
		 "board '%s')", what, board_compat);
	return 0;
}
#else
int failsafe_firmware_check_model(const void *data, size_t size,
				  const char *what)
{
	(void)data;
	(void)size;
	(void)what;
	return 0;
}
#endif

#if CONFIG_IS_ENABLED(OF_LIBFDT)
/*
 * Strict model validation of a U-Boot image (BL33).
 *
 * A FIP carries its own U-Boot, and that U-Boot ships the control device
 * tree the board boots with.  In strict mode a boot chain is only
 * accepted when the U-Boot inside it declares a 'compatible' that
 * matches the running board - the rule the reference MediaTek tree
 * applies to the "u-boot" entry of a FIP (fip_check_uboot_data()).  This
 * blocks, for example, the boot chain of a different board model from
 * being flashed on this one.
 *
 * The check is a no-op (returns 0) when:
 *   - the switch is off,
 *   - the running board DT has no 'compatible' (cannot decide),
 *   - the U-Boot payload carries no control device tree, or stores it in
 *     a container this build cannot expand (nothing to compare).
 */

/* Does a plausible FDT header start at @p, @avail bytes being available? */
static bool failsafe_uboot_fdt_header_ok(const u8 *p, size_t avail)
{
	u32 totalsize;

	if (avail < 8)
		return false;

	if (get_unaligned_be32(p) != FDT_MAGIC)
		return false;

	totalsize = get_unaligned_be32(p + 4);

	return totalsize >= 16 && totalsize <= avail;
}

/*
 * Locate the control device tree inside a U-Boot image.  The device tree
 * is either linked into the binary (OF_EMBED) or appended to it
 * (OF_SEPARATE, the default), so the search walks back from the end and
 * takes the first plausible FDT header that really carries a
 * 'compatible' - mirrors locate_fdt_in_uboot() of the reference tree.
 */
static const void *failsafe_uboot_find_fdt(const void *data, size_t size)
{
	const u8 *buf = data;
	size_t off;

	if (size < 8)
		return NULL;

	for (off = size - 8; off; off--) {
		const void *fdt = buf + off;
		int len;

		if (!failsafe_uboot_fdt_header_ok(fdt, size - off))
			continue;

		if (!fdt_getprop(fdt, 0, "compatible", &len))
			continue;

		return fdt;
	}

	return NULL;
}

/*
 * Locate the device tree of a U-Boot payload taken from a FIP.  The
 * payload is stored compressed on some builds (LZMA-Alone on Airoha, an
 * .xz container on MediaTek with FIP compression enabled), in which case
 * it is expanded into a scratch buffer that the caller frees through
 * @scratch.  Returns NULL (and reports why) when no device tree could be
 * found.
 */
static const void *failsafe_uboot_locate_fdt(const void *data, size_t size,
					     void **scratch, const char *what)
{
	const void *fdt;
	const char *container;
	size_t out_len = 0;
	int ret;
	void *buf;

	/* Uncompressed U-Boot: the device tree is right there. */
	fdt = failsafe_uboot_find_fdt(data, size);
	if (fdt)
		return fdt;

	/* The payload is compressed at least in appearance; failing to
	 * expand it (no decoder built in, out of memory, ...) leaves
	 * nothing to compare, which the console should say honestly. */
	container = failsafe_payload_container(data, size);

	buf = malloc(FAILSAFE_COMPRESS_SCRATCH_SIZE);
	if (!buf) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(out of memory)", what);
		return NULL;
	}

	ret = failsafe_payload_decompress(data, size, buf,
					  FAILSAFE_COMPRESS_SCRATCH_SIZE,
					  &out_len);
	if (ret) {
		free(buf);
		if (container)
			cprintln(CAUTION, "Failsafe: %s strict-model check "
				 "skipped (compressed U-Boot (%s) could not "
				 "be expanded)", what, container);
		else
			cprintln(CAUTION, "Failsafe: %s strict-model check "
				 "skipped (no control device tree in the "
				 "U-Boot image)", what);
		return NULL;
	}

	fdt = failsafe_uboot_find_fdt(buf, out_len);
	if (!fdt) {
		free(buf);
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(no control device tree in the decompressed "
			 "U-Boot)", what);
		return NULL;
	}

	*scratch = buf;
	return fdt;
}

/* Read the root 'compatible' list of @fdt into @compats. */
static int failsafe_model_read_compat(const void *fdt, const char *compats[],
				      int max)
{
	const char *prop, *p, *end;
	int len, count = 0;

	if (!fdt || fdt_check_header(fdt))
		return -EINVAL;

	prop = fdt_getprop(fdt, 0, "compatible", &len);
	if (!prop || len < 2)
		return -ENOENT;

	end = prop + len;
	for (p = prop; p < end && *p && count < max; p += strlen(p) + 1)
		compats[count++] = p;

	return count ? count : -ENOENT;
}

/*
 * Are two root 'compatible' lists compatible?  Two device trees match
 * when the shorter list is a prefix of the longer one (the longer list
 * is the more specific description of the same board), which is exactly
 * the rule the reference implementation applies.
 */
static bool failsafe_model_compat_match(const char *a[], int a_count,
					const char *b[], int b_count)
{
	int i, count = a_count < b_count ? a_count : b_count;

	for (i = 0; i < count; i++)
		if (strcmp(a[i], b[i]))
			return false;

	return true;
}

int failsafe_uboot_check_model(const void *data, size_t size,
			       const char *what)
{
	DECLARE_GLOBAL_DATA_PTR;
	const char *board[FAILSAFE_MODEL_COMPAT_MAX];
	const char *target[FAILSAFE_MODEL_COMPAT_MAX];
	const void *fdt;
	void *scratch = NULL;
	int board_count, target_count;

	if (!failsafe_strict_model_enabled())
		return 0;

	board_count = failsafe_model_read_compat(gd_fdt_blob(), board,
						 (int)ARRAY_SIZE(board));
	if (board_count < 0) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT has no compatible)", what);
		return 0;
	}

	/* Reports on its own why it could not produce a device tree. */
	fdt = failsafe_uboot_locate_fdt(data, size, &scratch, what);
	if (!fdt) {
		free(scratch);
		return 0;
	}

	target_count = failsafe_model_read_compat(fdt, target,
						  (int)ARRAY_SIZE(target));
	free(scratch);

	if (target_count < 0) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(U-Boot DT has no compatible)", what);
		return 0;
	}

	if (!failsafe_model_compat_match(board, board_count, target,
					 target_count)) {
		return failsafe_error(-EINVAL, "%s rejected by strict-model "
			"check (built for '%s', board is '%s')", what,
			target[0], board[0]);
	}

	cprintln(SUCCESS, "Failsafe: %s strict-model check OK (matches "
		 "board '%s')", what, board[0]);
	return 0;
}
#else
int failsafe_uboot_check_model(const void *data, size_t size,
			       const char *what)
{
	(void)data;
	(void)size;
	(void)what;
	return 0;
}
#endif /* CONFIG_IS_ENABLED(OF_LIBFDT) */

int failsafe_boot_image_from_mem(ulong data_load_addr, size_t image_size,
				 ulong load_fallback)
{
	const char *loadaddr = env_get("loadaddr");
	const char *bootconf = env_get("bootconf");
	ulong load_addr;
	char cmd[96];
	int ret;

	if (loadaddr && loadaddr[0])
		load_addr = simple_strtoul(loadaddr, NULL, 0);
	else
		load_addr = load_fallback;

	if (load_addr != data_load_addr)
		memcpy((void *)load_addr, (const void *)data_load_addr,
		       image_size);

	switch (genimg_get_format((const void *)load_addr)) {
	case IMAGE_FORMAT_FIT:
	case IMAGE_FORMAT_LEGACY:
		/* Parseable boot image: use bootm (the initramfs path). */
		if (bootconf && bootconf[0])
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx#%s",
				 load_addr, bootconf);
		else
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx", load_addr);

		return run_command(cmd, 0);

	default:
		/* Raw binary: jump straight to it with "go".  The image was
		 * copied with memcpy() while caches were active, so make sure
		 * it actually reached RAM and is not served from a stale
		 * i-cache line before executing.
		 */
		flush_cache(load_addr, image_size);
		invalidate_icache_all();
		cprintln(NORMAL, "\n*** Failsafe: raw image - 'go 0x%lx' ***\n",
			 load_addr);
		snprintf(cmd, sizeof(cmd), "go 0x%lx", load_addr);
		ret = run_command(cmd, 0);
		if (ret)
			return failsafe_error(ret, "'go 0x%lx' failed",
				load_addr);
		return 0;
	}
}
