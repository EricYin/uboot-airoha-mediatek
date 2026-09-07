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
