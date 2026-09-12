/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Interface for the shared failsafe BL2 (preloader) helper.
 *
 * The version banner parser and the "locate the BL2 payload inside a
 * boot image" walk are identical on Airoha and MediaTek, so they live
 * here instead of in each board file.
 */

#ifndef _FAILSAFE_BL2_H_
#define _FAILSAFE_BL2_H_

#include <linux/types.h>

#define FAILSAFE_BL2_VERSION_MAX	128
#define FAILSAFE_BL2_DATE_MAX		64

/**
 * struct failsafe_bl2_info - version banner of a BL2 (preloader) image
 * @found: true when at least the version or the build date was extracted
 * @version: version string, e.g. "v2.10.0 (release):00dba2b"
 * @build_date: build date/time, e.g. "14:09:01, Sep 11 2026"
 *
 * Both strings are empty when nothing could be extracted.
 */
struct failsafe_bl2_info {
	bool found;
	char version[FAILSAFE_BL2_VERSION_MAX];
	char build_date[FAILSAFE_BL2_DATE_MAX];
};

/**
 * failsafe_bl2_parse_banner() - extract the preloader version banner
 * @data: raw BL2 region
 * @size: region size in bytes
 * @info: output structure, fully overwritten
 *
 * Mirrors tools/airoha_info_preloader.py without regular expressions:
 *   - a printable run matching "v<digits>.<digit>..." is the version,
 *   - when that run carries no '(', the run right after it is appended
 *     (reassembles "v2.10.0" + "(release):00dba2b"),
 *   - a run starting with "Built" carries the build date/time.
 *
 * The strict "v<digits>.<digit>" shape matters: binary data contains
 * plenty of short runs like "v3z," that must not be mistaken for a
 * version banner.
 *
 * Returns true when at least the version or the build date was found.
 */
bool failsafe_bl2_parse_banner(const void *data, size_t size,
			       struct failsafe_bl2_info *info);

/**
 * failsafe_bl2_locate() - locate the BL2 payload inside a boot image
 * @data: boot image contents
 * @size: boot image size in bytes
 * @fip_offsets: FIP offsets to probe, in order
 * @num_offsets: number of entries in @fip_offsets
 * @bl2: receives a pointer to the BL2 payload
 * @bl2_size: receives the BL2 payload size
 *
 * The legacy 512 KiB Airoha image keeps its internal FIP at 0x800 while
 * the split preloader.bin keeps it at 0; both carry BL2 as the "tb-fw"
 * ToC entry.  When no usable FIP/BL2 entry is found the whole image is
 * reported, which also covers a bare bl2.bin upload.
 */
void failsafe_bl2_locate(const void *data, size_t size,
			 const size_t *fip_offsets, size_t num_offsets,
			 const void **bl2, size_t *bl2_size);

#endif /* _FAILSAFE_BL2_H_ */
