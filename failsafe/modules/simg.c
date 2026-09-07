/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance
 * with the license agreement.
 *
 * Failsafe SIMG (single image / ROM dump) writer
 *
 * A SIMG is a raw dump of the whole flash chip ("ROM dump"): one single
 * image covering the entire MTD master device from offset 0 up to
 * mtd->size.  This module only implements the restore direction — the
 * browser slices the uploaded image into erase-block aligned chunks and
 * streams them to /simg/write one after another, each request erasing
 * and programming only its own range.  Streaming keeps the peak heap
 * usage down to a single erase block, so a full 128 MiB dump can be
 * restored on a device with very little free RAM.
 *
 * There is deliberately no read / erase / offset editing support, and no
 * partition selection: a ROM dump always targets the raw chip (the MTD
 * device without a parent), never one of its partitions.  The only user
 * interaction is "select a SIMG file and write it".
 */

#include <errno.h>
#include <malloc.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <net/mtk_httpd.h>
#include <vsprintf.h>

#ifdef CONFIG_MTD
#include <mtd.h>
#include <linux/mtd/mtd.h>
#endif

#include <failsafe/internal.h>

/* Maximum payload of a single /simg/write request.  The browser must use
 * the exact same value (it is also reported by /simg/info as
 * "max_chunk"), otherwise a chunk would be rejected with "bad_range".
 */
#define SIMG_MAX_CHUNK		(4 * 1024 * 1024)

/* JSON buffer sizes */
#define SIMG_INFO_BUF_SZ	4096
#define SIMG_JSON_BUF_SZ	256

/* Maximum accepted length of the optional "target" form value */
#define SIMG_TARGET_MAX_LEN	64

/* Maximum number of MTD devices probed */
#define SIMG_MTD_MAX_DEVICES	64

/* ------------------------------------------------------------------ */
/*  MTD helpers                                                        */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MTD
/*
 * simg_get_master() - get the whole-flash (master) MTD device
 * @name: requested device name, or NULL/empty for "first master device"
 *
 * A master device is an MTD device without a parent; every partition of
 * it has ->parent set.  A ROM dump always covers the raw chip, so
 * partitions are never accepted as a target.
 *
 * Returns the device (still referenced, caller must put_mtd_device()) or
 * NULL when no matching master device exists.
 */
static struct mtd_info *simg_get_master(const char *name)
{
	struct mtd_info *mtd;
	u32 i;

	mtd_probe_devices();

	for (i = 0; i < SIMG_MTD_MAX_DEVICES; i++) {
		mtd = get_mtd_device(NULL, i);
		if (IS_ERR(mtd))
			continue;

		if (mtd->parent || !mtd->name || !mtd->name[0]) {
			put_mtd_device(mtd);
			continue;
		}

		if (name && name[0] && strcmp(mtd->name, name)) {
			put_mtd_device(mtd);
			continue;
		}

		return mtd;
	}

	return NULL;
}

static int simg_erase_block(struct mtd_info *mtd, u64 off)
{
	struct erase_info ei;

	memset(&ei, 0, sizeof(ei));
	ei.mtd = mtd;
	ei.addr = off;
	ei.len = mtd->erasesize;

	return mtd_erase(mtd, &ei);
}

/* Program @len bytes, one write unit (page) at a time. */
static int simg_program_range(struct mtd_info *mtd, u64 off, const u8 *data,
			      size_t len)
{
	size_t write_sz = mtd->writesize;
	size_t done = 0;
	int ret;

	while (done < len) {
		size_t step = min_t(size_t, write_sz, len - done);
		size_t retlen = 0;

		ret = mtd_write(mtd, off + done, step, &retlen, data + done);
		if (ret)
			return ret;
		if (retlen != step)
			return -EIO;

		done += retlen;
	}

	return 0;
}

/*
 * simg_write_range() - erase and program [start, start + len) of @mtd
 * @mtd: target device (master)
 * @start: absolute flash offset of the first byte
 * @data: payload to program
 * @len: payload length
 * @written: receives the number of payload bytes actually programmed
 * @skipped: receives the number of payload bytes lost to bad blocks
 *
 * Blocks fully covered by the request are erased and programmed
 * directly.  A block that is only partially covered (only possible at
 * the tail of an image whose size is not a multiple of the erase size)
 * is preserved with a read-modify-write cycle so that the bytes outside
 * the requested range survive.  Bad blocks are skipped and accounted in
 * @skipped.
 */
static int simg_write_range(struct mtd_info *mtd, u64 start, const u8 *data,
			    size_t len, size_t *written, size_t *skipped)
{
	u64 erase_sz = mtd->erasesize;
	u64 block_start, block_end, blk;
	u64 done = 0, lost = 0;
	u8 *blkbuf = NULL;
	int ret = 0;

	if (!erase_sz || !mtd->writesize || !len)
		return -EINVAL;

	block_start = start & ~(erase_sz - 1);
	block_end = (start + len + erase_sz - 1) & ~(erase_sz - 1);

	for (blk = block_start; blk < block_end; blk += erase_sz) {
		u64 data_start = max_t(u64, start, blk);
		u64 data_end = min_t(u64, start + len, blk + erase_sz);
		bool full_block = (data_start == blk) &&
				  (data_end == blk + erase_sz);
		size_t copy_len = (size_t)(data_end - data_start);
		int bad;

		bad = mtd_block_isbad(mtd, blk);
		if (bad < 0) {
			ret = bad;
			goto out;
		}

		if (bad) {
			lost += copy_len;
			continue;
		}

		if (!full_block) {
			size_t readlen = 0;

			if (!blkbuf) {
				blkbuf = malloc(erase_sz);
				if (!blkbuf) {
					ret = -ENOMEM;
					goto out;
				}
			}

			ret = mtd_read(mtd, blk, erase_sz, &readlen, blkbuf);
			if (ret && ret != -EUCLEAN) {
				printf("simg: read 0x%llx failed: %d\n",
				       blk, ret);
				goto out;
			}
			if (readlen != erase_sz) {
				ret = -EIO;
				goto out;
			}
		}

		ret = simg_erase_block(mtd, blk);
		if (ret) {
			printf("simg: erase 0x%llx failed: %d\n", blk, ret);
			goto out;
		}

		if (full_block) {
			ret = simg_program_range(mtd, blk,
						 data + (data_start - start),
						 copy_len);
		} else {
			memcpy(blkbuf + (data_start - blk),
			       data + (data_start - start), copy_len);
			ret = simg_program_range(mtd, blk, blkbuf, erase_sz);
		}

		if (ret) {
			printf("simg: write 0x%llx failed: %d\n", blk, ret);
			goto out;
		}

		done += copy_len;
	}

out:
	free(blkbuf);

	if (written)
		*written = (size_t)done;
	if (skipped)
		*skipped = (size_t)lost;

	return ret;
}
#endif /* CONFIG_MTD */

/* ------------------------------------------------------------------ */
/*  GET /simg/info                                                     */
/* ------------------------------------------------------------------ */

/**
 * simg_info_handler - GET /simg/info
 *
 * Reports the whole-flash MTD devices a SIMG can be restored to, plus
 * the maximum chunk size the writer accepts:
 * {"targets":[{"name":"spi-nand0","size":N,"erasesize":N,"writesize":N,
 *              "type":N}],"max_chunk":N}
 */
void simg_info_handler(enum httpd_uri_handler_status status,
		       struct httpd_request *request,
		       struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = SIMG_INFO_BUF_SZ;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_GET) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	len = buf_appendf(buf, left, len,
			  "{\"max_chunk\":%u,\"targets\":[", SIMG_MAX_CHUNK);

#ifdef CONFIG_MTD
	{
		struct mtd_info *mtd;
		bool first = true;
		u32 i;

		mtd_probe_devices();

		for (i = 0; i < SIMG_MTD_MAX_DEVICES && len < left - 192; i++) {
			char esc_name[128];

			mtd = get_mtd_device(NULL, i);
			if (IS_ERR(mtd))
				continue;

			/* partitions are never a valid SIMG target */
			if (mtd->parent || !mtd->name || !mtd->name[0]) {
				put_mtd_device(mtd);
				continue;
			}

			json_escape(esc_name, sizeof(esc_name), mtd->name);
			len = buf_appendf(buf, left, len,
				"%s{\"name\":\"%s\",\"size\":%llu,"
				"\"erasesize\":%llu,\"writesize\":%llu,"
				"\"type\":%d}",
				first ? "" : ",",
				esc_name,
				(unsigned long long)mtd->size,
				(unsigned long long)mtd->erasesize,
				(unsigned long long)mtd->writesize,
				(int)mtd->type);

			first = false;
			put_mtd_device(mtd);
		}
	}
#endif

	len = buf_appendf(buf, left, len, "]}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/* ------------------------------------------------------------------ */
/*  POST /simg/write                                                   */
/* ------------------------------------------------------------------ */

/**
 * simg_write_handler - POST /simg/write (one streamed chunk)
 *
 * Form parameters:
 *   target - MTD master device name (optional, defaults to the first one)
 *   start  - absolute flash offset of this chunk (hex or decimal)
 *   end    - end offset of this chunk, exclusive (hex or decimal)
 *   data   - chunk payload (binary safe)
 *
 * Returns JSON: {"ok":true,"written":N,"skipped":N} or {"error":"..."}.
 */
void simg_write_handler(enum httpd_uri_handler_status status,
			struct httpd_request *request,
			struct httpd_response *response)
{
#ifndef CONFIG_MTD
	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	failsafe_http_reply_json(response, 500,
				 "{\"ok\":false,\"error\":\"no_mtd\"}");
#else
	struct httpd_form_value *data_val;
	char *target = NULL;
	char *start_str = NULL;
	char *end_str = NULL;
	char *json;
	struct mtd_info *mtd;
	u64 start = 0, end = 0;
	size_t len, written = 0, skipped = 0;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	data_val = httpd_request_find_value(request, "data");
	if (!data_val || !data_val->data || !data_val->size) {
		failsafe_http_reply_json(response, 400,
					 "{\"ok\":false,\"error\":\"no_data\"}");
		return;
	}

	ret = failsafe_get_form_value(request, "start", &start_str, 32,
				      false, false);
	if (ret) {
		failsafe_http_reply_json(response, 400,
					 "{\"ok\":false,\"error\":\"no_range\"}");
		return;
	}

	ret = failsafe_get_form_value(request, "end", &end_str, 32,
				      false, false);
	if (ret) {
		free(start_str);
		failsafe_http_reply_json(response, 400,
					 "{\"ok\":false,\"error\":\"no_range\"}");
		return;
	}

	start = simple_strtoull(start_str, NULL, 0);
	end = simple_strtoull(end_str, NULL, 0);
	free(start_str);
	free(end_str);

	if (end <= start || (end - start) > SIMG_MAX_CHUNK ||
	    (u64)data_val->size != (end - start)) {
		failsafe_http_reply_json(response, 400,
					 "{\"ok\":false,\"error\":\"bad_range\"}");
		return;
	}

	len = data_val->size;

	(void)failsafe_get_form_value(request, "target", &target,
				      SIMG_TARGET_MAX_LEN, true, true);

	mtd = simg_get_master(target);
	free(target);

	if (!mtd) {
		failsafe_http_reply_json(response, 404,
			"{\"ok\":false,\"error\":\"target_not_found\"}");
		return;
	}

	if (!mtd->erasesize || !mtd->writesize || end > mtd->size) {
		put_mtd_device(mtd);
		failsafe_http_reply_json(response, 400,
					 "{\"ok\":false,\"error\":\"bad_range\"}");
		return;
	}

	printf("simg: writing %s 0x%llx-0x%llx (%zu bytes)\n",
	       mtd->name, start, end, len);

	ret = simg_write_range(mtd, start, (const u8 *)data_val->data, len,
			       &written, &skipped);

	put_mtd_device(mtd);

	if (ret) {
		failsafe_http_reply_json(response, 500,
					 "{\"ok\":false,\"error\":\"io\"}");
		return;
	}

	json = malloc(SIMG_JSON_BUF_SZ);
	if (!json) {
		failsafe_http_reply_json(response, 500,
					 "{\"ok\":false,\"error\":\"oom\"}");
		return;
	}

	snprintf(json, SIMG_JSON_BUF_SZ,
		 "{\"ok\":true,\"start\":\"0x%llx\",\"end\":\"0x%llx\","
		 "\"written\":%zu,\"skipped\":%zu}\n",
		 start, end, written, skipped);

	failsafe_http_reply_json_alloc(response, 200, json, json);
#endif /* CONFIG_MTD */
}

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_WEBUI_FAILSAFE_SIMG
void simg_register_handlers(struct httpd_instance *inst)
{
	httpd_register_uri_handler(inst, "/simg.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/simg_js.js", &js_handler, NULL);
	httpd_register_uri_handler(inst, "/simg/info", &simg_info_handler, NULL);
	httpd_register_uri_handler(inst, "/simg/write", &simg_write_handler, NULL);
}
#endif
