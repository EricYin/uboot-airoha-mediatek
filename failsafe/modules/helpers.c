/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Common HTTP helper functions shared by all failsafe modules
 */

#include <errno.h>
#include <malloc.h>
#include <limits.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <vsprintf.h>
#include <net/mtk_httpd.h>

#include <failsafe/helpers.h>

/* ------------------------------------------------------------------ */
/*  HTTP response helpers                                              */
/* ------------------------------------------------------------------ */

void failsafe_http_reply_text(struct httpd_response *response,
			      int code, const char *text)
{
	response->status = HTTP_RESP_STD;
	response->data = text ? text : "";
	response->size = strlen(response->data);
	response->info.code = code;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";
}

void failsafe_http_reply_json(struct httpd_response *response,
			      int code, const char *json)
{
	response->status = HTTP_RESP_STD;
	response->data = json ? json : "{}";
	response->size = strlen(response->data);
	response->info.code = code;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
}

void failsafe_http_reply_json_alloc(struct httpd_response *response,
				    int code, const char *json,
				    void *session_data)
{
	response->status = HTTP_RESP_STD;
	response->data = json ? json : "{}";
	response->size = strlen(response->data);
	response->info.code = code;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
	response->session_data = session_data;
}

/* ------------------------------------------------------------------ */
/*  Form value extraction                                              */
/* ------------------------------------------------------------------ */

int failsafe_get_form_value(struct httpd_request *request,
			    const char *key, char **out,
			    size_t max_len, bool allow_empty,
			    bool allow_missing)
{
	struct httpd_form_value *v;
	char *buf;
	size_t n;

	if (!request || !key || !out)
		return -EINVAL;

	v = httpd_request_find_value(request, key);
	if (!v || !v->data) {
		if (allow_missing) {
			*out = NULL;
			return 0;
		}
		if (allow_empty) {
			buf = strdup("");
			if (!buf)
				return -ENOMEM;
			*out = buf;
			return 0;
		}
		return -EINVAL;
	}

	n = v->size;
	if (!allow_empty && !n)
		return -EINVAL;
	if (n > max_len)
		return -E2BIG;

	buf = malloc(n + 1);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, v->data, n);
	buf[n] = '\0';
	*out = buf;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Session lifecycle                                                  */
/* ------------------------------------------------------------------ */

void failsafe_free_session(enum httpd_uri_handler_status status,
			   struct httpd_response *response)
{
	if (status != HTTP_CB_CLOSED)
		return;

	if (response->session_data) {
		free(response->session_data);
		response->session_data = NULL;
	}
}

/* ------------------------------------------------------------------ */
/*  String utilities                                                   */
/* ------------------------------------------------------------------ */

void failsafe_str_sanitize(char *s)
{
	char *p;

	if (!s)
		return;

	for (p = s; *p; p++) {
		unsigned char c = *p;

		if (isalnum(c) || c == '-' || c == '_' || c == '.')
			continue;

		*p = '_';
	}
}

/* ------------------------------------------------------------------ */
/*  JSON escape                                                        */
/* ------------------------------------------------------------------ */

size_t json_escape(char *dst, size_t dst_sz, const char *src)
{
	size_t di = 0;
	const unsigned char *s = (const unsigned char *)src;

	if (!dst || !dst_sz)
		return 0;

	if (!src) {
		dst[0] = '\0';
		return 0;
	}

	while (*s && di + 2 < dst_sz) {
		unsigned char c = *s++;

		if (c == '"' || c == '\\') {
			if (di + 2 >= dst_sz)
				break;
			dst[di++] = '\\';
			dst[di++] = (char)c;
			continue;
		}

		if (c == '\n' || c == '\r' || c == '\t') {
			if (di + 2 >= dst_sz)
				break;
			dst[di++] = '\\';
			dst[di++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
			continue;
		}

		if (c == 0x1b || c == 0x08) {
			/*
			 * Round-trip CSI / backspace so the failsafe web
			 * console can render U-Boot `bootmenu` (and any
			 * other program using cursor positioning / SGR
			 * colors) instead of collapsing it into one
			 * unreadable blob of "[2J[1;1H..." text.  See
			 * console_js.js:ansiTerm.
			 */
			static const char hex[] = "0123456789abcdef";

			if (di + 6 >= dst_sz)
				break;
			dst[di++] = '\\';
			dst[di++] = 'u';
			dst[di++] = '0';
			dst[di++] = '0';
			dst[di++] = hex[(c >> 4) & 0xf];
			dst[di++] = hex[c & 0xf];
			continue;
		}

		if (c < 0x20) {
			/* skip other control chars */
			dst[di++] = ' ';
			continue;
		}

		dst[di++] = (char)c;
	}

	dst[di] = '\0';
	return di;
}

/* ------------------------------------------------------------------ */
/*  Safe buffer append (JSON payload builders)                         */
/* ------------------------------------------------------------------ */

int buf_appendf(char *buf, int size, int len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (len >= size)
		return len;

	va_start(ap, fmt);
	n = vscnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);

	if (n > 0)
		return len + n;
	return len;
}

/* ------------------------------------------------------------------ */
/*  MMC manufacturer ID lookup                                         */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_MMC

struct mmc_mid_entry {
	unsigned int mid;
	const char *name;
};

/* JEDEC / SD card manufacturer IDs read from the MMC CID (MID). */
static const struct mmc_mid_entry mmc_mid_table[] = {
	{ 0x00, "SanDisk/Spansion" },
	{ 0x01, "Samsung" },
	{ 0x02, "Kingston/SanDisk" },
	{ 0x03, "Toshiba" },
	{ 0x05, "Unknown" },
	{ 0x06, "Unknown" },
	{ 0x11, "Toshiba" },
	{ 0x13, "Micron" },
	{ 0x15, "Samsung/SanDisk/LG" },
	{ 0x18, "Swissbit" },
	{ 0x2c, "HIKSEM/Kingston" },
	{ 0x2f, "Konsemi" },
	{ 0x30, "SMART Modular" },
	{ 0x32, "Qimonda" },
	{ 0x37, "KingMax" },
	{ 0x44, "ATP/Transcend" },
	{ 0x45, "SanDisk/WesternDigital" },
	{ 0x70, "Kingston" },
	{ 0x88, "Longsys" },
	{ 0x90, "SK hynix" },
	{ 0x9b, "YMTC" },
	{ 0x9c, "ATP" },
	{ 0xce, "Samsung" },
	{ 0xd6, "Longsys" },
	{ 0xdf, "SCY" },
	{ 0xea, "Kowin/SiliconGo/SPeMMC" },
	{ 0xec, "ATO/Rayson" },
	{ 0xf4, "BIWIN" },
	{ 0xfe, "Foresee/Micron" },
};

static const char *mmc_mid_lookup(unsigned int mid)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(mmc_mid_table); i++) {
		if (mmc_mid_table[i].mid == mid)
			return mmc_mid_table[i].name;
	}

	return NULL;
}

/*
 * Decorate an MMC vendor string:
 *   "Man 00002c Snr 02e9a8c9" -> "Man 00002c(HIKSEM/Kingston) Snr 02e9a8c9"
 * An unknown / unexpected format is copied through unchanged.
 */
void failsafe_mmc_vendor_pretty(const char *vendor, char *dst, size_t dst_sz)
{
	const char *p, *snr;
	unsigned int mid;
	const char *name;
	char hex[7];
	int i;

	if (!vendor || !vendor[0] || !dst || !dst_sz)
		return;

	dst[0] = '\0';

	/* Only transform strings starting with "Man ". */
	if (strncmp(vendor, "Man ", 4))
		goto copy_raw;

	/* Parse the 6 character hex MID: "Man XXXXXX". */
	p = vendor + 4;
	if (strlen(p) < 6)
		goto copy_raw;

	memcpy(hex, p, 6);
	hex[6] = '\0';

	for (i = 0; i < 6; i++) {
		if (!isxdigit((unsigned char)hex[i]))
			goto copy_raw;
	}

	mid = simple_strtoul(hex, NULL, 16);

	/* The serial number follows the MID. */
	snr = strstr(p + 6, " Snr");
	if (!snr)
		goto copy_raw;

	name = mmc_mid_lookup(mid);
	if (!name)
		goto copy_raw;

	snprintf(dst, dst_sz, "Man %06x(%s)%s", mid, name, snr);
	return;

copy_raw:
	strlcpy(dst, vendor ? vendor : "", dst_sz);
}

#endif /* CONFIG_MMC */
