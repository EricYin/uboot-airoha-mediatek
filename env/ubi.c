// SPDX-License-Identifier: GPL-2.0+
/*
 * (c) Copyright 2012 by National Instruments,
 *        Joe Hershberger <joe.hershberger@ni.com>
 */

#include <asm/global_data.h>

#include <command.h>
#include <env.h>
#include <env_internal.h>
#include <errno.h>
#include <malloc.h>
#include <memalign.h>
#include <search.h>
#include <ubi_uboot.h>
#include <vsprintf.h>
#undef crc32

#ifndef CONFIG_ENV_UBI_EXTRA_VOLUMES
#define CONFIG_ENV_UBI_EXTRA_VOLUMES ""
#endif

#define _QUOTE(x) #x
#define QUOTE(x) _QUOTE(x)

#if (CONFIG_ENV_UBI_VID_OFFSET == 0)
 #define UBI_VID_OFFSET NULL
#else
 #define UBI_VID_OFFSET QUOTE(CONFIG_ENV_UBI_VID_OFFSET)
#endif

DECLARE_GLOBAL_DATA_PTR;

#if CONFIG_ENV_REDUNDANT
#define ENV_UBI_VOLUME_REDUND CONFIG_ENV_UBI_VOLUME_REDUND
#else
#define ENV_UBI_VOLUME_REDUND "invalid"
#endif

#ifdef CONFIG_ENV_REDUNDANT
static int env_ubi_save(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(env_t, env_new, 1);
	int ret;

	ret = env_export(env_new);
	if (ret)
		return ret;

	if (ubi_part(CONFIG_ENV_UBI_PART, UBI_VID_OFFSET)) {
		printf("\n** Cannot find mtd partition \"%s\"\n",
		       CONFIG_ENV_UBI_PART);
		return 1;
	}

	if (gd->env_valid == ENV_VALID) {
		puts("Writing to redundant UBI... ");
		if (ubi_volume_write(CONFIG_ENV_UBI_VOLUME_REDUND,
				     (void *)env_new, 0, CONFIG_ENV_SIZE)) {
			printf("\n** Unable to write env to %s:%s **\n",
			       CONFIG_ENV_UBI_PART,
			       CONFIG_ENV_UBI_VOLUME_REDUND);
			return 1;
		}
	} else {
		puts("Writing to UBI... ");
		if (ubi_volume_write(CONFIG_ENV_UBI_VOLUME,
				     (void *)env_new, 0, CONFIG_ENV_SIZE)) {
			printf("\n** Unable to write env to %s:%s **\n",
			       CONFIG_ENV_UBI_PART,
			       CONFIG_ENV_UBI_VOLUME);
			return 1;
		}
	}

	puts("done\n");

	gd->env_valid = gd->env_valid == ENV_VALID ? ENV_REDUND : ENV_VALID;

	return 0;
}
#else /* ! CONFIG_ENV_REDUNDANT */
static int env_ubi_save(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(env_t, env_new, 1);
	int ret;

	ret = env_export(env_new);
	if (ret)
		return ret;

	if (ubi_part(CONFIG_ENV_UBI_PART, UBI_VID_OFFSET)) {
		printf("\n** Cannot find mtd partition \"%s\"\n",
		       CONFIG_ENV_UBI_PART);
		return 1;
	}

	if (ubi_volume_write(CONFIG_ENV_UBI_VOLUME, (void *)env_new, 0,
			     CONFIG_ENV_SIZE)) {
		printf("\n** Unable to write env to %s:%s **\n",
		       CONFIG_ENV_UBI_PART, CONFIG_ENV_UBI_VOLUME);
		return 1;
	}

	puts("done\n");
	return 0;
}
#endif /* CONFIG_ENV_REDUNDANT */

static int env_ubi_volume_create(const char *volume)
{
	bool dynamic = !IS_ENABLED(CONFIG_ENV_UBI_VOLUME_STATIC);
	struct ubi_volume *vol;
	int ret;

	vol = ubi_find_volume(volume);
	if (vol)
		return 0;

	ret = ubi_create_vol(volume, CONFIG_ENV_SIZE, dynamic, UBI_VOL_NUM_AUTO,
			     false);
	if (ret)
		printf("Failed to create environment volume '%s'\n", volume);

	return ret;
}

/*
 * Create user defined UBI volumes if they do not exist yet.
 *
 * The volume list is configured via CONFIG_ENV_UBI_EXTRA_VOLUMES using an
 * mtdparts-like syntax, e.g. "-(factory),1M(misc)".  A size of '-' means
 * the volume should occupy all remaining free space.  Fixed-size volumes
 * are created in a first pass, and auto-sized ('-') volumes in a second
 * pass, so that the auto-sized volumes can absorb the space left over.
 */
static void env_ubi_extra_volume_create(void)
{
	char buf[sizeof(CONFIG_ENV_UBI_EXTRA_VOLUMES)];
	bool dynamic = !IS_ENABLED(CONFIG_ENV_UBI_VOLUME_STATIC);
	char *p, *entry;
	int pass;

	if (!CONFIG_ENV_UBI_EXTRA_VOLUMES[0])
		return;

	for (pass = 0; pass < 2; pass++) {
		strcpy(buf, CONFIG_ENV_UBI_EXTRA_VOLUMES);
		p = buf;

		while ((entry = strsep(&p, ",")) != NULL) {
			char *name, *end;
			int64_t size;
			bool auto_size;
			struct ubi_volume *vol;
			int ret;

			if (*entry == '\0')
				continue;

			name = strchr(entry, '(');
			if (!name) {
				printf("env: invalid extra volume entry '%s'\n",
				       entry);
				continue;
			}
			*name++ = '\0';

			end = strchr(name, ')');
			if (!end || end == name) {
				printf("env: invalid extra volume entry '%s'\n",
				       entry);
				continue;
			}
			*end = '\0';

			auto_size = entry[0] == '-';
			/* fixed-size in pass 0, auto-sized in pass 1 */
			if (auto_size != (pass == 1))
				continue;

			if (auto_size) {
				size = -1;
			} else {
				const char *endp;

				size = simple_strtoull(entry, (char **)&endp, 0);
				if (endp == entry || *endp) {
					printf("env: invalid size '%s' for "
					       "volume '%s'\n", entry, name);
					continue;
				}
			}

			if (size == 0) {
				printf("env: invalid size '%s' for volume "
				       "'%s'\n", entry, name);
				continue;
			}

			vol = ubi_find_volume(name);
			if (vol)
				continue;

			ret = ubi_create_vol(name, size, dynamic,
					     UBI_VOL_NUM_AUTO, false);
			if (ret)
				printf("Failed to create extra UBI volume "
				       "'%s'\n", name);
		}
	}
}

/*
 * Give the names of the extra volumes to a caller that has to keep their
 * contents across a rebuild of the UBI device (the failsafe UBI page):
 * those volumes hold board data - the factory MAC in "ri", the radio
 * calibration in "art" - that is recreated empty otherwise.
 *
 * @names receives the names comma separated ("bosa,ri,art"), or an empty
 * string when the list is: the "size(name)" syntax is the environment
 * driver's, so it is parsed here instead of a second time by the caller.
 *
 * Return: 0, or -ENOSPC when the names do not fit in @names.
 */
int env_ubi_extra_volume_names(char *names, size_t sz)
{
	char buf[sizeof(CONFIG_ENV_UBI_EXTRA_VOLUMES)];
	char *p, *entry;

	if (!sz)
		return -ENOSPC;

	names[0] = '\0';

	if (!CONFIG_ENV_UBI_EXTRA_VOLUMES[0])
		return 0;

	strcpy(buf, CONFIG_ENV_UBI_EXTRA_VOLUMES);
	p = buf;

	while ((entry = strsep(&p, ",")) != NULL) {
		char *name, *end;

		if (*entry == '\0')
			continue;

		name = strchr(entry, '(');
		if (!name)
			continue;
		name++;

		end = strchr(name, ')');
		if (!end || end == name)
			continue;
		*end = '\0';

		if (names[0] && strlcat(names, ",", sz) >= sz)
			return -ENOSPC;

		if (strlcat(names, name, sz) >= sz)
			return -ENOSPC;
	}

	return 0;
}

/*
 * Create the environment volumes (and the extra ones) if they are missing.
 *
 * This is what env_ubi_load() does on every boot when
 * CONFIG_ENV_UBI_VOLUME_CREATE is enabled.  It is exported because a device
 * whose UBI device was rebuilt at run time - see the failsafe UBI page -
 * has to give the environment a home again before anything can be saved
 * into it: env_save() writes the volumes, it does not create them.
 *
 * With CONFIG_ENV_REDUNDANT both volumes are created, and neither call may
 * be skipped when the other one succeeds: env_save() writes whichever copy
 * gd->env_valid points at, and that is the redundant one while the primary
 * copy is the valid one - a device whose second volume was never created
 * therefore fails its first save with "Volume <redund> not found".
 * env_ubi_load() makes the same two calls unconditionally.
 *
 * Note that the volumes are created here even when
 * CONFIG_ENV_UBI_VOLUME_CREATE is off: that option decides whether the
 * *boot* path may format a device that has no environment, while a caller
 * that has just erased the UBI device knows the environment is gone and
 * wants it back.
 *
 * Returns 0 when an environment volume is usable, -ENODEV otherwise.
 */
#if IS_ENABLED(CONFIG_ENV_UBI_VOLUME_CREATE)
int env_ubi_volumes_create(void)
{
#ifdef CONFIG_ENV_REDUNDANT
	int create1_fail, create2_fail;

	create1_fail = env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME);
	create2_fail = env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME_REDUND);

	if (create1_fail && create2_fail)
		return -ENODEV;
#else
	if (env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME))
		return -ENODEV;
#endif

	env_ubi_extra_volume_create();

	return 0;
}
#endif /* CONFIG_ENV_UBI_VOLUME_CREATE */

#ifdef CONFIG_ENV_REDUNDANT
static int env_ubi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, env1_buf, CONFIG_ENV_SIZE);
	ALLOC_CACHE_ALIGN_BUFFER(char, env2_buf, CONFIG_ENV_SIZE);
	int read1_fail, read2_fail, create1_fail = 0, create2_fail = 0;
	env_t *tmp_env1, *tmp_env2;

	/*
	 * In case we have restarted u-boot there is a chance that buffer
	 * contains old environment (from the previous boot).
	 * If UBI volume is zero size, ubi_volume_read() doesn't modify the
	 * buffer.
	 * We need to clear buffer manually here, so the invalid CRC will
	 * cause setting default environment as expected.
	 */
	memset(env1_buf, 0x0, CONFIG_ENV_SIZE);
	memset(env2_buf, 0x0, CONFIG_ENV_SIZE);

	tmp_env1 = (env_t *)env1_buf;
	tmp_env2 = (env_t *)env2_buf;

	if (ubi_part(CONFIG_ENV_UBI_PART, UBI_VID_OFFSET)) {
		printf("\n** Cannot find mtd partition \"%s\"\n",
		       CONFIG_ENV_UBI_PART);
		env_set_default(NULL, 0);
		return -EIO;
	}

	if (IS_ENABLED(CONFIG_ENV_UBI_VOLUME_CREATE)) {
		create1_fail = env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME);
		create2_fail = env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME_REDUND);
		if (create1_fail && create2_fail) {
			env_set_default(NULL, 0);
			return -ENODEV;
		}
		env_ubi_extra_volume_create();
	}

	if (!create1_fail) {
		read1_fail = ubi_volume_read(CONFIG_ENV_UBI_VOLUME, tmp_env1, 0,
					     CONFIG_ENV_SIZE);
		if (read1_fail)
			printf("\n** Unable to read env from %s:%s **\n",
			       CONFIG_ENV_UBI_PART, CONFIG_ENV_UBI_VOLUME);
	} else {
		read1_fail = create1_fail;
	}

	if (!create2_fail) {
		read2_fail = ubi_volume_read(CONFIG_ENV_UBI_VOLUME_REDUND,
					     tmp_env2, 0, CONFIG_ENV_SIZE);
		if (read2_fail)
			printf("\n** Unable to read redundant env from %s:%s **\n",
			       CONFIG_ENV_UBI_PART,
			       CONFIG_ENV_UBI_VOLUME_REDUND);
	} else {
		read2_fail = create2_fail;
	}

	return env_import_redund((char *)tmp_env1, read1_fail, (char *)tmp_env2,
				 read2_fail, H_EXTERNAL);
}
#else /* ! CONFIG_ENV_REDUNDANT */
static int env_ubi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, buf, CONFIG_ENV_SIZE);

	/*
	 * In case we have restarted u-boot there is a chance that buffer
	 * contains old environment (from the previous boot).
	 * If UBI volume is zero size, ubi_volume_read() doesn't modify the
	 * buffer.
	 * We need to clear buffer manually here, so the invalid CRC will
	 * cause setting default environment as expected.
	 */
	memset(buf, 0x0, CONFIG_ENV_SIZE);

	if (ubi_part(CONFIG_ENV_UBI_PART, UBI_VID_OFFSET)) {
		printf("\n** Cannot find mtd partition \"%s\"\n",
		       CONFIG_ENV_UBI_PART);
		env_set_default(NULL, 0);
		return -EIO;
	}

	if (IS_ENABLED(CONFIG_ENV_UBI_VOLUME_CREATE)) {
		if (env_ubi_volume_create(CONFIG_ENV_UBI_VOLUME)) {
			env_set_default(NULL, 0);
			return -ENODEV;
		}
		env_ubi_extra_volume_create();
	}

	if (ubi_volume_read(CONFIG_ENV_UBI_VOLUME, buf, 0, CONFIG_ENV_SIZE)) {
		printf("\n** Unable to read env from %s:%s **\n",
		       CONFIG_ENV_UBI_PART, CONFIG_ENV_UBI_VOLUME);
		env_set_default(NULL, 0);
		return -EIO;
	}

	return env_import(buf, 1, H_EXTERNAL);
}
#endif /* CONFIG_ENV_REDUNDANT */

static int env_ubi_erase(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, env_buf, CONFIG_ENV_SIZE);
	int ret = 0;

	if (ubi_part(CONFIG_ENV_UBI_PART, UBI_VID_OFFSET)) {
		printf("\n** Cannot find mtd partition \"%s\"\n",
		       CONFIG_ENV_UBI_PART);
		return 1;
	}

	memset(env_buf, 0x0, CONFIG_ENV_SIZE);

	if (ubi_volume_write(CONFIG_ENV_UBI_VOLUME,
			     (void *)env_buf, 0, CONFIG_ENV_SIZE)) {
		printf("\n** Unable to erase env to %s:%s **\n",
		       CONFIG_ENV_UBI_PART,
		       CONFIG_ENV_UBI_VOLUME);
		ret = 1;
	}
	if (IS_ENABLED(CONFIG_ENV_REDUNDANT)) {
		if (ubi_volume_write(ENV_UBI_VOLUME_REDUND,
				     (void *)env_buf, 0, CONFIG_ENV_SIZE)) {
			printf("\n** Unable to erase env to %s:%s **\n",
			       CONFIG_ENV_UBI_PART,
			       ENV_UBI_VOLUME_REDUND);
			ret = 1;
		}
	}

	return ret;
}

U_BOOT_ENV_LOCATION(ubi) = {
	.location	= ENVL_UBI,
	ENV_NAME("UBI")
	.load		= env_ubi_load,
	.save		= ENV_SAVE_PTR(env_ubi_save),
	.erase		= ENV_ERASE_PTR(env_ubi_erase),
};
