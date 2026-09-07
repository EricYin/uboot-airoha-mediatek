// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <linker_lists.h>
#include <malloc.h>
#include <string.h>
#include <vsprintf.h>
#include <net/mtk_httpd.h>
#include <failsafe/fs.h>

const struct fs_desc *fs_find_file(const char *path)
{
	struct fs_desc *start = ll_entry_start(struct fs_desc, fs);
	int len = ll_entry_count(struct fs_desc, fs);

	while (len) {
		if (!strcmp(start->path, path))
			return start;

		len--;
		start++;
	}

	return NULL;
}

int failsafe_output_file(struct httpd_response *response,
			 const char *filename,
			 const char *content_type)
{
	const struct fs_desc *file;

	file = fs_find_file(filename);

	response->status = HTTP_RESP_STD;

	if (file) {
		response->data = file->data;
		response->size = file->size;
		/* embedded assets are gzip-compressed at build time */
		response->info.content_encoding = "gzip";
		response->info.code = 200;
	} else {
		response->data = "Error: file not found";
		response->size = strlen(response->data);
		response->info.content_encoding = NULL;
		response->info.code = 404;
	}

	response->info.connection_close = 1;
	response->info.content_type = content_type ? content_type : "text/html";

	return file ? 0 : 1;
}
