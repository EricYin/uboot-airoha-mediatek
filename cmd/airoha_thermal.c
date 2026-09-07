// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha thermal temperature display command
 *
 * Displays the current temperature of Airoha thermal sensors
 * (en7581 / an7583 / en7523) via the DM thermal uclass.
 *
 * Usage:
 *   airoha_thermal         - read and display temperature once
 *   airoha_thermal <n>     - sample n times and report the average
 */

#include <command.h>
#include <dm.h>
#include <stdio.h>
#include <thermal.h>
#include <vsprintf.h>

#define AIROHA_THERMAL_DEF_SAMPLES	1
#define AIROHA_THERMAL_MAX_SAMPLES	1000

static int frac_part(int mtemp)
{
	int frac = mtemp % 1000;

	if (frac < 0)
		frac = -frac;

	return frac;
}

static int read_and_print_temp(struct udevice *dev, int samples)
{
	int total = 0;
	int valid = 0;
	int i;

	printf("%-20s (%s)\n", dev->name, dev->driver->name);

	for (i = 0; i < samples; i++) {
		int temp;

		if (thermal_get_temp(dev, &temp)) {
			printf("  sample %2d: read failed\n", i + 1);
			continue;
		}
		total += temp;
		valid++;
		printf("  sample %2d: %d.%03d C (%d mC)\n", i + 1,
		       temp / 1000, frac_part(temp), temp);
	}

	if (valid && samples > 1) {
		int avg = total / valid;

		printf("  average  : %d.%03d C (%d mC)\n",
		       avg / 1000, frac_part(avg), avg);
	}

	return valid ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

static int do_airoha_thermal(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	struct udevice *dev;
	int samples = AIROHA_THERMAL_DEF_SAMPLES;
	int ret = CMD_RET_FAILURE;
	bool found = false;

	if (argc > 1) {
		samples = simple_strtol(argv[1], NULL, 0);
		if (samples < 1)
			return CMD_RET_USAGE;
		if (samples > AIROHA_THERMAL_MAX_SAMPLES)
			samples = AIROHA_THERMAL_MAX_SAMPLES;
	}

	uclass_foreach_dev_probe(UCLASS_THERMAL, dev) {
		found = true;
		if (read_and_print_temp(dev, samples) == CMD_RET_SUCCESS)
			ret = CMD_RET_SUCCESS;
	}

	if (!found) {
		printf("no thermal device found\n");
		return CMD_RET_FAILURE;
	}

	return ret;
}

U_BOOT_CMD(
	airoha_thermal,	2,	1,	do_airoha_thermal,
	"display Airoha thermal sensor temperature",
	"[samples]\n"
	"    - display the current temperature of Airoha thermal sensors\n"
	"      (en7581/an7583/en7523). If [samples] is given, read that\n"
	"      many times and report the average."
);
