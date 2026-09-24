/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe sysinfo module
 * Handles sysinfo (board, RAM, NAND chip model + partitions) and the
 * manual /atfversion debugging endpoint (flashed BL2 / BL31 banners)
 */

#include <env.h>
#include <malloc.h>
#include <net/mtk_httpd.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <dm/ofnode.h>
#include <vsprintf.h>

#ifdef CONFIG_MTD
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/spinand.h>
#endif

#include <failsafe/internal.h>

/*
 * SoC model reporting is available on Airoha (read from the NP-SCU
 * registers) and MediaTek (derived from the CONFIG_TARGET_MT798x build
 * target, no runtime detection needed).
 */
#if defined(CONFIG_ARCH_AIROHA) || defined(CONFIG_ARCH_MEDIATEK)
#define FAILSAFE_SOC_NAME_ENABLED	1
#endif

/*
 * The ATF (BL2 / BL31) version banners come from Trusted Firmware-A, which
 * only exists on ARM.  Other architectures have no such boot stage, so the
 * Web UI must not offer the read-out there: /sysinfo reports the capability
 * and renderSysInfo() in main.js leaves the version row out unless it is
 * there.  The manual /atfversion endpoint itself stays on every
 * architecture.
 */
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#define FAILSAFE_ATF_INFO_ENABLED	1
#endif

#ifdef CONFIG_ARCH_AIROHA
#include <linux/err.h>
#include <soc/airoha/pkgids.h>
#endif

/* ------------------------------------------------------------------ */
/*  sysinfo sub-functions                                              */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ARCH_AIROHA
/*
 * Read the exact SoC model from the NP-SCU registers, mirroring the logic
 * in print_cpuinfo() in the per-family init.c under arch/arm/mach-airoha.
 * Returns an empty string when the registers cannot be read.
 */
static const char *sysinfo_soc_name(char *out, size_t out_sz)
{
	struct regmap *np_scu;
	u32 hir = 0;
	u32 pdidr = 0;
	u32 pkgid = END_PACKAGE_ID;
	u32 value;
	const char *name;

	if (!out || !out_sz)
		return "";
	out[0] = '\0';

	np_scu = airoha_get_scu_regmap();
	if (IS_ERR_OR_NULL(np_scu))
		return "";

	if (!regmap_read(np_scu, AIROHA_NP_SCU_HIR, &value))
		hir = FIELD_GET(AIROHA_NP_SCU_HIR_MASK, value);
	if (!regmap_read(np_scu, AIROHA_NP_SCU_PDIDR, &value))
		pdidr = FIELD_GET(AIROHA_NP_SCU_PDIDR_MASK, value);
	if (!regmap_read(np_scu, AIROHA_NP_SCU_SCREG_WR1, &value))
		pkgid = airoha_pkgid_from_screg(value);

	name = airoha_soc_name_from_regs(hir, pkgid, pdidr);
	if (!name || !name[0] || !strcmp(name, "unknown"))
		return "";

	snprintf(out, out_sz, "%s", name);
	return out;
}
#endif /* CONFIG_ARCH_AIROHA */

#ifdef CONFIG_ARCH_MEDIATEK
/*
 * MediaTek SoCs are identified at build time: the SoC model follows the
 * selected CONFIG_TARGET_MT798x (mt7981 / mt7986 / mt7987 / mt7988),
 * there are no runtime registers to consult.
 */
static const char *sysinfo_soc_name(char *out, size_t out_sz)
{
	const char *name = NULL;

	if (!out || !out_sz)
		return "";
	out[0] = '\0';

#if defined(CONFIG_TARGET_MT7981)
	name = "mt7981";
#elif defined(CONFIG_TARGET_MT7986)
	name = "mt7986";
#elif defined(CONFIG_TARGET_MT7987)
	name = "mt7987";
#elif defined(CONFIG_TARGET_MT7988)
	name = "mt7988";
#endif

	if (!name)
		return "";

	snprintf(out, out_sz, "%s", name);
	return out;
}
#endif /* CONFIG_ARCH_MEDIATEK */

static int sysinfo_json_append_board(char *buf, int len, int left)
{
	ofnode root;
	const char *board_model = NULL;
	const char *board_compat = NULL;
	const char *build_variant = NULL;
	off_t ram_size = 0;
	char esc_board_model[256], esc_board_compat[256], esc_build_variant[256];
#ifdef FAILSAFE_SOC_NAME_ENABLED
	char soc_buf[128], esc_soc[128];
	const char *soc_name = "";
#endif

	root = ofnode_path("/");
	if (ofnode_valid(root)) {
		board_model = ofnode_read_string(root, "model");
		board_compat = ofnode_read_string(root, "compatible");
	}

	if (!board_model || !board_model[0]) {
		board_model = env_get("model");
		if (!board_model || !board_model[0])
			board_model = env_get("board_name");
		if (!board_model || !board_model[0])
			board_model = env_get("board");
	}

	if (gd)
		ram_size = (off_t)gd->ram_size;

	build_variant = CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT;
	if (build_variant && !build_variant[0])
		build_variant = NULL;

	json_escape(esc_board_model, sizeof(esc_board_model), board_model ? board_model : "");
	json_escape(esc_board_compat, sizeof(esc_board_compat), board_compat ? board_compat : "");
	json_escape(esc_build_variant, sizeof(esc_build_variant), build_variant ? build_variant : "");
#ifdef FAILSAFE_SOC_NAME_ENABLED
	soc_name = sysinfo_soc_name(soc_buf, sizeof(soc_buf));
	json_escape(esc_soc, sizeof(esc_soc), soc_name ? soc_name : "");
#endif

	len = buf_appendf(buf, left, len,
		"\"board\":{\"model\":\"%s\",\"compatible\":\"%s\"},",
		esc_board_model, esc_board_compat);
	len = buf_appendf(buf, left, len,
		"\"ram\":{\"size\":%llu},",
		(unsigned long long)ram_size);
#ifdef FAILSAFE_SOC_NAME_ENABLED
	len = buf_appendf(buf, left, len,
		"\"soc\":\"%s\",",
		esc_soc);
#endif
	len = buf_appendf(buf, left, len,
		"\"build_variant\":\"%s\"",
		esc_build_variant);

#ifdef FAILSAFE_ATF_INFO_ENABLED
	len = buf_appendf(buf, left, len, ",\"atf\":true");
#else
	len = buf_appendf(buf, left, len, ",\"atf\":false");
#endif

	return len;
}

void sysinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = 8192;

	(void)request;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{}");
		return;
	}

	len = buf_appendf(buf, left, len, "{");

	/* board + RAM + build_variant */
	len = sysinfo_json_append_board(buf, len, left);

	len = buf_appendf(buf, left, len, "}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/* ------------------------------------------------------------------ */
/*  sysinfo/nand handler (SPI NAND chip model + partitions)            */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MTD
#if IS_ENABLED(CONFIG_MTD_SPI_NAND)
static const struct spinand_info *sysinfo_spinand_match_info(struct spinand_device *spinand)
{
	size_t i;
	const struct spinand_manufacturer *manufacturer;
	const u8 *id;

	if (!spinand)
		return NULL;

	manufacturer = spinand->manufacturer;
	if (!manufacturer || !manufacturer->chips || !manufacturer->nchips)
		return NULL;

	id = spinand->id.data;

	for (i = 0; i < manufacturer->nchips; i++) {
		const struct spinand_info *info = &manufacturer->chips[i];

		if (!info->devid.id || !info->devid.len)
			continue;

		/* spinand->id.data[0] is manufacturer ID, device ID starts from [1]. */
		if (spinand->id.len < (int)(1 + info->devid.len))
			continue;

		if (!memcmp(id + 1, info->devid.id, info->devid.len))
			return info;
	}

	return NULL;
}
#endif /* CONFIG_MTD_SPI_NAND */

static const char *sysinfo_get_mtd_chip_model(struct mtd_info *mtd, char *out,
					      size_t out_sz)
{
	if (!out || !out_sz)
		return "";

	out[0] = '\0';

	if (!mtd)
		return "";

#if IS_ENABLED(CONFIG_MTD_SPI_NAND)
	/* SPI NAND: mtd->priv points to struct nand_device embedded in
	 * struct spinand_device. */
	if (mtd->type == MTD_NANDFLASH || mtd->type == MTD_MLCNANDFLASH) {
		struct spinand_device *spinand = mtd_to_spinand(mtd);
		const struct spinand_manufacturer *manufacturer;
		const struct spinand_info *info;
		const char *mname = NULL;
		const char *model = NULL;

		if (spinand) {
			manufacturer = spinand->manufacturer;
			info = sysinfo_spinand_match_info(spinand);

			if (manufacturer && manufacturer->name &&
			    manufacturer->name[0])
				mname = manufacturer->name;
			if (info && info->model && info->model[0])
				model = info->model;

			if (mname && model) {
				snprintf(out, out_sz, "%s %s", mname, model);
				return out;
			}
			if (model) {
				snprintf(out, out_sz, "%s", model);
				return out;
			}
			if (mname) {
				snprintf(out, out_sz, "%s", mname);
				return out;
			}
		}
	}
#endif

	/* Fallback: use MTD device name. */
	if (mtd->name && mtd->name[0]) {
		snprintf(out, out_sz, "%s", mtd->name);
		return out;
	}

	return "";
}
#endif /* CONFIG_MTD */

void sysinfo_nand_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = 16384;

	(void)request;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{}");
		return;
	}

	len = buf_appendf(buf, left, len, "{");

	/* NAND info + partitions */
	len = buf_appendf(buf, left, len, "\"nand\":{");
#ifdef CONFIG_MTD
	{
		struct mtd_info *mtd, *sel = NULL;
		u32 i;
		bool first = true;
		const char *model = NULL;
		char model_buf[128];
		char esc_model[128];
		int type = -1;
		bool present = false;

		mtd_probe_devices();

		/* Prefer a master MTD device (mtd->parent == NULL) for chip model info. */
		for (i = 0; i < 64; i++) {
			mtd = get_mtd_device(NULL, i);
			if (IS_ERR(mtd))
				continue;

			if (!sel) {
				sel = mtd;
			} else {
				if (mtd->parent) {
					put_mtd_device(mtd);
					continue;
				}

				/* Found master: replace current selection. */
				put_mtd_device(sel);
				sel = mtd;
				break;
			}

			if (!mtd->parent)
				break;
		}

		if (sel && !IS_ERR(sel)) {
			present = true;
			type = sel->type;
			model = sysinfo_get_mtd_chip_model(sel, model_buf,
							   sizeof(model_buf));
			put_mtd_device(sel);
		}

		json_escape(esc_model, sizeof(esc_model), model ? model : "");
		len = buf_appendf(buf, left, len,
			"\"present\":%s,\"model\":\"%s\",\"type\":%d,",
			present ? "true" : "false",
			esc_model, type);

		len = buf_appendf(buf, left, len, "\"parts\":[");
		for (i = 0; i < 64 && len < left - 128; i++) {
			char esc_name[128];

			mtd = get_mtd_device(NULL, i);
			if (IS_ERR(mtd))
				continue;

			if (!mtd->name || !mtd->name[0]) {
				put_mtd_device(mtd);
				continue;
			}

			json_escape(esc_name, sizeof(esc_name), mtd->name);
			len = buf_appendf(buf, left, len,
				"%s{\"name\":\"%s\",\"size\":%llu,\"master\":%s}",
				first ? "" : ",",
				esc_name,
				(unsigned long long)mtd->size,
				mtd->parent ? "false" : "true");

			first = false;
			put_mtd_device(mtd);
		}
		len = buf_appendf(buf, left, len, "]");
	}
#else
	len = buf_appendf(buf, left, len, "\"present\":false,\"parts\":[]");
#endif
	len = buf_appendf(buf, left, len, "}");
	len = buf_appendf(buf, left, len, "}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/* ------------------------------------------------------------------ */
/*  atfversion handler (manual debug endpoint)                         */
/* ------------------------------------------------------------------ */

/*
 * GET /atfversion - version and build-date banners of the BL2 and BL31
 * images currently stored in flash.
 *
 * Manual debugging aid: no Web UI page uses it, so it just answers with
 * plain text (like /version).  The banners are read back from the
 * flashed boot chain, which makes it easy to confirm what a device is
 * really running after an update.
 *
 * The BL31 half is optional (CONFIG_WEBUI_FAILSAFE_BL31): when it is
 * disabled the board hook reports no BL31 banner and the BL31 lines are
 * left out of the reply entirely.
 */
void atfversion_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	static struct failsafe_version_info bl2, bl31;
	static char buf[512];
	int len = 0;

	(void)request;

	if (status != HTTP_CB_NEW)
		return;

	memset(&bl2, 0, sizeof(bl2));
	memset(&bl31, 0, sizeof(bl31));
	failsafe_atf_version_info(&bl2, &bl31);

	len = buf_appendf(buf, sizeof(buf), len,
		"BL2 version: %s\n"
		"BL2 build date: %s\n",
		bl2.version[0] ? bl2.version : "n/a",
		bl2.build_date[0] ? bl2.build_date : "n/a");

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	len = buf_appendf(buf, sizeof(buf), len,
		"BL31 version: %s\n"
		"BL31 build date: %s\n",
		bl31.version[0] ? bl31.version : "n/a",
		bl31.build_date[0] ? bl31.build_date : "n/a");
#endif

	failsafe_http_reply_text(response, 200, buf);
}

/* ------------------------------------------------------------------ */
/*  Public registration function                                       */
/* ------------------------------------------------------------------ */

void sysinfo_register_handlers(struct httpd_instance *inst)
{
	httpd_register_uri_handler(inst, "/sysinfo", &sysinfo_handler, NULL);
	httpd_register_uri_handler(inst, "/sysinfo/nand", &sysinfo_nand_handler, NULL);
	httpd_register_uri_handler(inst, "/atfversion", &atfversion_handler, NULL);
}
