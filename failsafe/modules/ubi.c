/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe UBI volume management
 */

#include <errno.h>
#include <malloc.h>
#include <memalign.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <net/mtk_httpd.h>
#include <mtd.h>
#include <nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/err.h>
#include <ubi_uboot.h>
#include <linux/errno.h>
#include <vsprintf.h>
#include <command.h>

#ifdef CONFIG_CMD_UBIFS
#include <ubifs_uboot.h>
#endif

#include <failsafe/internal.h>
#include <failsafe/storage.h>

#include <env.h>
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
#include <net.h>
#endif

/* Max buffer size for JSON response */
#define UBI_JSON_BUF_SZ		16384

/* Max volume name length */
#define UBI_VOL_NAME_MAX_LEN	128

/* Max MTD partition name length */
#define UBI_MTD_NAME_MAX_LEN	64

/**
 * ubi_info_handler - GET /ubi/info
 *
 * Returns JSON with UBI device information:
 * {"mtd_name":"...","flash_size":0,"peb_size":0,"leb_size":0,...}
 */
void ubi_info_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;

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

	/* Check if UBI is attached */
	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		len = buf_appendf(buf, left, len,
			"{\"error\":\"no ubi device\",\"attached\":false}");
		goto done;
	}

	len = buf_appendf(buf, left, len,
		"{\"attached\":true,"
		"\"mtd_name\":\"%s\","
		"\"ubi_num\":%d,"
		"\"flash_size\":%llu,"
		"\"peb_size\":%d,"
		"\"leb_size\":%d,"
		"\"good_peb_count\":%d,"
		"\"bad_peb_count\":%d,"
		"\"min_io_size\":%d,"
		"\"max_vol_count\":%d,"
		"\"vol_count\":%d,"
		"\"avail_pebs\":%d,"
		"\"rsvd_pebs\":%d,"
		"\"beb_rsvd_pebs\":%d,"
		"\"max_ec\":%d,"
		"\"mean_ec\":%d}",
		ubi->mtd ? ubi->mtd->name : "unknown",
		ubi->ubi_num,
		(unsigned long long)ubi->flash_size,
		ubi->peb_size,
		ubi->leb_size,
		ubi->good_peb_count,
		ubi->bad_peb_count,
		ubi->min_io_size,
		ubi->vtbl_slots,
		ubi->vol_count - UBI_INT_VOL_COUNT,
		ubi->avail_pebs,
		ubi->rsvd_pebs,
		ubi->beb_rsvd_pebs,
		ubi->max_ec,
		ubi->mean_ec);

done:
	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_volumes_handler - GET /ubi/volumes
 *
 * Returns JSON array of UBI volumes:
 * {"volumes":[{"id":0,"name":"...","size":0,"type":"dynamic",...},...]}
 */
void ubi_volumes_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;
	bool first = true;

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

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		len = buf_appendf(buf, left, len,
			"{\"error\":\"no ubi device\",\"volumes\":[]}");
		goto done;
	}

	len = buf_appendf(buf, left, len, "{\"volumes\":[");

	for (int i = 0; i < ubi->vtbl_slots && len < left - 256; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol)
			continue;
		if (vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;

		len = buf_appendf(buf, left, len,
			"%s{\"id\":%d,\"name\":\"%s\","
			"\"size\":%llu,\"used_bytes\":%llu,"
			"\"type\":\"%s\","
			"\"corrupted\":%d,\"upd_marker\":%d,"
			"\"skip_check\":%d,"
			"\"reserved_peb\":%d,\"alignment\":%d,"
			"\"data_pad\":%d,\"usable_leb_size\":%d}",
			first ? "" : ",",
			vol->vol_id,
			vol->name,
			(unsigned long long)vol->reserved_pebs * ubi->leb_size,
			(unsigned long long)vol->used_bytes,
			vol->vol_type == UBI_DYNAMIC_VOLUME ? "dynamic" : "static",
			vol->corrupted,
			vol->upd_marker,
			vol->skip_check,
			vol->reserved_pebs,
			vol->alignment,
			vol->data_pad,
			vol->usable_leb_size);

		first = false;
	}

	len = buf_appendf(buf, left, len, "]}");

done:
	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_attach_handler - POST /ubi/attach
 *
 * Form parameters:
 *   mtd_name - MTD partition name to attach
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_attach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *mtd_name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	/* Get MTD name */
	ret = failsafe_get_form_value(request, "mtd_name", &mtd_name,
		UBI_MTD_NAME_MAX_LEN, false, false);
	if (ret || !mtd_name || !mtd_name[0]) {
		json_out = strdup("{\"error\":\"missing mtd_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing mtd_name\"}",
			json_out);
		return;
	}

	/* Detach existing UBI first */
	ubi_detach();

	/* Attach to new partition */
	ret = ubi_part(mtd_name, NULL);
	free(mtd_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"attach failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"attach failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_detach_handler - POST /ubi/detach
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_detach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	ret = ubi_detach();

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"detach failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"detach failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_rebuild_handler - POST /ubi/rebuild
 *
 * Build the UBI device up from nothing:
 *
 *   0. read the FIP out of its "fip" volume, if there is one - it is the
 *      only volume that cannot be recreated from anything else, and a
 *      board without it does not boot (the split layout the board support
 *      package uses keeps it there);
 *   1. "ubi detach";
 *   2. erase the whole MTD partition that holds UBI, the same thing as
 *      "mtd erase <partition>": afterwards there is no volume table left
 *      at all, which is what makes this work on a device whose table is
 *      corrupt or whose volumes cannot be removed any more;
 *   3. "ubi part <partition>", which formats a new, empty UBI device;
 *   4. create the "fip" volume again - as the static volume of
 *      FAILSAFE_STORAGE_STATIC_SIZE the firmware upgrade creates (see
 *      <failsafe/storage.h>), so the rebuilt device is laid out exactly
 *      like a freshly upgraded one - and write the saved image back into
 *      it;
 *   5. give the environment a home again when it lives in this UBI device
 *      (ENV_IS_IN_UBI) and save the environment we are running with, so
 *      the device does not come up with a factory empty one after the next
 *      reset.  The MAC address the network stack depends on is part of it:
 *      the one the device had is written back, a device that never had one
 *      gets a random locally administered address.
 *
 * Every other volume (the system image "fit", the OpenWrt "rootfs_data"
 * overlay, ...) is gone afterwards: that is the point of the operation,
 * and the page warns about it before asking for confirmation.
 *
 * Form parameters:
 *   mtd_name - MTD partition holding UBI (optional: the partition the
 *              device is attached to right now, or "ubi")
 *
 * Returns JSON: {"ok":true,"mtd":"...","fip_bytes":N} or {"error":"..."}
 */
void ubi_rebuild_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char mtd_name[UBI_MTD_NAME_MAX_LEN];
	char *form_name = NULL;
	char *json_out;
	struct mtd_info *mtd;
	struct erase_info ei;
	void *fip = NULL;
	size_t fip_size = 0;
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	char ethaddr[18];
	bool ethaddr_present = false;
	bool ethaddr_generated = false;
#endif
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	/* Which partition to erase: the form value when given, otherwise the
	 * one the device is attached to, otherwise the conventional name.
	 */
	ret = failsafe_get_form_value(request, "mtd_name", &form_name,
		UBI_MTD_NAME_MAX_LEN, true, true);
	if (ret == 0 && form_name && form_name[0])
		strlcpy(mtd_name, form_name, sizeof(mtd_name));
	else if (ubi_devices[0] && ubi_devices[0]->mtd &&
		 ubi_devices[0]->mtd->name)
		strlcpy(mtd_name, ubi_devices[0]->mtd->name,
			sizeof(mtd_name));
	else
		strlcpy(mtd_name, "ubi", sizeof(mtd_name));
	free(form_name);

	/* Step 0: keep the FIP. */
	if (ubi_devices[0]) {
		struct ubi_volume *vol =
			ubi_find_volume(FAILSAFE_STORAGE_STATIC_TARGET);

		if (vol && vol->used_bytes) {
			fip_size = (size_t)vol->used_bytes;
			fip = malloc(fip_size);
			if (!fip) {
				failsafe_http_reply_json(response, 500,
							 "{\"error\":\"oom\"}");
				return;
			}

			ret = ubi_volume_read(FAILSAFE_STORAGE_STATIC_TARGET,
					      fip, 0, fip_size);
			if (ret) {
				free(fip);
				json_out = malloc(160);
				if (json_out)
					snprintf(json_out, 160,
						 "{\"error\":\"cannot read "
						 "the '%s' volume (%d)\"}",
						 FAILSAFE_STORAGE_STATIC_TARGET,
						 ret);
				failsafe_http_reply_json_alloc(response, 500,
					json_out ? json_out : "{\"error\":"
					"\"cannot read the fip volume\"}",
					json_out);
				return;
			}
		}
	}

#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	/*
	 * The environment lives in this UBI device as well, so the wipe takes
	 * it with it - and with it the MAC address the network stack needs.
	 * Keep it aside, but only when it is a usable address: a device can
	 * carry a placeholder such as ff:ff:ff:ff:ff:ff - the factory address
	 * of this family is kept in a UBI volume the rebuild recreates empty,
	 * so a unit that lost it once has the placeholder from then on - and
	 * writing that back only makes the Ethernet driver reject it.
	 * Anything else is replaced by a generated address below.
	 */
	{
		const char *val = env_get("ethaddr");

		if (val) {
			uchar mac[ARP_HLEN];

			string_to_enetaddr(val, mac);
			if (is_valid_ethaddr(mac)) {
				snprintf(ethaddr, sizeof(ethaddr), "%pM", mac);
				ethaddr_present = true;
			}
		}
	}
#endif

	/* Step 1: detach the old device. */
	ubi_detach();

	/* Step 2: erase what it lived on, so no stale volume table is left
	 * behind for the new device to pick up.
	 */
	mtd_probe_devices();
	mtd = get_mtd_device_nm(mtd_name);
	if (IS_ERR_OR_NULL(mtd)) {
		free(fip);
		json_out = malloc(160);
		if (json_out)
			snprintf(json_out, 160,
				 "{\"error\":\"no MTD partition '%s'\"}",
				 mtd_name);
		failsafe_http_reply_json_alloc(response, 404,
			json_out ? json_out : "{\"error\":\"no MTD partition\"}",
			json_out);
		return;
	}

	memset(&ei, 0, sizeof(ei));
	ei.mtd = mtd;
	ei.addr = 0;
	ei.len = mtd->size;

	ret = mtd_erase(mtd, &ei);
	put_mtd_device(mtd);

	if (ret) {
		free(fip);
		json_out = malloc(160);
		if (json_out)
			snprintf(json_out, 160,
				 "{\"error\":\"erase '%s' failed (%d)\"}",
				 mtd_name, ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"erase failed\"}",
			json_out);
		return;
	}

	/* Step 3: format and attach a fresh, empty device. */
	ret = ubi_part(mtd_name, NULL);
	if (ret) {
		free(fip);
		json_out = malloc(160);
		if (json_out)
			snprintf(json_out, 160,
				 "{\"error\":\"attach '%s' failed (%d)\"}",
				 mtd_name, ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"attach failed\"}",
			json_out);
		return;
	}

	/* Step 4: put the FIP back into the volume the upgrade path uses. */
	if (fip) {
		ret = ubi_create_vol(FAILSAFE_STORAGE_STATIC_TARGET,
				     FAILSAFE_STORAGE_STATIC_SIZE, false,
				     UBI_VOL_NUM_AUTO, false);
		if (!ret)
			ret = ubi_volume_write(FAILSAFE_STORAGE_STATIC_TARGET,
					       fip, 0, fip_size);
		free(fip);

		if (ret) {
			json_out = malloc(160);
			if (json_out)
				snprintf(json_out, 160,
					 "{\"error\":\"cannot restore the "
					 "'%s' volume (%d)\"}",
					 FAILSAFE_STORAGE_STATIC_TARGET, ret);
			failsafe_http_reply_json_alloc(response, 500,
				json_out ? json_out : "{\"error\":"
				"\"cannot restore the fip volume\"}",
				json_out);
			return;
		}
	}

#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	/*
	 * Step 5: the environment volume is gone with the old volume table,
	 * so put one back and save the environment this session is running
	 * with into it - everything else would be lost on the next reset,
	 * starting with the MAC address of the network interfaces.
	 */
	ret = env_ubi_volumes_create();
	if (ret) {
		json_out = malloc(160);
		if (json_out)
			snprintf(json_out, 160,
				 "{\"error\":\"cannot create the environment "
				 "volume (%d)\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":"
			"\"cannot create the environment volume\"}",
			json_out);
		return;
	}

	if (ethaddr_present) {
		env_set("ethaddr", ethaddr);
	} else {
		/* Same address the network stack would fall back to, only
		 * this one is written back and stays. */
		uchar mac[ARP_HLEN];
		char buf[18];

		net_random_ethaddr(mac);
		snprintf(buf, sizeof(buf), "%pM", mac);
		env_set("ethaddr", buf);
		ethaddr_generated = true;
	}

	ret = env_save();
	if (ret) {
		json_out = malloc(160);
		if (json_out)
			snprintf(json_out, 160,
				 "{\"error\":\"cannot save the environment "
				 "(%d)\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":"
			"\"cannot save the environment\"}",
			json_out);
		return;
	}
#endif /* CONFIG_ENV_IS_IN_UBI */

	json_out = malloc(192);
	if (json_out)
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
		snprintf(json_out, 192,
			 "{\"ok\":true,\"mtd\":\"%s\",\"fip_bytes\":%zu,"
			 "\"env_restored\":true,\"ethaddr_generated\":%s}",
			 mtd_name, fip_size,
			 ethaddr_generated ? "true" : "false");
#else
		snprintf(json_out, 192,
			 "{\"ok\":true,\"mtd\":\"%s\",\"fip_bytes\":%zu,"
			 "\"env_restored\":false}",
			 mtd_name, fip_size);
#endif
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_create_vol_handler - POST /ubi/create
 *
 * Form parameters:
 *   name - Volume name
 *   size - Volume size in bytes (0 or empty for maximum)
 *   type - Volume type: "dynamic" or "static"
 *   skipcheck - Skip CRC check: "1" or "0"
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_create_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *size_str = NULL;
	char *type_str = NULL;
	char *skipcheck_str = NULL;
	char *json_out;
	int64_t size = 0;
	int dynamic = 1;
	bool skipcheck = false;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Get size (optional) */
	ret = failsafe_get_form_value(request, "size", &size_str, 32, false, true);
	if (ret == 0 && size_str && size_str[0]) {
		size = simple_strtoull(size_str, NULL, 0);
	}
	free(size_str);

	/* Get type (optional, default dynamic) */
	ret = failsafe_get_form_value(request, "type", &type_str, 16, false, true);
	if (ret == 0 && type_str) {
		if (strncmp(type_str, "s", 1) == 0)
			dynamic = 0;
	}
	free(type_str);

	/* Get skipcheck (optional) */
	ret = failsafe_get_form_value(request, "skipcheck", &skipcheck_str, 4, false, true);
	if (ret == 0 && skipcheck_str) {
		skipcheck = (skipcheck_str[0] == '1');
	}
	free(skipcheck_str);

	/* Use maximum available size if not specified */
	if (size <= 0) {
		size = (int64_t)ubi->avail_pebs * ubi->leb_size;
	}

	/* Create volume */
	ret = ubi_create_vol(name, size, dynamic, UBI_VOL_NUM_AUTO, skipcheck);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"create failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"create failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_remove_vol_handler - POST /ubi/remove
 *
 * Form parameters:
 *   name - Volume name to remove
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_remove_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Remove volume */
	ret = ubi_remove_vol(name);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"remove failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"remove failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_rename_vol_handler - POST /ubi/rename
 *
 * Form parameters:
 *   old_name - Current volume name
 *   new_name - New volume name
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_rename_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *old_name = NULL;
	char *new_name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get old name */
	ret = failsafe_get_form_value(request, "old_name", &old_name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !old_name || !old_name[0]) {
		json_out = strdup("{\"error\":\"missing old_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing old_name\"}",
			json_out);
		return;
	}

	/* Get new name */
	ret = failsafe_get_form_value(request, "new_name", &new_name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !new_name || !new_name[0]) {
		free(old_name);
		json_out = strdup("{\"error\":\"missing new_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing new_name\"}",
			json_out);
		return;
	}

	/* Find volume */
	struct ubi_volume *vol;
	vol = ubi_find_volume(old_name);
	if (!vol) {
		free(old_name);
		free(new_name);
		json_out = strdup("{\"error\":\"volume not found\"}");
		failsafe_http_reply_json_alloc(response, 404,
			json_out ? json_out : "{\"error\":\"not found\"}",
			json_out);
		return;
	}

	/* Rename volume */
	struct ubi_rename_entry rename;
	struct ubi_volume_desc desc;
	struct list_head list;

	rename.new_name_len = strlen(new_name);
	strcpy(rename.new_name, new_name);
	rename.remove = 0;
	desc.vol = vol;
	desc.mode = 0;
	rename.desc = &desc;
	INIT_LIST_HEAD(&rename.list);
	INIT_LIST_HEAD(&list);
	list_add(&rename.list, &list);

	ret = ubi_rename_volumes(ubi, &list);
	free(old_name);
	free(new_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"rename failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"rename failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_mtd_list_handler - GET /ubi/mtd_list
 *
 * Returns JSON array of available MTD partitions:
 * {"partitions":[{"name":"...","size":0,"type":"..."},...]}
 */
void ubi_mtd_list_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;
	bool first = true;
	struct mtd_info *mtd;

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

	len = buf_appendf(buf, left, len, "{\"partitions\":[");

	/* Probe all MTD devices */
	mtd_probe_devices();

	mtd_for_each_device(mtd) {
		if (len >= left - 256)
			break;

		len = buf_appendf(buf, left, len,
			"%s{\"name\":\"%s\",\"size\":%llu,\"erasesize\":%lu}",
			first ? "" : ",",
			mtd->name,
			(unsigned long long)mtd->size,
			(unsigned long)mtd->erasesize);

		first = false;
	}

	len = buf_appendf(buf, left, len, "]}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_backup_handler - POST /ubi/backup
 *
 * Form parameters:
 *   name - Volume name to backup/download
 *
 * Returns the volume content as a binary download.
 */
void ubi_backup_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	struct ubi_backup_session {
		struct ubi_volume *vol;
		struct ubi_device *ubi;
		u64 total;
		u64 cur;
		void *buf;
		size_t buf_size;
		char hdr[512];
		int hdr_len;
	} *st;
	struct httpd_form_value *name_val;
	char *vol_name = NULL;
	char filename[128];
	int ret;

	failsafe_free_session(status, response);

	if (status == HTTP_CB_RESPONDING) {
		u64 remain;
		size_t to_read;

		st = response->session_data;
		if (!st) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		remain = st->total - st->cur;
		if (!remain) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		to_read = (size_t)min_t(u64, remain, st->buf_size);

		ret = ubi_volume_read(st->vol->name, st->buf, st->cur, to_read);
		if (ret) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		st->cur += to_read;

		response->status = HTTP_RESP_CUSTOM;
		response->data = (const char *)st->buf;
		response->size = to_read;
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;
		if (st) {
			free(st->buf);
			free(st);
		}
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	/* Get volume name */
	name_val = httpd_request_find_value(request, "name");
	if (!name_val || !name_val->data || !name_val->size) {
		failsafe_http_reply_text(response, 400, "missing name");
		return;
	}

	if (name_val->size > UBI_VOL_NAME_MAX_LEN) {
		failsafe_http_reply_text(response, 400, "name too long");
		return;
	}

	vol_name = malloc(name_val->size + 1);
	if (!vol_name) {
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	memcpy(vol_name, name_val->data, name_val->size);
	vol_name[name_val->size] = '\0';

	/* Check if UBI is attached */
	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		free(vol_name);
		failsafe_http_reply_text(response, 400, "no ubi device");
		return;
	}

	/* Find volume */
	struct ubi_volume *vol = ubi_find_volume(vol_name);
	if (!vol) {
		free(vol_name);
		failsafe_http_reply_text(response, 404, "volume not found");
		return;
	}

	/* Allocate session */
	st = calloc(1, sizeof(*st));
	if (!st) {
		free(vol_name);
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	st->buf_size = 64 * 1024;
	st->buf = malloc(st->buf_size);
	if (!st->buf) {
		free(st);
		free(vol_name);
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	st->vol = vol;
	st->ubi = ubi;
	st->total = (u64)vol->used_bytes;
	st->cur = 0;

	/* Generate filename */
	{
		char safe_name[64];
		const char *p;
		size_t i;

		/* Sanitize volume name for filename */
		p = vol_name;
		for (i = 0; i < sizeof(safe_name) - 1 && *p; i++, p++) {
			unsigned char c = *p;
			if (isalnum(c) || c == '-' || c == '_')
				safe_name[i] = c;
			else
				safe_name[i] = '_';
		}
		safe_name[i] = '\0';

		snprintf(filename, sizeof(filename), "ubi_%s.bin", safe_name);
	}

	free(vol_name);

	/* Build HTTP header */
	st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/octet-stream\r\n"
		"Content-Length: %llu\r\n"
		"Content-Disposition: attachment; filename=\"%s\"\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned long long)st->total,
		filename);

	response->session_data = st;
	response->status = HTTP_RESP_CUSTOM;
	response->data = st->hdr;
	response->size = st->hdr_len;
}

/**
 * ubi_check_vol_handler - POST /ubi/check
 *
 * Form parameters:
 *   name - Volume name to check
 *
 * Returns JSON: {"exists":true} or {"exists":false}
 */
void ubi_check_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Check volume existence */
	struct ubi_volume *vol = ubi_find_volume(name);
	free(name);

	json_out = malloc(64);
	if (json_out)
		snprintf(json_out, 64, "{\"exists\":%s}",
			vol ? "true" : "false");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"exists\":false}", json_out);
}

/**
 * ubi_write_vol_handler - POST /ubi/write
 *
 * Form parameters:
 *   name      - Volume name to write
 *   data      - File content to write (binary safe)
 *   offset    - Write offset in bytes (optional, default 0)
 *   full_size - Total size of the update (optional, for partial updates)
 *
 * Behavior:
 *   - offset > 0:   offset-based write at the given byte offset
 *   - offset == 0 && full_size > 0 && full_size != size:
 *                   begin a partial update declaring full_size as total
 *   - otherwise:    full volume update with size == full_size
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_write_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	struct httpd_form_value *name_val;
	struct httpd_form_value *data_val;
	char *vol_name = NULL;
	char *offset_str = NULL;
	char *full_str = NULL;
	char *json_out;
	loff_t offset = 0;
	size_t full_size = 0;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name (binary-safe form value) */
	name_val = httpd_request_find_value(request, "name");
	if (!name_val || !name_val->data || !name_val->size) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	if (name_val->size > UBI_VOL_NAME_MAX_LEN) {
		json_out = strdup("{\"error\":\"volume name too long\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"name too long\"}",
			json_out);
		return;
	}

	vol_name = malloc(name_val->size + 1);
	if (!vol_name) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	memcpy(vol_name, name_val->data, name_val->size);
	vol_name[name_val->size] = '\0';

	/* Get file data (binary safe) */
	data_val = httpd_request_find_value(request, "data");
	if (!data_val || !data_val->data || !data_val->size) {
		free(vol_name);
		json_out = strdup("{\"error\":\"missing data\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing data\"}",
			json_out);
		return;
	}

	/* Get optional offset */
	ret = failsafe_get_form_value(request, "offset", &offset_str, 32,
		true, true);
	if (ret == 0 && offset_str && offset_str[0]) {
		offset = (loff_t)simple_strtoull(offset_str, NULL, 0);
		if (offset < 0)
			offset = 0;
	}
	free(offset_str);

	/* Get optional full_size */
	ret = failsafe_get_form_value(request, "full_size", &full_str, 32,
		true, true);
	if (ret == 0 && full_str && full_str[0])
		full_size = (size_t)simple_strtoull(full_str, NULL, 0);
	free(full_str);

	/* Write data */
	if (offset > 0) {
		ret = ubi_volume_write(vol_name, data_val->data,
			offset, data_val->size);
	} else if (full_size > 0 && full_size != data_val->size) {
		ret = ubi_volume_begin_write(vol_name, data_val->data,
			data_val->size, full_size);
	} else {
		ret = ubi_volume_write(vol_name, data_val->data,
			0, data_val->size);
	}
	free(vol_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"write failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"write failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_skipcheck_handler - POST /ubi/skipcheck
 *
 * Form parameters:
 *   name - Volume name
 *   mode - "on" or "1" to enable skip check, "off" or "0" to disable
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_skipcheck_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *mode = NULL;
	char *json_out;
	bool skip_check;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Get mode */
	ret = failsafe_get_form_value(request, "mode", &mode, 8, false, false);
	if (ret || !mode || !mode[0]) {
		free(name);
		json_out = strdup("{\"error\":\"missing mode\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing mode\"}",
			json_out);
		return;
	}

	skip_check = (mode[0] == 'o' && mode[1] == 'n') ||
		     (mode[0] == '1');
	free(mode);

	/* Find volume */
	struct ubi_volume *vol = ubi_find_volume(name);
	if (!vol) {
		free(name);
		json_out = strdup("{\"error\":\"volume not found\"}");
		failsafe_http_reply_json_alloc(response, 404,
			json_out ? json_out : "{\"error\":\"not found\"}",
			json_out);
		return;
	}

	/* Set/clear skip check flag */
	ret = ubi_set_skip_check(name, skip_check);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"skipcheck failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"skipcheck failed\"}",
			json_out);
		return;
	}

	json_out = malloc(96);
	if (json_out)
		snprintf(json_out, 96, "{\"ok\":true,\"skip_check\":%s}",
			skip_check ? "true" : "false");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

#ifdef CONFIG_WEBUI_FAILSAFE_UBI
void ubi_register_handlers(struct httpd_instance *inst)
{
	/* The page and its script are registered by the page inventory
	 * (failsafe/pages.c); this module only owns the endpoints. */
	httpd_register_uri_handler(inst, "/ubi/info", &ubi_info_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/volumes", &ubi_volumes_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/attach", &ubi_attach_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/detach", &ubi_detach_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/rebuild", &ubi_rebuild_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/create", &ubi_create_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/remove", &ubi_remove_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/rename", &ubi_rename_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/check", &ubi_check_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/write", &ubi_write_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/skipcheck", &ubi_skipcheck_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/mtd_list", &ubi_mtd_list_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/backup", &ubi_backup_handler, NULL);
}
#endif
