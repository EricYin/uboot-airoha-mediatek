/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe Web UI - LED status indication
 *
 * The failsafe component is the single owner of every LED used around
 * the recovery workflow (waiting for a button, the web UI, an upgrade
 * and its result).  Board code only stores LED labels and effects in
 * the environment; all of the driving happens in failsafe/led.c.
 *
 * Phase to environment variable mapping (see failsafe/led.c for the
 * effect syntax):
 *
 *   FAILSAFE_LED_IDLE     failsafe_led_idle     (fallback: btnchk_led)
 *   FAILSAFE_LED_READY    failsafe_led_ready    (fallback: failsafe_led)
 *   FAILSAFE_LED_UPGRADE  failsafe_led_upgrade
 *   FAILSAFE_LED_SUCCESS  failsafe_led_success
 *   FAILSAFE_LED_FAIL     failsafe_led_fail
 *
 * An unset/empty variable simply means "no LED activity" for that
 * phase, so the feature stays completely optional.
 *
 * Availability is conditional, because not every board has LEDs:
 *
 *   - CONFIG_WEBUI_FAILSAFE_LED (which depends on CONFIG_LED) decides
 *     at build time whether the engine is compiled in at all.  Without
 *     it, every function below is an empty stub and the environment is
 *     never touched;
 *   - LED labels are resolved lazily at run time.  A label that does
 *     not exist on the board (or a LED subsystem that is not ready)
 *     simply produces no activity, and when none of the labels of the
 *     configured effect exists, the engine goes idle instead of running
 *     the cyclic hook.
 */

#ifndef _FAILSAFE_LED_H_
#define _FAILSAFE_LED_H_

#include <linux/kconfig.h>

/**
 * enum failsafe_led_phase - LED indication phases of the failsafe
 *
 * @FAILSAFE_LED_IDLE:	waiting for a button press that enters the web UI
 * @FAILSAFE_LED_READY:	web failsafe running, waiting for an upload
 * @FAILSAFE_LED_UPGRADE: an image was received and is being written
 * @FAILSAFE_LED_SUCCESS: the upgrade finished successfully
 * @FAILSAFE_LED_FAIL:	the upgrade (or its validation) failed
 * @FAILSAFE_LED_COUNT:	number of phases
 */
enum failsafe_led_phase {
	FAILSAFE_LED_IDLE = 0,
	FAILSAFE_LED_READY,
	FAILSAFE_LED_UPGRADE,
	FAILSAFE_LED_SUCCESS,
	FAILSAFE_LED_FAIL,
	FAILSAFE_LED_COUNT,
};

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_LED)

/**
 * failsafe_led_set_phase() - switch the LED indication to a new phase
 * @phase: phase to display from now on
 *
 * The effect is re-read from the environment, the LEDs of the previous
 * phase are switched off and the first frame of the new effect is
 * applied immediately.  Setting the phase that is already active is a
 * no-op, so it is safe to call from anywhere.
 *
 * Phases without an effect configured (or without any LED present on the
 * board) simply switch all LEDs off; the engine then stays idle until the
 * next phase change.
 */
void failsafe_led_set_phase(enum failsafe_led_phase phase);

/**
 * failsafe_led_poll() - advance the effect to the next frame when due
 *
 * Must be called periodically; long running loops (button polling, the
 * httpd poll loop) call it directly, and, when CONFIG_CYCLIC is enabled,
 * it is additionally driven from the cyclic framework so that rotating
 * effects keep running even inside blocking flash operations.
 */
void failsafe_led_poll(void);

/**
 * failsafe_led_off() - stop the effect and switch all LEDs off
 *
 * Used when the failsafe session ends; the cyclic hook is released as
 * well so that a subsequent set_phase() starts from a clean state.
 */
void failsafe_led_off(void);

#else /* !CONFIG_WEBUI_FAILSAFE_LED */

/*
 * No LED support (CONFIG_LED / CONFIG_WEBUI_FAILSAFE_LED disabled) or no
 * failsafe at all: the whole feature disappears, callers need no #ifdef.
 */
static inline void failsafe_led_set_phase(enum failsafe_led_phase phase)
{
	(void)phase;
}

static inline void failsafe_led_poll(void)
{
}

static inline void failsafe_led_off(void)
{
}

#endif /* CONFIG_WEBUI_FAILSAFE_LED */

#endif /* _FAILSAFE_LED_H_ */
