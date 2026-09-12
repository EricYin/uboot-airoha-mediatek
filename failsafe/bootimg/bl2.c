// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: BL2 (preloader) banner parsing and payload
 * location, shared by the Airoha / MediaTek board code.
 *
 * The preloader embeds a banner built from a few short strings, e.g.
 * (Airoha open-source blob):
 *
 *     "v2.10.0\t(release):00dba2b\0Built : 14:09:01, Sep 11 2026\0"
 *
 * or (MediaTek SDK blob):
 *
 *     "v2.15.0(release):v2.15.0-681-g1e7a2a66f-dirty\0"
 *     "Built : 21:50:06, Sep 11 2026\0"
 *
 * tools/airoha_info_preloader.py parses the same strings; the rules
 * below mirror its version / extra / commit / build-date extraction
 * without regular expressions.
 */

#include <linux/string.h>

#include <failsafe/bl2.h>
#include <failsafe/fip.h>

static bool bl2_is_printable(u8 c)
{
	return c >= 0x20 && c <= 0x7e;
}

/*
 * Return the next run of printable ASCII characters and advance @pos
 * past it.  Returns NULL at the end of the buffer.
 */
static const char *bl2_next_run(const u8 *data, size_t size, size_t *pos,
				size_t *run_len)
{
	size_t start, i = *pos;

	while (i < size && !bl2_is_printable(data[i]))
		i++;

	start = i;
	while (i < size && bl2_is_printable(data[i]))
		i++;

	*pos = i;
	*run_len = i - start;

	return i > start ? (const char *)(data + start) : NULL;
}

/* Does @run look like a version banner "v<digits>.<digit>..."? */
static bool bl2_run_is_version(const char *run, size_t len)
{
	size_t i;

	if (len < 4 || run[0] != 'v')
		return false;

	i = 1;
	if (run[i] < '0' || run[i] > '9')
		return false;

	while (i < len && run[i] >= '0' && run[i] <= '9')
		i++;

	if (i >= len || run[i] != '.')
		return false;

	i++;
	return i < len && run[i] >= '0' && run[i] <= '9';
}

static void bl2_copy(char *dst, size_t dst_sz, const char *src, size_t len)
{
	if (!dst_sz)
		return;

	if (len > dst_sz - 1)
		len = dst_sz - 1;

	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* Append " <src>" to @dst, keeping it NUL-terminated and in bounds. */
static void bl2_append(char *dst, size_t dst_sz, const char *src, size_t len)
{
	size_t used = strlen(dst);
	size_t space, n;

	if (used + 1 >= dst_sz)
		return;

	dst[used++] = ' ';
	dst[used] = '\0';

	space = dst_sz - 1 - used;
	n = len < space ? len : space;
	memcpy(dst + used, src, n);
	dst[used + n] = '\0';
}

bool failsafe_bl2_parse_banner(const void *data, size_t size,
			       struct failsafe_bl2_info *info)
{
	const u8 *buf = data;
	size_t pos = 0, run_len = 0;
	const char *run;
	bool have_version = false;

	memset(info, 0, sizeof(*info));

	while ((run = bl2_next_run(buf, size, &pos, &run_len))) {
		if (!have_version && bl2_run_is_version(run, run_len)) {
			size_t half_len = 0;
			const char *half;

			bl2_copy(info->version, sizeof(info->version), run,
				 run_len);
			have_version = true;

			/*
			 * "v2.10.0" is followed by "(release):00dba2b" in a
			 * separate run; merge the two.  The peeked run is not
			 * consumed, so it is still seen by the main loop when
			 * it is not a continuation.
			 */
			if (!strchr(info->version, '(')) {
				size_t peek = pos;

				half = bl2_next_run(buf, size, &peek,
						    &half_len);
				if (half && half_len >= 3 && half[0] == '(' &&
				    memchr(half, ')', half_len))
					bl2_append(info->version,
						   sizeof(info->version),
						   half, half_len);
			}
			continue;
		}

		if (!info->build_date[0] && run_len >= 5 &&
		    !strncmp(run, "Built", 5)) {
			const char *p = run + 5;
			const char *end = run + run_len;

			while (p < end && (*p == ' ' || *p == '\t'))
				p++;
			if (p < end && *p == ':')
				p++;
			while (p < end && (*p == ' ' || *p == '\t'))
				p++;

			bl2_copy(info->build_date, sizeof(info->build_date),
				 p, end - p);
			continue;
		}
	}

	info->found = info->version[0] || info->build_date[0];

	return info->found;
}

void failsafe_bl2_locate(const void *data, size_t size,
			 const size_t *fip_offsets, size_t num_offsets,
			 const void **bl2, size_t *bl2_size)
{
	size_t i;

	*bl2 = data;
	*bl2_size = size;

	for (i = 0; i < num_offsets; i++) {
		size_t foff = fip_offsets[i];
		const u8 *payload;
		size_t payload_size;

		if (!failsafe_fip_check(data, size, foff))
			continue;

		if (!failsafe_fip_find(data, size, foff,
				      failsafe_fip_uuid_tb_fw, &payload,
				      &payload_size, NULL)) {
			*bl2 = payload;
			*bl2_size = payload_size;
			return;
		}

		/* FIP magic but no usable tb-fw entry: fall back to the
		 * whole-image scan (only one offset can hold the FIP). */
		break;
	}
}
