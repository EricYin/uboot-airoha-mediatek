// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha thermal temperature display command
 *
 * Airoha SoCs put more than one temperature sensor behind a single ADC mux
 * (AN7583 has bandgap, GBE and CPU, AN7563 has a diode and a BJT), while the
 * thermal uclass reports a single value per device.  This command therefore
 * asks the driver for the sensor list and prints each of them; the uclass
 * value (the hottest sensor) is shown as well.
 *
 * Usage:
 *   airoha_thermal                 - read every sensor of every thermal device
 *   airoha_thermal <samples>       - average <samples> reads of each sensor
 *   airoha_thermal -n <sensor> [<samples>]
 *                                  - only read sensor <sensor>
 */

#include <command.h>
#include <dm.h>
#include <stdio.h>
#include <thermal.h>
#include <vsprintf.h>
#include <soc/airoha/thermal.h>

#define AIROHA_THERMAL_DEF_SAMPLES	1
#define AIROHA_THERMAL_MAX_SAMPLES	1000

static int frac_part(int mtemp)
{
	int frac = mtemp % 1000;

	if (frac < 0)
		frac = -frac;

	return frac;
}

static void print_temp(const char *label, int mtemp)
{
	printf("  %-10s: %d.%03d C (%d mC)\n", label, mtemp / 1000,
	       frac_part(mtemp), mtemp);
}

/*
 * Read one sensor, <samples> times when asked for.  Returns 0 and the average
 * reading (milli Celsius) on success.
 */
static int read_sensor(struct udevice *dev, int sensor, int samples, int *temp)
{
	int total = 0;
	int valid = 0;
	int i;

	for (i = 0; i < samples; i++) {
		int mtemp;

		if (airoha_thermal_sensor_temp(dev, sensor, &mtemp))
			continue;

		total += mtemp;
		valid++;
	}

	if (!valid)
		return -EIO;

	*temp = total / valid;

	return 0;
}

static int show_airoha_device(struct udevice *dev, int want, int samples)
{
	int count = airoha_thermal_sensor_count(dev);
	int first = want < 0 ? 0 : want;
	int last = want < 0 ? count - 1 : want;
	int hottest = 0;
	int ret = CMD_RET_SUCCESS;
	int valid = 0;
	int i;

	for (i = first; i <= last; i++) {
		const char *name = airoha_thermal_sensor_name(dev, i);
		int mtemp;

		if (read_sensor(dev, i, samples, &mtemp)) {
			printf("  %-10s: read failed\n", name);
			ret = CMD_RET_FAILURE;
			continue;
		}

		print_temp(name, mtemp);
		valid++;
		if (valid == 1 || mtemp > hottest)
			hottest = mtemp;
	}

	/* With several sensors the uclass value (the hottest) is worth showing */
	if (want < 0 && last > first && valid)
		print_temp("hottest", hottest);

	return ret;
}

static int show_generic_device(struct udevice *dev, int samples)
{
	int total = 0;
	int valid = 0;
	int i;

	for (i = 0; i < samples; i++) {
		int mtemp;

		if (thermal_get_temp(dev, &mtemp))
			continue;

		total += mtemp;
		valid++;
	}

	if (!valid) {
		printf("  read failed\n");
		return CMD_RET_FAILURE;
	}

	print_temp("temp", total / valid);

	return CMD_RET_SUCCESS;
}

static int do_airoha_thermal(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	struct udevice *dev;
	int samples = AIROHA_THERMAL_DEF_SAMPLES;
	int want = -1;
	int ret = CMD_RET_FAILURE;
	bool found = false;
	int arg = 1;

	if (argc > 1 && !strcmp(argv[1], "-n")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		want = simple_strtol(argv[2], NULL, 0);
		if (want < 0)
			return CMD_RET_USAGE;
		arg = 3;
	}

	if (argc > arg) {
		samples = simple_strtol(argv[arg], NULL, 0);
		if (samples < 1)
			return CMD_RET_USAGE;
		if (samples > AIROHA_THERMAL_MAX_SAMPLES)
			samples = AIROHA_THERMAL_MAX_SAMPLES;
	}

	uclass_foreach_dev_probe(UCLASS_THERMAL, dev) {
		int n;

		found = true;
		printf("%s (%s)\n", dev->name, dev->driver->name);

		n = airoha_thermal_sensor_count(dev);
		if (!n) {
			if (show_generic_device(dev, samples) == CMD_RET_SUCCESS)
				ret = CMD_RET_SUCCESS;
			continue;
		}

		if (want >= n) {
			printf("  only %d sensor(s): 0..%d\n", n, n - 1);
			continue;
		}

		if (show_airoha_device(dev, want, samples) == CMD_RET_SUCCESS)
			ret = CMD_RET_SUCCESS;
	}

	if (!found) {
		printf("no thermal device found\n");
		return CMD_RET_FAILURE;
	}

	return ret;
}

U_BOOT_CMD(
	airoha_thermal,	4,	1,	do_airoha_thermal,
	"display Airoha thermal sensor temperature",
	"[-n <sensor>] [<samples>]\n"
	"    - display the temperature of the sensors of every thermal device\n"
	"      (en7523/en7581: one sensor; an7583: bandgap/gbe/cpu; an7563:\n"
	"      diode/bjt). The hottest sensor is reported as well.\n"
	"    - -n <sensor>: only read that sensor\n"
	"    - [<samples>]: read that many times and report the average"
);
