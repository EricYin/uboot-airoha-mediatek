// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: FIT / legacy uImage validation and the
 * RAM-boot path, shared by the Airoha / MediaTek board code and by the
 * per-platform image validators.
 */

#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <image.h>
#include <linux/kconfig.h>
#include <linux/string.h>
#include <vsprintf.h>

#include <failsafe/image.h>

#if CONFIG_IS_ENABLED(FIT)
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	if (size < 4) {
		printf("Failsafe: '%s' image too small (%zu)\n", what, size);
		return -EINVAL;
	}

	if (fit_check_format(data, size)) {
		printf("Failsafe: '%s' image is not a valid FIT\n", what);
		return -EINVAL;
	}

	return 0;
}
#else
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	(void)data;
	(void)size;
	printf("Failsafe: '%s' image rejected (no FIT support built in)\n",
	       what);
	return -EINVAL;
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
		printf("Failsafe: '%s' legacy image too small (%zu)\n",
		       what, size);
		return -EINVAL;
	}
	if (!image_check_magic(hdr)) {
		printf("Failsafe: '%s' has no legacy uImage magic\n", what);
		return -EINVAL;
	}
	if (!image_check_hcrc(hdr)) {
		printf("Failsafe: '%s' legacy header CRC mismatch\n", what);
		return -EINVAL;
	}

	dsize = image_get_data_size(hdr);
	if (dsize > size - image_get_header_size()) {
		printf("Failsafe: '%s' legacy payload (0x%zx) exceeds "
		       "image (%zu)\n", what, dsize, size);
		return -EINVAL;
	}
	if (!image_check_dcrc(hdr)) {
		printf("Failsafe: '%s' legacy payload CRC mismatch\n", what);
		return -EINVAL;
	}

	return 0;
}

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
		printf("\n*** Failsafe: raw image - 'go 0x%lx' ***\n\n",
		       load_addr);
		snprintf(cmd, sizeof(cmd), "go 0x%lx", load_addr);
		ret = run_command(cmd, 0);
		if (ret)
			printf("Failsafe: 'go 0x%lx' failed (ret=%d)\n",
			       load_addr, ret);
		return ret;
	}
}
