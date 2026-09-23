// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: FIT / legacy uImage validation and the
 * RAM-boot path, shared by the Airoha / MediaTek board code and by the
 * per-platform image validators.
 */

#include <asm/global_data.h>
#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <image.h>
#include <linux/kconfig.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <vsprintf.h>

#include <failsafe/image.h>
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

#if CONFIG_IS_ENABLED(FIT)
/*
 * Strict model validation (opt-in, env 'failsafe_strict_model' == "1").
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
	const char *strict = env_get("failsafe_strict_model");
	const void *fdt = gd_fdt_blob();
	const char *board_compat;
	int off;

	/* Default ON.  Only an explicit "0" disables the strict check. */
	if (strict && !strcmp(strict, "0"))
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
