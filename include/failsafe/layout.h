/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Flash layouts described by the board device tree ("failsafe-layout").
 *
 * A board may carry more than one flash layout: the one its stock firmware
 * uses, and the one a replacement boot chain installs.  Each layout is a list
 * of named ranges of the raw device, so the recovery can edit a device *in a
 * layout it does not currently run* - which is how a device is moved from one
 * layout to another:
 *
 *	multi_layout {
 *		compatible = "failsafe-layout";
 *		#address-cells = <1>;
 *		#size-cells = <1>;
 *
 *		layout@0 {
 *			label = "stock";
 *
 *			partition@0 {
 *				label = "bootloader";
 *				reg = <0x0 0x80000>;
 *			};
 *			...
 *		};
 *
 *		layout@1 {
 *			label = "tcboot";
 *			...
 *		};
 *	};
 *
 * The offsets are raw device offsets: which device they apply to is the
 * caller's decision (the master MTD chip, the user area of an SD / eMMC
 * device, ...).  A partition whose size is 0 reaches the end of the device.
 *
 * The labels point into the control device tree, which stays mapped for the
 * whole session, so what failsafe_layout_parse() returns stays usable.
 */

#ifndef _FAILSAFE_LAYOUT_H_
#define _FAILSAFE_LAYOUT_H_

#include <linux/types.h>

/* Names of the node holding the layouts and of its properties. */
#define FAILSAFE_LAYOUT_COMPATIBLE	"failsafe-layout"
#define FAILSAFE_LAYOUT_PROP_LABEL	"label"
#define FAILSAFE_LAYOUT_PROP_REG	"reg"

/* Upper bounds of what a board may describe (keeps the caller's arrays and
 * the HTTP responses bounded).
 */
#define FAILSAFE_LAYOUT_MAX		4
#define FAILSAFE_LAYOUT_MAX_PARTS	28

struct failsafe_layout_part {
	const char *label;
	u64 offset;		/* raw device offset */
	u64 size;		/* 0: reaches the end of the device */
};

struct failsafe_layout {
	const char *label;	/* layout name, as reported to the user */
	int num_parts;
	struct failsafe_layout_part parts[FAILSAFE_LAYOUT_MAX_PARTS];
};

/**
 * failsafe_layout_parse() - read the layouts of the control device tree
 * @layouts: array to fill
 * @max: number of entries @layouts can hold
 *
 * The "failsafe-layout" node is optional, so a board without one simply has
 * no layouts.  Partitions whose "reg" does not fit the #address-cells /
 * #size-cells in effect are read as a plain <offset size> pair when they have
 * two cells, and reported as skipped otherwise.
 *
 * Returns the number of layouts found (never more than @max), or 0 when the
 * board describes none.
 */
int failsafe_layout_parse(struct failsafe_layout *layouts, int max);

/**
 * failsafe_layout_find_part() - look up a partition by label
 * @layout: a layout filled in by failsafe_layout_parse()
 * @label: partition label to look for
 *
 * Returns the partition, or NULL when @layout has none with that label.
 */
const struct failsafe_layout_part *failsafe_layout_find_part(
			const struct failsafe_layout *layout,
			const char *label);

#endif /* _FAILSAFE_LAYOUT_H_ */
