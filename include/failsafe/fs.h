// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef _FAILSAFE_FS_H_
#define _FAILSAFE_FS_H_

#include <net/mtk_httpd.h>

/*
 * Content-Encoding advertised for the embedded assets.
 *
 * The build compresses every embedded HTML / CSS / JS / SVG file with
 * gzip, or stores the minified assets as-is
 * (see CONFIG_WEBUI_FAILSAFE_COMPRESS_*); the data is stored as-is and
 * passed straight through to the HTTP client.
 *
 * The httpd does no Accept-Encoding negotiation, so whatever is selected
 * here is what every client must understand.  Brotli must not be added
 * back: browsers never advertise "br" over plain HTTP, so a brotli
 * payload always fails to decode on the client.
 */
#if defined(CONFIG_WEBUI_FAILSAFE_COMPRESS_NONE)
#define FAILSAFE_CONTENT_ENCODING	NULL
#else
#define FAILSAFE_CONTENT_ENCODING	"gzip"
#endif

struct fs_desc {
	const char *path;
	size_t size;
	const void *data;
};

const struct fs_desc *fs_find_file(const char *path);

/**
 * failsafe_output_file - serve an embedded (gzip-compressed) file
 * @response: HTTP response structure
 * @filename: path in the embedded filesystem (e.g. "index.html")
 * @content_type: MIME type (NULL defaults to "text/html")
 *
 * Sets response->info.content_encoding to FAILSAFE_CONTENT_ENCODING
 * when the file exists.  Returns 0 on success, 1 if not found.
 */
int failsafe_output_file(struct httpd_response *response,
			 const char *filename,
			 const char *content_type);

/**
 * failsafe_output_binary - serve an embedded file with a guessed MIME type
 * @response: HTTP response structure
 * @filename: path in the embedded filesystem (e.g. "favicon.svg")
 * @content_type: MIME type (NULL defaults to "application/octet-stream")
 *
 * Same as failsafe_output_file() but defaults to a binary MIME type and
 * does not force the response code when the file exists.
 * Returns 0 on success, 1 if the file was not found.
 */
int failsafe_output_binary(struct httpd_response *response,
			   const char *filename,
			   const char *content_type);

#endif /* _FAILSAFE_FS_H_ */
