// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe Web UI - firmware upload / flash / RAM boot
 *
 * Airoha adaptation: this module was extracted from the original
 * monolithic failsafe.c (renamed to upgrade.c when the failsafe
 * framework was modularised).  Only the upgrade core is kept here:
 *
 *   - upload_handler(): receive an uploaded image, derive its target
 *     type from the multipart form field name, validate it through the
 *     board-level failsafe_validate_image() hook and report
 *     "size MD5" to the Web UI.
 *
 *   - result_handler(): commit the staged image to the selected MTD
 *     partition / UBI volume through failsafe_write_image(), or hand it
 *     to the RAM-boot path for an uploaded image (booted by
 *     boot_from_mem() from failsafe_core.c: FIT / legacy images via
 *     bootm, other raw binaries via the "go" command), and report
 *     success / failed.
 *
 * The shared state (upload_data_id / upload_data / upload_size /
 * upgrade_success / auto_action_pending / fw_type) is referenced by
 * failsafe_core.c.  The HTTP entry point (do_httpd), the Web page
 * handlers and the RAM-boot staging live in failsafe_core.c, sysinfo.c /
 * theme.c and the board layer respectively.
 *
 * Airoha supports only: firmware (UBI volume), bl2, chainloader and
 * u-boot (MTD partitions), fip (UBI volume) and RAM boot of an uploaded
 * image (initramfs FIT via bootm or a raw binary via "go").
 * No MediaTek-specific features (MTD layout switching, MMC, GPT, single
 * image or factory images) are present.
 */

#include <command.h>
#include <env.h>
#include <linux/string.h>
#include <linux/types.h>
#include <malloc.h>
#include <net/mtk_httpd.h>
#include <net/mtk_tcp.h>
#include <rand.h>
#include <stdio.h>
#include <u-boot/md5.h>
#include <version_string.h>
#include <vsprintf.h>

#include <failsafe/fw_type.h>
#include <failsafe/internal.h>
#include <failsafe/led.h>

/* ------------------------------------------------------------------ */
/*  Version handler                                                    */
/* ------------------------------------------------------------------ */

#ifndef WEBUI_FAILSAFE_GIT_HASH
#define WEBUI_FAILSAFE_GIT_HASH "unknown"
#endif

#ifndef WEBUI_FAILSAFE_GIT_DIRTY
#define WEBUI_FAILSAFE_GIT_DIRTY 0
#endif

void version_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	const char *build_variant;
	const char *git_hash = WEBUI_FAILSAFE_GIT_HASH;
	static char version_buf[512];
	bool dirty = !!WEBUI_FAILSAFE_GIT_DIRTY;

	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;

	build_variant = CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT;
	if (!git_hash || !git_hash[0])
		git_hash = "unknown";

	if (build_variant && build_variant[0]) {
		snprintf(version_buf, sizeof(version_buf),
			 "%s %s%s %s",
			 version_string, git_hash, dirty ? "-dirty" : "",
			 build_variant);
		response->data = version_buf;
	} else {
		snprintf(version_buf, sizeof(version_buf),
			 "%s %s%s",
			 version_string, git_hash, dirty ? "-dirty" : "");
		response->data = version_buf;
	}
	response->size = strlen(response->data);

	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";
}

/* ------------------------------------------------------------------ */
/*  Reboot handlers                                                    */
/* ------------------------------------------------------------------ */

struct reboot_session {
	bool do_reboot;
};

/*
 * Set once a /reboot or /reboot-failsafe request has been answered and its
 * connection has been fully closed.  The reset itself is performed by
 * do_httpd() after the poll loop has exited.
 *
 * Calling do_reset() here would run it -- and mtk_tcp_close_all_conn() --
 * from inside the TCP callback, i.e. inside the eth_rx() → connection
 * teardown chain.  The teardown of the very connection running this
 * callback then re-entered the callback, which recursed until the stack
 * blew up, printing its "Rebooting now" line over and over without ever
 * reaching do_reset().
 */
bool reboot_pending;

/*
 * Consume the reboot session of a closed connection.
 *
 * session_data is detached before it is freed: the connection teardown
 * path can deliver HTTP_CB_CLOSED more than once for the same session,
 * and reading or freeing an already released session turned the reboot
 * into an unbounded recursion.
 */
static bool reboot_session_take(struct httpd_response *response)
{
	struct reboot_session *st = response->session_data;
	bool do_reboot = false;

	if (st) {
		do_reboot = st->do_reboot;
		response->session_data = NULL;
		free(st);
	}

	return do_reboot;
}

void reboot_handler(enum httpd_uri_handler_status status,
			   struct httpd_request *request,
			   struct httpd_response *response)
{
	struct reboot_session *st;

	if (status == HTTP_CB_NEW) {
		st = calloc(1, sizeof(*st));
		if (!st) {
			response->info.code = 500;
			return;
		}

		st->do_reboot = true;

		response->session_data = st;
		response->status = HTTP_RESP_STD;
		response->data = "rebooting";
		response->size = strlen(response->data);
		response->info.code = 200;
		response->info.connection_close = 1;
		response->info.content_type = "text/plain";
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		/*
		 * Only record the request.  do_httpd() performs the reset
		 * after the poll loop has exited and the network has been
		 * halted, i.e. safely outside the TCP callback chain.
		 */
		if (reboot_session_take(response))
			reboot_pending = true;
	}
}

void reboot_failsafe_handler(enum httpd_uri_handler_status status,
				   struct httpd_request *request,
				   struct httpd_response *response)
{
	struct reboot_session *st;
	int ret;

	if (status == HTTP_CB_NEW) {
		ret = env_set("failsafe", "1");
		if (!ret)
			ret = env_save();

		if (ret) {
			response->status = HTTP_RESP_STD;
			response->data = "failsafe env set failed";
			response->size = strlen(response->data);
			response->info.code = 500;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		st = calloc(1, sizeof(*st));
		if (!st) {
			response->info.code = 500;
			return;
		}

		st->do_reboot = true;
		response->session_data = st;
		response->status = HTTP_RESP_STD;
		response->data = "rebooting to failsafe";
		response->size = strlen(response->data);
		response->info.code = 200;
		response->info.connection_close = 1;
		response->info.content_type = "text/plain";
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		if (reboot_session_take(response))
			reboot_pending = true;
	}
}

/* ------------------------------------------------------------------ */
/*  Shared state (referenced by failsafe_core.c)                       */
/* ------------------------------------------------------------------ */

u32 upload_data_id;
const void *upload_data;
size_t upload_size;
bool upgrade_success;
bool auto_action_pending;
failsafe_fw_t fw_type;

/* ------------------------------------------------------------------ */
/*  Auto-reboot helper                                                 */
/* ------------------------------------------------------------------ */

/*
 * The Bootstrap UI shows a "Reboot" button after a successful upgrade and
 * lets the user trigger the reboot explicitly, so no automatic reboot by
 * default.  Set failsafe_auto_reboot=1 to restore the legacy always-reboot
 * behaviour.  An uploaded initramfs is always booted immediately.
 */
static bool failsafe_auto_reboot_enabled(void)
{
	const char *val = env_get("failsafe_auto_reboot");

	if (!val || !val[0])
		return false;

	if (!strcmp(val, "1"))
		return true;

	return false;
}

/* ------------------------------------------------------------------ */
/*  Upload handler                                                     */
/* ------------------------------------------------------------------ */

void upload_handler(enum httpd_uri_handler_status status,
		    struct httpd_request *request,
		    struct httpd_response *response)
{
	static char md5_str[33] = "";
	static char resp[288];
	struct httpd_form_value *fw;
	u8 md5_sum[16];
	static char hexchars[] = "0123456789abcdef";
	int i;

	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";

	/*
	 * The upload is fully received here: show the "upgrade in
	 * progress" LED effect until /result has committed (or rejected)
	 * the image.
	 */
	failsafe_led_set_phase(FAILSAFE_LED_UPGRADE);

	fw = httpd_request_find_value(request, "fip");
	if (fw) {
		fw_type = FW_TYPE_FIP;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

	fw = httpd_request_find_value(request, "bl2");
	if (fw) {
		fw_type = FW_TYPE_BL2;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

	fw = httpd_request_find_value(request, "chainloader");
	if (fw) {
		fw_type = FW_TYPE_CHAINLOADER;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

	fw = httpd_request_find_value(request, "uboot");
	if (fw) {
		fw_type = FW_TYPE_UBOOT;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

	fw = httpd_request_find_value(request, "firmware");
	if (fw) {
		fw_type = FW_TYPE_FW;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

	fw = httpd_request_find_value(request, "initramfs");
	if (fw) {
		/* RAM-boot upload (no flash target).  boot_from_mem() decides
		 * later, based on the image header, between bootm (FIT /
		 * legacy uImage initramfs) and "go" (raw binary at loadaddr).
		 */
		fw_type = FW_TYPE_INITRD;
		if (failsafe_validate_image(fw->data, fw->size, fw_type))
			goto fail;
		goto done;
	}

fail:
	failsafe_led_set_phase(FAILSAFE_LED_FAIL);
	response->data = "fail";
	response->size = strlen(response->data);
	return;

done:
	upload_data_id = upload_id;
	upload_data = fw->data;
	upload_size = fw->size;

	md5_wd((u8 *)fw->data, fw->size, md5_sum, 0);

	for (i = 0; i < 16; i++) {
		u8 hex = (md5_sum[i] >> 4) & 0xf;

		md5_str[i * 2] = hexchars[hex];
		hex = md5_sum[i] & 0xf;
		md5_str[i * 2 + 1] = hexchars[hex];
	}

	/*
	 * The first line keeps the historical "<size> <md5>" format (other
	 * consumers only look at it); when the board can extract the BL2
	 * (preloader) banner of the uploaded image, it follows on its own
	 * lines, one "key:value" pair per field:
	 *
	 *     <size> <md5>
	 *     bl2_version:<version>
	 *     bl2_date:<build date>
	 */
	{
		struct failsafe_bl2_info bl2;
		int len = 0;

		len = buf_appendf(resp, sizeof(resp), len, "%zu %s",
				  fw->size, md5_str);

		memset(&bl2, 0, sizeof(bl2));
		if (!failsafe_bl2_version_info(fw->data, fw->size, fw_type,
					       &bl2) && bl2.found) {
			if (bl2.version[0])
				len = buf_appendf(resp, sizeof(resp), len,
						  "\nbl2_version:%s",
						  bl2.version);
			if (bl2.build_date[0])
				len = buf_appendf(resp, sizeof(resp), len,
						  "\nbl2_date:%s",
						  bl2.build_date);
		}

		response->data = resp;
		response->size = len;
	}
}

/* ------------------------------------------------------------------ */
/*  Result handler (flashing / RAM boot)                               */
/* ------------------------------------------------------------------ */

struct flashing_status {
	char buf[4096];
	int ret;
	int body_sent;
};

void result_handler(enum httpd_uri_handler_status status,
		    struct httpd_request *request,
		    struct httpd_response *response)
{
	struct flashing_status *st;
	u32 size;

	if (status == HTTP_CB_NEW) {
		st = calloc(1, sizeof(*st));
		if (!st) {
			response->info.code = 500;
			return;
		}

		st->ret = -1;

		response->session_data = st;

		response->status = HTTP_RESP_CUSTOM;

		response->info.http_1_0 = 1;
		response->info.content_length = -1;
		response->info.connection_close = 1;
		response->info.content_type = "text/html";
		response->info.code = 200;

		size = http_make_response_header(&response->info, st->buf,
						 sizeof(st->buf));

		response->data = st->buf;
		response->size = size;

		return;
	}

	if (status == HTTP_CB_RESPONDING) {
		st = response->session_data;

		if (st->body_sent) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		if (upload_data_id == upload_id) {
			if (fw_type == FW_TYPE_INITRD) {
				st->ret = 0; /* RAM boot, nothing to flash */
			} else {
				failsafe_led_set_phase(FAILSAFE_LED_UPGRADE);
				st->ret = failsafe_write_image(upload_data,
							       upload_size,
							       fw_type);
			}

			/* report the outcome with the configured effect */
			failsafe_led_set_phase(st->ret ? FAILSAFE_LED_FAIL :
							FAILSAFE_LED_SUCCESS);
		}

		/* invalidate upload identifier */
		upload_data_id = rand();

		if (!st->ret)
			response->data = "success";
		else
			response->data = "failed";

		response->size = strlen(response->data);

		st->body_sent = 1;

		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;

		upgrade_success = !st->ret;
		auto_action_pending = upgrade_success &&
			(fw_type == FW_TYPE_INITRD ||
			 failsafe_auto_reboot_enabled());

		free(response->session_data);
	}
}

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

void upgrade_register_handlers(struct httpd_instance *inst)
{
	httpd_register_uri_handler(inst, "/upload", &upload_handler, NULL);
	httpd_register_uri_handler(inst, "/result", &result_handler, NULL);
	httpd_register_uri_handler(inst, "/version", &version_handler, NULL);
	httpd_register_uri_handler(inst, "/reboot", &reboot_handler, NULL);
	httpd_register_uri_handler(inst, "/reboot-failsafe", &reboot_failsafe_handler, NULL);
}
