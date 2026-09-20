/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 * Helper to print colored texts
 */

#ifndef _COLORED_PRINT_H_
#define _COLORED_PRINT_H_

#include <stdio.h>

#define COLOR_PROMPT	"\x1b[0;33m"
#define COLOR_INPUT	"\x1b[4;36m"
#define COLOR_ERROR	"\x1b[93;41m"
#define COLOR_CAUTION	"\x1b[1;31m"
#define COLOR_SUCCESS	"\x1b[32m"
#define COLOR_NORMAL	"\x1b[0m"

/*
 * Colored printing for the failsafe subsystem.
 *
 *   cprintln(color, fmt, ...)  - one line, reset to normal at the end
 *   cprint(color, fmt, ...)    - same, but no trailing newline
 *   cprint_cont(color, fmt, ...) - continue a line in @color (no reset)
 *
 * The color argument is one of PROMPT / INPUT / ERROR / CAUTION / SUCCESS /
 * NORMAL and is pasted onto the COLOR_ prefix.  When
 * CONFIG_WEBUI_FAILSAFE_COLOR is disabled the escape codes are dropped and
 * the macros behave like plain printf(), so callers never need a #ifdef of
 * their own.
 */
#ifdef CONFIG_WEBUI_FAILSAFE_COLOR
#define cprintln(color, fmt, ...) \
	printf(COLOR_##color fmt COLOR_NORMAL "\n", ##__VA_ARGS__)

#define cprint(color, fmt, ...) \
	printf(COLOR_##color fmt COLOR_NORMAL, ##__VA_ARGS__)

#define cprint_cont(color, fmt, ...) \
	printf(COLOR_##color fmt, ##__VA_ARGS__)
#else
#define cprintln(color, fmt, ...) \
	printf(fmt "\n", ##__VA_ARGS__)

#define cprint(color, fmt, ...) \
	printf(fmt, ##__VA_ARGS__)

#define cprint_cont(color, fmt, ...) \
	printf(fmt, ##__VA_ARGS__)
#endif

#endif /* _COLORED_PRINT_H_ */
