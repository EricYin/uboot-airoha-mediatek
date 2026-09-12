/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Shared version-banner structure for the failsafe boot-image helpers.
 *
 * Every TF-A stage image of the boot chain embeds the same kind of
 * banner (a version token plus a "Built : <date>" string), so BL2
 * (preloader) and BL31 share this structure as well as the extraction
 * rules implemented in failsafe/bootimg/.
 */

#ifndef _FAILSAFE_VERSION_H_
#define _FAILSAFE_VERSION_H_

#include <linux/types.h>

#define FAILSAFE_VERSION_MAX		128
#define FAILSAFE_VERSION_DATE_MAX	64

/**
 * struct failsafe_version_info - version banner of a boot-chain image
 * @found: true when at least the version or the build date was extracted
 * @version: version string, e.g. "v2.10.0 (release):00dba2b"
 * @build_date: build date/time, e.g. "14:09:01, Sep 11 2026"
 *
 * Both strings are empty when nothing could be extracted.
 */
struct failsafe_version_info {
	bool found;
	char version[FAILSAFE_VERSION_MAX];
	char build_date[FAILSAFE_VERSION_DATE_MAX];
};

#endif /* _FAILSAFE_VERSION_H_ */
