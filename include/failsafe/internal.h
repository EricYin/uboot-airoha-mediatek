/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Internal interfaces for Failsafe Web UI modules
 */

#ifndef _FAILSAFE_INTERNAL_H_
#define _FAILSAFE_INTERNAL_H_

#include <net/mtk_httpd.h>
#include <linux/types.h>
#include <failsafe/fw_type.h>
#include <failsafe/helpers.h>

/* ------------------------------------------------------------------ */
/*  Core weak functions (defined in failsafe.c, used by modules)       */
/* ------------------------------------------------------------------ */

int failsafe_validate_image(const void *data, size_t size,
			    failsafe_fw_t fw);
int failsafe_write_image(const void *data, size_t size,
			 failsafe_fw_t fw);

/**
 * boot_from_mem() - boot an uploaded image staged in DRAM (RAM boot).
 *
 * The board-level implementation auto-detects the image type: FIT /
 * legacy uImage images (an initramfs) are booted with bootm, while any
 * other raw binary is copied to the "loadaddr" address and executed
 * directly with the "go" command.
 *
 * Implemented in board/airoha/common/failsafe.c and
 * board/mediatek/common/failsafe.c.  Airoha does not use the MediaTek
 * bootmenu framework (boot_helper.c is never compiled), so the
 * declaration lives here instead of the MTK board header.
 */
int boot_from_mem(ulong data_load_addr);

/**
 * failsafe_notify_network_cmd_done() - signal that a network command finished
 *
 * Called from telnetd after executing a network command (tftp, ping, etc.)
 * whose inner net_loop() calls eth_halt() on exit.
 *
 * The poll loop responds by calling eth_init() OUTSIDE the eth_rx() →
 * TCP callback chain, avoiding DMA receive-descriptor corruption that
 * occurs when eth_init() is called inline from within a TCP callback.
 * It also re-registers the DHCP UDP handler that net_clear_handlers()
 * removed.
 */
void failsafe_notify_network_cmd_done(void);

/* ------------------------------------------------------------------ */
/*  Handler declarations (used by failsafe.c for URI registration)     */
/* ------------------------------------------------------------------ */

/* ---- core handlers (failsafe_core.c) ---- */
void js_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void html_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

/* ---- sysinfo handlers (sysinfo.c) ---- */
void sysinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void sysinfo_nand_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

/* ---- upgrade handlers (upgrade.c) ---- */
void version_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void reboot_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void reboot_failsafe_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void upload_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void result_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

/* ---- sub-module handlers ---- */

/* theme */
void picture_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

#ifdef CONFIG_WEBUI_FAILSAFE_CONSOLE
int failsafe_webconsole_ensure_recording(void);
extern bool webconsole_exec_busy;
void webconsole_poll_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void webconsole_exec_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void webconsole_clear_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
#endif

#ifdef CONFIG_WEBUI_FAILSAFE_ENV
void env_list_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void env_set_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void env_unset_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void env_reset_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void env_restore_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void env_size_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void theme_get_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void theme_set_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
#endif

#ifdef CONFIG_WEBUI_FAILSAFE_UBI
void ubi_info_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_volumes_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_attach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_detach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_create_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_remove_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_rename_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_mtd_list_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void ubi_backup_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
#endif

#ifdef CONFIG_WEBUI_FAILSAFE_SIMG
void simg_info_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
void simg_write_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);
#endif

/* ------------------------------------------------------------------ */
/*  Module registration functions                                      */
/* ------------------------------------------------------------------ */

/* Always compiled */
void upgrade_register_handlers(struct httpd_instance *inst);

/* Conditionally compiled */
#ifdef CONFIG_WEBUI_FAILSAFE_ADVANCED
void sysinfo_register_handlers(struct httpd_instance *inst);
#endif
#ifdef CONFIG_WEBUI_FAILSAFE_ENV
void env_register_handlers(struct httpd_instance *inst);
#endif
#ifdef CONFIG_WEBUI_FAILSAFE_UI_BOOTSTRAP
void theme_register_handlers(struct httpd_instance *inst);
#endif
#ifdef CONFIG_WEBUI_FAILSAFE_UBI
void ubi_register_handlers(struct httpd_instance *inst);
#endif
#ifdef CONFIG_WEBUI_FAILSAFE_CONSOLE
void console_register_handlers(struct httpd_instance *inst);
#endif
#ifdef CONFIG_WEBUI_FAILSAFE_SIMG
void simg_register_handlers(struct httpd_instance *inst);
#endif

#endif /* _FAILSAFE_INTERNAL_H_ */
