// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7589 pin controller - GPIO and pinconf only.
 *
 * The AN7589 splits its pin control over two register regions:
 *
 *   - GPIO control at 0x1fbf0200 (DIR / DOUT / DIN / INT / PULL / DRV),
 *     the very same block and layout the rest of the family uses, so the
 *     shared airoha_gpiochip register tables in pinctrl-airoha.c apply
 *     unchanged;
 *   - the pin multiplexing ("IOMUX") registers at 0x1fa30000.
 *
 * The IOMUX register map of this SoC is not documented available so far,
 * so this driver deliberately exposes no pinmux groups and no pin function:
 * there is no table to fill in and guessing one would silently drive the
 * wrong bits.  Until the vendor IOMUX description shows up, pins are expected
 * to be configured by the boot chain / by their reset defaults, and only the
 * GPIO interface (and the pinconf plumbing it reuses) is provided.
 *
 * The pin count follows the two-GPIO-bank layout of the shared driver
 * (32 pins per bank, gpio_data_regs[]/gpio_dir_regs[] describe two banks),
 * i.e. gpio0..gpio63.  The AN7589 vendor device tree addresses pin 70, so
 * real hardware has more GPIOs than this driver can reach - that will be
 * fixed together with the pinmux tables, and needs the vendor pin map.
 */

#include "airoha-common.h"

/*
 * GPIO names: the shared gpio implementation works on the pin number
 * directly, so gpioN is pin N (gpio_offs = 0 below).  The AN7589 GPIO
 * numbering base has not been verified against vendor documentation.
 */
static const struct pinctrl_pin_desc pinctrl_pins[] = {
	PINCTRL_PIN(0, "gpio0"),
	PINCTRL_PIN(1, "gpio1"),
	PINCTRL_PIN(2, "gpio2"),
	PINCTRL_PIN(3, "gpio3"),
	PINCTRL_PIN(4, "gpio4"),
	PINCTRL_PIN(5, "gpio5"),
	PINCTRL_PIN(6, "gpio6"),
	PINCTRL_PIN(7, "gpio7"),
	PINCTRL_PIN(8, "gpio8"),
	PINCTRL_PIN(9, "gpio9"),
	PINCTRL_PIN(10, "gpio10"),
	PINCTRL_PIN(11, "gpio11"),
	PINCTRL_PIN(12, "gpio12"),
	PINCTRL_PIN(13, "gpio13"),
	PINCTRL_PIN(14, "gpio14"),
	PINCTRL_PIN(15, "gpio15"),
	PINCTRL_PIN(16, "gpio16"),
	PINCTRL_PIN(17, "gpio17"),
	PINCTRL_PIN(18, "gpio18"),
	PINCTRL_PIN(19, "gpio19"),
	PINCTRL_PIN(20, "gpio20"),
	PINCTRL_PIN(21, "gpio21"),
	PINCTRL_PIN(22, "gpio22"),
	PINCTRL_PIN(23, "gpio23"),
	PINCTRL_PIN(24, "gpio24"),
	PINCTRL_PIN(25, "gpio25"),
	PINCTRL_PIN(26, "gpio26"),
	PINCTRL_PIN(27, "gpio27"),
	PINCTRL_PIN(28, "gpio28"),
	PINCTRL_PIN(29, "gpio29"),
	PINCTRL_PIN(30, "gpio30"),
	PINCTRL_PIN(31, "gpio31"),
	PINCTRL_PIN(32, "gpio32"),
	PINCTRL_PIN(33, "gpio33"),
	PINCTRL_PIN(34, "gpio34"),
	PINCTRL_PIN(35, "gpio35"),
	PINCTRL_PIN(36, "gpio36"),
	PINCTRL_PIN(37, "gpio37"),
	PINCTRL_PIN(38, "gpio38"),
	PINCTRL_PIN(39, "gpio39"),
	PINCTRL_PIN(40, "gpio40"),
	PINCTRL_PIN(41, "gpio41"),
	PINCTRL_PIN(42, "gpio42"),
	PINCTRL_PIN(43, "gpio43"),
	PINCTRL_PIN(44, "gpio44"),
	PINCTRL_PIN(45, "gpio45"),
	PINCTRL_PIN(46, "gpio46"),
	PINCTRL_PIN(47, "gpio47"),
	PINCTRL_PIN(48, "gpio48"),
	PINCTRL_PIN(49, "gpio49"),
	PINCTRL_PIN(50, "gpio50"),
	PINCTRL_PIN(51, "gpio51"),
	PINCTRL_PIN(52, "gpio52"),
	PINCTRL_PIN(53, "gpio53"),
	PINCTRL_PIN(54, "gpio54"),
	PINCTRL_PIN(55, "gpio55"),
	PINCTRL_PIN(56, "gpio56"),
	PINCTRL_PIN(57, "gpio57"),
	PINCTRL_PIN(58, "gpio58"),
	PINCTRL_PIN(59, "gpio59"),
	PINCTRL_PIN(60, "gpio60"),
	PINCTRL_PIN(61, "gpio61"),
	PINCTRL_PIN(62, "gpio62"),
	PINCTRL_PIN(63, "gpio63"),
};

/*
 * No confs either: the pull-up / pull-down / drive strength registers the
 * an7583 driver describes live in the chip SCU, and their AN7589 layout is
 * just as undocumented as the IOMUX one.  pinconf requests therefore fail
 * with -EINVAL instead of poking an unverified register.
 */
static const struct airoha_pinctrl_match_data pinctrl_match_data = {
	.gpio_offs = 0,
	.gpio_pin_cnt = ARRAY_SIZE(pinctrl_pins),
	.chip_scu_compatible = "airoha,en7581-chip-scu",
	.pins = pinctrl_pins,
	.num_pins = ARRAY_SIZE(pinctrl_pins),
	.grps = NULL,
	.num_grps = 0,
	.funcs = NULL,
	.num_funcs = 0,
	.confs_info = { },
	.gpio_muxes = NULL,
	.num_gpio_muxes = 0,
};

static const struct udevice_id pinctrl_of_match[] = {
	{ .compatible = "airoha,an7589-pinctrl",
	  .data = (uintptr_t)&pinctrl_match_data },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(airoha_an7589_pinctrl) = {
	.name = "airoha-an7589-pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = of_match_ptr(pinctrl_of_match),
	.probe = airoha_pinctrl_probe,
	.bind = airoha_pinctrl_bind,
	.priv_auto = sizeof(struct airoha_pinctrl),
	.ops = &airoha_pinctrl_ops,
};
