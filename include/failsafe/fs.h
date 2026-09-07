// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef _FAILSAFE_FS_H_
#define _FAILSAFE_FS_H_

#include <net/mtk_httpd.h>

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
 * Sets response->info.content_encoding = "gzip" when the file exists.
 * Returns 0 on success, 1 if the file was not found.
 */
int failsafe_output_file(struct httpd_response *response,
			 const char *filename,
			 const char *content_type);

#endif /* _FAILSAFE_FS_H_ */
