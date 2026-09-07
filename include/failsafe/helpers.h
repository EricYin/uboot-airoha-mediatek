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

#ifndef _FAILSAFE_MODULES_HELPERS_H_
#define _FAILSAFE_MODULES_HELPERS_H_

#include <net/mtk_httpd.h>
#include <linux/types.h>
#include <stdbool.h>
#include <stdio.h>

/**
 * failsafe_http_reply_text - send a plain-text HTTP response
 * @response: HTTP response structure
 * @code: HTTP status code (200, 400, 405, 500, etc.)
 * @text: response body (may be NULL, treated as "")
 */
void failsafe_http_reply_text(struct httpd_response *response,
			      int code, const char *text);

/**
 * failsafe_http_reply_json - send a JSON HTTP response (static string)
 * @response: HTTP response structure
 * @code: HTTP status code
 * @json: JSON string (may be NULL, treated as "{}")
 */
void failsafe_http_reply_json(struct httpd_response *response,
			      int code, const char *json);

/**
 * failsafe_http_reply_json_alloc - send JSON response with session-owned buffer
 * @response: HTTP response structure
 * @code: HTTP status code
 * @json: JSON string (may be NULL)
 * @session_data: heap-allocated buffer to be freed on HTTP_CB_CLOSED (may be NULL)
 *
 * Use this when the JSON buffer is dynamically allocated and must survive
 * until the HTTP session closes.  The caller must NOT free @json after
 * passing it here; the framework will free @session_data on close.
 */
void failsafe_http_reply_json_alloc(struct httpd_response *response,
				    int code, const char *json,
				    void *session_data);

/**
 * failsafe_get_form_value - extract and duplicate a form/query value
 * @request: HTTP request
 * @key: form field name
 * @out: receives heap-allocated copy of the value (caller must free)
 * @max_len: maximum accepted value length
 * @allow_empty: if true, missing or zero-length values produce an empty string
 * @allow_missing: if true, missing keys return *out = NULL with success
 *
 * Returns 0 on success, -EINVAL if key missing/empty (when not allowed),
 * -E2BIG if value exceeds max_len, -ENOMEM on allocation failure.
 */
int failsafe_get_form_value(struct httpd_request *request,
			    const char *key, char **out,
			    size_t max_len, bool allow_empty,
			    bool allow_missing);

/**
 * failsafe_free_session - free session_data on HTTP_CB_CLOSED
 * @status: handler status enum
 * @response: HTTP response (session_data freed if non-NULL)
 *
 * Call this at the top of every handler that allocates session_data.
 */
void failsafe_free_session(enum httpd_uri_handler_status status,
			   struct httpd_response *response);

/**
 * failsafe_str_sanitize - replace non-alphanumeric chars with '_'
 * @s: string to sanitize in-place (may be NULL)
 *
 * Keeps '-', '_', '.' intact.  Useful for generating safe filenames
 * from user-supplied partition or device names.
 */
void failsafe_str_sanitize(char *s);

/**
 * json_escape - escape a string for JSON output
 * @dst: destination buffer
 * @dst_sz: destination buffer size
 * @src: source string
 *
 * Returns the number of characters written (excluding null terminator).
 */
size_t json_escape(char *dst, size_t dst_sz, const char *src);

/**
 * buf_appendf - append a formatted string to a buffer
 * @buf: destination buffer
 * @size: total size of @buf
 * @len: current used length of @buf
 * @fmt: format string
 * ...
 *
 * Appends the formatted output at @buf + @len.  Uses vscnprintf() so the
 * returned value is the number of characters actually written (clamped to
 * @size - 1 - @len); feeding it back as @len keeps @buf within bounds.
 *
 * Returns the new used length.
 */
int buf_appendf(char *buf, int size, int len, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

#endif /* _FAILSAFE_MODULES_HELPERS_H_ */
