/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Airoha thermal sensor driver - driver specific helpers
 *
 * The DM thermal uclass only has get_temp(), which returns one value per
 * device (the hottest sensor of the SoC).  The "airoha_thermal" command also
 * wants to show what each sensor of a SoC reads, so the driver exports the
 * sensor list through these helpers.
 */
#ifndef _SOC_AIROHA_THERMAL_H
#define _SOC_AIROHA_THERMAL_H

struct udevice;

/**
 * airoha_thermal_sensor_count() - number of sensors of an Airoha SoC
 *
 * @dev:	thermal device
 * Return:	number of ADC sensors the SoC exposes, or 0 when this is not an
 *		Airoha thermal device (the caller can then fall back to the
 *		generic thermal uclass interface)
 */
int airoha_thermal_sensor_count(struct udevice *dev);

/**
 * airoha_thermal_sensor_name() - name of one sensor
 *
 * @dev:	thermal device
 * @sensor:	sensor index, must be < airoha_thermal_sensor_count()
 * Return:	name of the sensor, e.g. "bandgap", "gbe", "cpu", "diode" or "bjt"
 */
const char *airoha_thermal_sensor_name(struct udevice *dev, int sensor);

/**
 * airoha_thermal_sensor_temp() - read one sensor
 *
 * @dev:	thermal device
 * @sensor:	sensor index, must be < airoha_thermal_sensor_count()
 * @millicelsius: returns the temperature in millidegree Celsius
 * Return:	0 on success, -EBUSY when the shared ADC is being read already,
 *		another negative errno on failure
 */
int airoha_thermal_sensor_temp(struct udevice *dev, int sensor,
			       int *millicelsius);

#endif /* _SOC_AIROHA_THERMAL_H */
