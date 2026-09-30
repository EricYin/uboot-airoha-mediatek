// SPDX-License-Identifier: GPL-2.0+
/*
 * GPIO Hardware Verification Tool — Airoha pinctrl backend
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Supported Airoha platforms: EN7523, EN7562, AN7563, AN7581, AN7583.
 *
 * GPIO numbering: uses dm_gpio offset (0 .. gpio_count-1).
 * Use "gpiotest list" to discover available pins and their mapping.
 *
 * Usage:
 *   gpiotest list                      — Scan all GPIOs, show states
 *   gpiotest blink <pin> [cnt] [ms]    — Blink GPIO (LED verification)
 *   gpiotest force-blink <pin> [cnt] [ms] — Force-blink (busy GPIO override)
 *   gpiotest monitor <pin>             — Monitor GPIO input (button test)
 *   gpiotest all-leds [start] [end] [ms] — Cycle through GPIOs
 *   gpiotest out <pin> <0|1>           — Set GPIO output
 *   gpiotest force <pin> <0|1>         — Force-set output (busy GPIO override)
 *   gpiotest in <pin>                  — Read GPIO input
 *
 * Shortcut: gpioscan  (same as gpiotest list)
 */

#include <command.h>
#include <console.h>
#include <linux/delay.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <dm.h>
#include <dm/uclass.h>
#include <asm/gpio.h>

/* ---- cached GPIO device info ---- */
static struct udevice *gpio_dev;
static int gpio_max;

#define GPIOTEST_OWNER_LEN	48

struct gpiotest_grab {
	struct gpio_desc desc;
	char owner[GPIOTEST_OWNER_LEN];
	bool stolen;
};

/* =================================================================
 * GPIO Backend — discover Airoha pinctrl GPIO device
 * ================================================================= */

static int gpiotest_gpio_init(void)
{
	struct gpio_dev_priv *uc_priv;
	struct udevice *dev;

	if (gpio_dev && gpio_max > 0)
		return 0;

	/* Find the Airoha GPIO child device */
	uclass_first_device(UCLASS_GPIO, &dev);
	while (dev) {
		if (!strcmp(dev->driver->name, "airoha_pinctrl_gpio")) {
			gpio_dev = dev;
			break;
		}
		uclass_next_device(&dev);
	}

	/* Fallback: use the first GPIO device in the system */
	if (!gpio_dev) {
		uclass_first_device(UCLASS_GPIO, &dev);
		if (!dev) {
			printf("gpiotest: no GPIO device found\n");
			return -ENODEV;
		}
		gpio_dev = dev;
	}

	uc_priv = dev_get_uclass_priv(gpio_dev);
	gpio_max = uc_priv->gpio_count;
	if (gpio_max <= 0) {
		printf("gpiotest: invalid gpio_count (%d)\n", gpio_max);
		return -ENODEV;
	}

	printf("gpiotest: GPIO device \"%s\", %d pins (offset 0..%d)\n",
	       gpio_dev->name, gpio_max, gpio_max - 1);
	return 0;
}

/*
 * Label of the driver currently owning @gpio, or NULL if unclaimed.
 * Used only for diagnostics / restoring the owner after a forced access.
 */
static const char *gpiotest_owner(unsigned int gpio)
{
	struct gpio_dev_priv *uc_priv;
	const char *name;

	if (!gpio_dev)
		return NULL;

	uc_priv = dev_get_uclass_priv(gpio_dev);
	if (!uc_priv || !uc_priv->name)
		return NULL;

	name = uc_priv->name[gpio];

	return name ? name : NULL;
}

static int gpiotest_request(unsigned int gpio, struct gpio_desc *desc)
{
	const char *owner;
	int ret;

	if (gpiotest_gpio_init())
		return -ENODEV;

	if (gpio >= (unsigned int)gpio_max) {
		printf("gpiotest: GPIO %u out of range (max %d)\n",
		       gpio, gpio_max - 1);
		return -EINVAL;
	}

	desc->dev  = gpio_dev;
	desc->offset = gpio;
	desc->flags  = 0;

	ret = dm_gpio_request(desc, "gpiotest");
	if (ret) {
		owner = gpiotest_owner(gpio);
		if (ret == -EBUSY && owner)
			printf("gpiotest: GPIO %u is in use by \"%s\" "
			       "(use force-blink / force)\n", gpio, owner);
		else
			printf("gpiotest: request GPIO %u failed (%d)\n",
			       gpio, ret);
	}
	return ret;
}

/*
 * Forced access — take over a GPIO that is already claimed by another
 * driver (typically an LED).
 *
 * Skipping dm_gpio_request() is NOT enough: every dm_gpio_*() accessor
 * runs check_reserved() and bails out with "not reserved" when the line
 * has not been claimed.  So the line really has to be claimed here; the
 * only difference from the normal path is that an existing owner is
 * temporarily un-claimed and put back afterwards, so that e.g. the LED
 * driver keeps working once the test finished.
 */
static int gpiotest_grab(unsigned int gpio, struct gpiotest_grab *g)
{
	const char *owner;
	int ret;

	if (gpiotest_gpio_init())
		return -ENODEV;

	if (gpio >= (unsigned int)gpio_max) {
		printf("gpiotest: GPIO %u out of range (max %d)\n",
		       gpio, gpio_max - 1);
		return -EINVAL;
	}

	memset(g, 0, sizeof(*g));
	g->desc.dev    = gpio_dev;
	g->desc.offset = gpio;
	g->desc.flags  = 0;

	ret = dm_gpio_request(&g->desc, "gpiotest");
	if (!ret)
		return 0;

	if (ret != -EBUSY) {
		printf("gpiotest: request GPIO %u failed (%d)\n", gpio, ret);
		return ret;
	}

	/* Busy: remember the owner, drop its claim, then claim ourselves */
	owner = gpiotest_owner(gpio);
	if (owner) {
		strncpy(g->owner, owner, sizeof(g->owner) - 1);
		g->owner[sizeof(g->owner) - 1] = '\0';
	}

	if (dm_gpio_free(gpio_dev, &g->desc)) {
		printf("gpiotest: cannot release GPIO %u owner\n", gpio);
		return -EBUSY;
	}

	ret = dm_gpio_request(&g->desc, "gpiotest");
	if (ret) {
		printf("gpiotest: cannot take over GPIO %u (%d)\n", gpio, ret);
		return ret;
	}

	g->stolen = true;
	printf("gpiotest: GPIO %u taken over from \"%s\"\n",
	       gpio, g->owner[0] ? g->owner : "?");

	return 0;
}

/* Release a GPIO grabbed with gpiotest_grab() and restore its owner */
static void gpiotest_ungrab(struct gpiotest_grab *g)
{
	struct gpio_desc restore;

	dm_gpio_free(gpio_dev, &g->desc);

	if (!g->stolen)
		return;

	restore.dev    = gpio_dev;
	restore.offset = g->desc.offset;
	restore.flags  = 0;

	if (dm_gpio_request(&restore, g->owner[0] ? g->owner : "gpiotest_restore"))
		printf("gpiotest: warning: cannot restore owner of GPIO %u\n",
		       g->desc.offset);
}

/* =================================================================
 * Command implementations
 * ================================================================= */

static int do_gpiotest_list(void)
{
	struct gpio_desc desc;
	int i, ret;

	if (gpiotest_gpio_init())
		return CMD_RET_FAILURE;

	printf("\n%-4s %-8s %-8s %-8s %s\n",
	       "PIN", "MODE", "DIR", "VALUE", "OWNER");
	printf("---- -------- -------- -------- ----------------\n");

	for (i = 0; i < gpio_max; i++) {
		memset(&desc, 0, sizeof(desc));

		desc.dev    = gpio_dev;
		desc.offset = i;
		desc.flags  = 0;

		ret = dm_gpio_request(&desc, "gpiotest_list");
		if (ret) {
			/* Owned by another driver (LED, hog, ...) */
			printf("%-4d %-8s %-8s %-8s %s\n", i, "?", "?", "?",
			       gpiotest_owner(i) ? gpiotest_owner(i) : "in use");
			continue;
		}

		ret = gpio_get_function(gpio_dev, i, NULL);

		printf("%-4d ", i);

		if (ret == GPIOF_INPUT) {
			int val = dm_gpio_get_value(&desc);
			printf("%-8s %-8s %-8d %s\n",
			       "GPIO", "INPUT", val, "-");
		} else if (ret == GPIOF_OUTPUT) {
			int val = dm_gpio_get_value(&desc);
			printf("%-8s %-8s %-8d %s\n",
			       "GPIO", "OUTPUT", val, "-");
		} else {
			printf("%-8s %-8s %-8s %s\n",
			       "FUNC", "N/A", "N/A", "-");
		}

		dm_gpio_free(gpio_dev, &desc);
	}

	return CMD_RET_SUCCESS;
}

static int do_gpiotest_blink(int pin, int count, int ms)
{
	struct gpio_desc desc;
	int i, ret;

	if (count <= 0)
		count = 5;
	if (ms <= 0)
		ms = 500;

	if (gpiotest_request(pin, &desc))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&desc, GPIOD_IS_OUT);
	if (ret) {
		printf("gpiotest: set GPIO %d direction failed (%d)\n",
		       pin, ret);
		dm_gpio_free(gpio_dev, &desc);
		return CMD_RET_FAILURE;
	}

	printf("Blinking GPIO %d: %d times, %d ms interval\n",
	       pin, count, ms);

	for (i = 0; i < count; i++) {
		if (ctrlc())
			break;

		dm_gpio_set_value(&desc, 1);
		mdelay(ms);
		dm_gpio_set_value(&desc, 0);
		mdelay(ms);
	}

	dm_gpio_free(gpio_dev, &desc);
	return CMD_RET_SUCCESS;
}

static int do_gpiotest_monitor(int pin)
{
	struct gpio_desc desc;
	int prev = -1, val, ret;

	if (gpiotest_request(pin, &desc))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&desc, GPIOD_IS_IN);
	if (ret) {
		printf("gpiotest: set GPIO %d direction failed (%d)\n",
		       pin, ret);
		dm_gpio_free(gpio_dev, &desc);
		return CMD_RET_FAILURE;
	}

	printf("Monitoring GPIO %d (press Ctrl+C to stop)...\n", pin);

	while (1) {
		if (ctrlc())
			break;

		val = dm_gpio_get_value(&desc);
		if (val != prev) {
			printf("GPIO %d -> %d\n", pin, val);
			prev = val;
		}
		mdelay(50);
	}

	dm_gpio_free(gpio_dev, &desc);
	return CMD_RET_SUCCESS;
}

static int do_gpiotest_all_leds(int start, int end, int ms)
{
	struct gpio_desc desc;
	int i;

	if (gpiotest_gpio_init())
		return CMD_RET_FAILURE;

	if (start < 0)
		start = 0;
	if (end < 0 || end >= gpio_max)
		end = gpio_max - 1;
	if (ms <= 0)
		ms = 100;

	printf("Cycling GPIOs %d..%d, %d ms interval (Ctrl+C to stop)\n",
	       start, end, ms);

	for (i = start; i <= end; i++) {
		if (ctrlc())
			break;

		memset(&desc, 0, sizeof(desc));
		desc.dev    = gpio_dev;
		desc.offset = i;
		desc.flags  = 0;

		if (dm_gpio_request(&desc, "gpiotest_leds")) {
			printf("  skip GPIO %d (in use by \"%s\")\n", i,
			       gpiotest_owner(i) ? gpiotest_owner(i) : "?");
			continue;
		}

		dm_gpio_set_dir_flags(&desc, GPIOD_IS_OUT);
		dm_gpio_set_value(&desc, 1);
		mdelay(ms);
		dm_gpio_set_value(&desc, 0);

		dm_gpio_free(gpio_dev, &desc);
	}

	return CMD_RET_SUCCESS;
}

static int do_gpiotest_out(int pin, int val)
{
	struct gpio_desc desc;
	int ret;

	if (gpiotest_request(pin, &desc))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&desc, GPIOD_IS_OUT |
				    (val ? GPIOD_IS_OUT_ACTIVE : 0));

	printf("GPIO %d -> output %d%s\n", pin, val, ret ? " (failed)" : "");

	dm_gpio_free(gpio_dev, &desc);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gpiotest_in(int pin)
{
	struct gpio_desc desc;
	int val, ret;

	if (gpiotest_request(pin, &desc))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&desc, GPIOD_IS_IN);
	if (ret) {
		printf("gpiotest: set GPIO %d direction failed (%d)\n",
		       pin, ret);
		dm_gpio_free(gpio_dev, &desc);
		return CMD_RET_FAILURE;
	}

	val = dm_gpio_get_value(&desc);
	printf("GPIO %d input value: %d\n", pin, val);

	dm_gpio_free(gpio_dev, &desc);
	return CMD_RET_SUCCESS;
}

static int do_gpiotest_force_out(int pin, int val)
{
	struct gpiotest_grab g;
	int ret;

	if (pin < 0)
		return CMD_RET_FAILURE;

	if (gpiotest_grab(pin, &g))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&g.desc, GPIOD_IS_OUT |
				    (val ? GPIOD_IS_OUT_ACTIVE : 0));
	if (ret)
		printf("gpiotest: set GPIO %d failed (%d)\n", pin, ret);
	else
		printf("GPIO %d -> output %d (forced)\n", pin, val);

	gpiotest_ungrab(&g);

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gpiotest_force_blink(int pin, int count, int ms)
{
	struct gpiotest_grab g;
	int i, ret;

	if (pin < 0)
		return CMD_RET_FAILURE;

	if (count <= 0)
		count = 5;
	if (ms <= 0)
		ms = 500;

	if (gpiotest_grab(pin, &g))
		return CMD_RET_FAILURE;

	ret = dm_gpio_set_dir_flags(&g.desc, GPIOD_IS_OUT);
	if (ret) {
		printf("gpiotest: set GPIO %d direction failed (%d)\n", pin, ret);
		gpiotest_ungrab(&g);
		return CMD_RET_FAILURE;
	}

	printf("Blinking GPIO %d (forced): %d times, %d ms interval\n",
	       pin, count, ms);

	for (i = 0; i < count; i++) {
		if (ctrlc())
			break;

		dm_gpio_set_value(&g.desc, 1);
		mdelay(ms);
		dm_gpio_set_value(&g.desc, 0);
		mdelay(ms);
	}

	gpiotest_ungrab(&g);
	return CMD_RET_SUCCESS;
}

/* =================================================================
 * U-Boot command entry
 * ================================================================= */

static int do_gpiotest(struct cmd_tbl *cmdtp, int flag,
		       int argc, char *const argv[])
{
	const char *cmd;
	int pin, count, ms, val;

	if (argc < 2)
		return CMD_RET_USAGE;

	cmd = argv[1];

	if (!strcmp(cmd, "list") || !strcmp(cmd, "scan")) {
		return do_gpiotest_list();

	} else if (!strcmp(cmd, "blink")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		pin   = dectoul(argv[2], NULL);
		count = (argc > 3) ? dectoul(argv[3], NULL) : 5;
		ms    = (argc > 4) ? dectoul(argv[4], NULL) : 500;
		return do_gpiotest_blink(pin, count, ms);

	} else if (!strcmp(cmd, "monitor")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		pin = dectoul(argv[2], NULL);
		return do_gpiotest_monitor(pin);

	} else if (!strcmp(cmd, "all-leds")) {
		pin   = (argc > 2) ? dectoul(argv[2], NULL) : -1;
		count = (argc > 3) ? dectoul(argv[3], NULL) : -1;
		ms    = (argc > 4) ? dectoul(argv[4], NULL) : 100;
		return do_gpiotest_all_leds(pin, count, ms);

	} else if (!strcmp(cmd, "out")) {
		if (argc < 4)
			return CMD_RET_USAGE;
		pin = dectoul(argv[2], NULL);
		val = dectoul(argv[3], NULL);
		return do_gpiotest_out(pin, val);

	} else if (!strcmp(cmd, "in")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		pin = dectoul(argv[2], NULL);
		return do_gpiotest_in(pin);

	} else if (!strcmp(cmd, "force")) {
		if (argc < 4)
			return CMD_RET_USAGE;
		pin = dectoul(argv[2], NULL);
		val = dectoul(argv[3], NULL);
		return do_gpiotest_force_out(pin, val);

	} else if (!strcmp(cmd, "force-blink")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		pin   = dectoul(argv[2], NULL);
		count = (argc > 3) ? dectoul(argv[3], NULL) : 5;
		ms    = (argc > 4) ? dectoul(argv[4], NULL) : 500;
		return do_gpiotest_force_blink(pin, count, ms);

	} else {
		return CMD_RET_USAGE;
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	gpiotest, 6, 0, do_gpiotest,
	"GPIO hardware verification tool (Airoha)",
	"list                              - scan all GPIO pins\n"
	"gpiotest blink <pin> [cnt] [ms]   - blink a GPIO pin\n"
	"gpiotest force-blink <pin> [cnt] [ms] - force-blink (bypass busy check)\n"
	"gpiotest monitor <pin>            - monitor input level changes\n"
	"gpiotest all-leds [start] [end] [ms] - cycle through GPIOs\n"
	"gpiotest out <pin> <0|1>          - set output value\n"
	"gpiotest force <pin> <0|1>        - set output (bypass busy check)\n"
	"gpiotest in <pin>                 - read input value"
);

U_BOOT_CMD(
	gpioscan, 6, 0, do_gpiotest,
	"Shortcut: scan all GPIO pins",
	"    (same as 'gpiotest list')"
);
