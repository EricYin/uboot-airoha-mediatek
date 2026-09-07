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

/*
 * Shared tail of failsafe_output_file() / failsafe_output_binary(): both
 * only differ in the default MIME type and in the 404 body, so keeping a
 * single copy avoids duplicating the response setup in every caller.
 */
static int output_file_common(struct httpd_response *response,
			      const char *filename,
			      const char *content_type,
			      const char *default_type,
			      const char *not_found_body)
{
	const struct fs_desc *file;

	file = fs_find_file(filename);

	response->status = HTTP_RESP_STD;

	if (file) {
		response->data = file->data;
		response->size = file->size;
		/* embedded assets are compressed at build time */
		response->info.content_encoding = FAILSAFE_CONTENT_ENCODING;
	} else {
		response->data = not_found_body;
		response->size = strlen(response->data);
		response->info.content_encoding = NULL;
		response->info.code = 404;
	}

	if (!response->info.code)
		response->info.code = 200;

	response->info.connection_close = 1;
	response->info.content_type = content_type ? content_type : default_type;

	return file ? 0 : 1;
}

int failsafe_output_file(struct httpd_response *response,
			 const char *filename,
			 const char *content_type)
{
	return output_file_common(response, filename, content_type,
				  "text/html", "Error: file not found");
}

int failsafe_output_binary(struct httpd_response *response,
			   const char *filename,
			   const char *content_type)
{
	return output_file_common(response, filename, content_type,
				  "application/octet-stream", "Not Found");
}
