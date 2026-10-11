// SPDX-License-Identifier: GPL-2.0+
/*
 * Software blinking helpers
 * Copyright (C) 2024 IOPSYS Software Solutions AB
 * Author: Mikhail Kshevetskiy <mikhail.kshevetskiy@iopsys.eu>
 */

#include <cyclic.h>
#include <dm.h>
#include <led.h>
#include <time.h>
#include <stdlib.h>

#define CYCLIC_NAME_PREFIX	"led_sw_blink_"

static void led_sw_blink(struct cyclic_info *c)
{
	struct led_sw_blink *sw_blink;
	struct udevice *dev;
	struct led_ops *ops;

	sw_blink = container_of(c, struct led_sw_blink, cyclic);
	dev = sw_blink->dev;
	ops = led_get_ops(dev);

	switch (sw_blink->state) {
	case LED_SW_BLINK_ST_OFF:
		sw_blink->state = LED_SW_BLINK_ST_ON;
		ops->set_state(dev, LEDST_ON);
		break;
	case LED_SW_BLINK_ST_ON:
		sw_blink->state = LED_SW_BLINK_ST_OFF;
		ops->set_state(dev, LEDST_OFF);
		break;
	case LED_SW_BLINK_ST_NOT_READY:
		/*
		 * led_set_period has been called, but
		 * led_set_state(LDST_BLINK) has not yet,
		 * so doing nothing
		 */
		break;
	default:
		break;
	}
}

int led_sw_set_period(struct udevice *dev, int period_ms)
{
	struct led_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct led_sw_blink *sw_blink = uc_plat->sw_blink;
	struct led_ops *ops = led_get_ops(dev);
	int half_period_us;

	half_period_us = period_ms * 1000 / 2;

	if (!sw_blink) {
		int len = sizeof(struct led_sw_blink) +
			  strlen(CYCLIC_NAME_PREFIX) +
			  strlen(uc_plat->label) + 1;

		sw_blink = calloc(1, len);
		if (!sw_blink)
			return -ENOMEM;

		sw_blink->dev = dev;
		sw_blink->state = LED_SW_BLINK_ST_DISABLED;
		strcpy((char *)sw_blink->cyclic_name, CYCLIC_NAME_PREFIX);
		strcat((char *)sw_blink->cyclic_name, uc_plat->label);

		uc_plat->sw_blink = sw_blink;
	}

	/*
	 * Only a blink that never ran needs a cyclic: once registered the
	 * entry stays with the LED for good (see led_sw_on_state_change())
	 * and a new period is applied in place.  Registering again here
	 * could change the cyclic list while the cyclic framework is walking
	 * it, which is exactly what this file must not do.
	 */
	if (sw_blink->state == LED_SW_BLINK_ST_DISABLED) {
		cyclic_register(&sw_blink->cyclic, led_sw_blink,
				half_period_us, sw_blink->cyclic_name);
	} else {
		sw_blink->cyclic.delay_us = half_period_us;
		sw_blink->cyclic.start_time_us = timer_get_us();
	}

	sw_blink->state = LED_SW_BLINK_ST_NOT_READY;
	ops->set_state(dev, LEDST_OFF);

	return 0;
}

bool led_sw_is_blinking(struct udevice *dev)
{
	struct led_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct led_sw_blink *sw_blink = uc_plat->sw_blink;

	if (!sw_blink)
		return false;

	return sw_blink->state > LED_SW_BLINK_ST_NOT_READY;
}

bool led_sw_on_state_change(struct udevice *dev, enum led_state_t state)
{
	struct led_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct led_sw_blink *sw_blink = uc_plat->sw_blink;

	if (!sw_blink || sw_blink->state == LED_SW_BLINK_ST_DISABLED)
		return false;

	if (state == LEDST_BLINK) {
		struct led_ops *ops = led_get_ops(dev);

		/*
		 * toggle LED initially and start blinking on next
		 * led_sw_blink() call.
		 */
		switch (ops->get_state(dev)) {
		case LEDST_ON:
			ops->set_state(dev, LEDST_OFF);
			sw_blink->state = LED_SW_BLINK_ST_OFF;
			break;
		default:
			ops->set_state(dev, LEDST_ON);
			sw_blink->state = LED_SW_BLINK_ST_ON;
			break;
		}

		return true;
	}

	/*
	 * Stop blinking, but keep the object and its registered cyclic in
	 * place.  This runs from within a cyclic callback as well - a LED
	 * animation alternating blinking and non-blinking frames does
	 * exactly that - and a cyclic must never be unregistered, let alone
	 * freed, while the framework walks its list.
	 *
	 * NOT_READY means "registered, but not blinking": led_sw_blink()
	 * ignores it, led_sw_is_blinking() reports "not blinking" and a
	 * following led_set_period() updates the period in place instead of
	 * registering a second time.  Keeping the object also keeps the
	 * period of the last blink, so resuming an animation does not have
	 * to reallocate anything.
	 */
	sw_blink->state = LED_SW_BLINK_ST_NOT_READY;

	return false;
}
