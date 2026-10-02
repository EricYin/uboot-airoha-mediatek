// SPDX-License-Identifier: GPL-2.0+
/*
 * Econet MIPS UART driver
 * 
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 */

#include <dm.h>
#include <errno.h>
#include <serial.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/types.h>

#define UART_RBR	0x03
#define UART_THR	0x03
#define UART_IER	0x07
#define UART_FCR	0x0b
#define UART_LCR	0x0f
#define UART_MCR	0x13
#define UART_LSR	0x17
#define UART_MISCC	0x27
#define UART_XYD	0x2c

#define UART_LCR_DLAB	0x80
#define UART_LCR_8N1	0x03
#define UART_LSR_DR	0x01
#define UART_LSR_THRE	0x20

struct econet_serial_plat {
	void __iomem *base;
};

static void econet_serial_hw_init(struct econet_serial_plat *plat,
				  int baudrate)
{
	void __iomem *base = plat->base;
	u32 div_x;

	/*
	 * EN7512/EN7521 use Y=65000 and X=baud*13/25 with the fixed
	 * 20 MHz UART source. This reproduces the vendor divider table.
	 */
	if (baudrate <= 0)
		baudrate = CONFIG_BAUDRATE;
	div_x = DIV_ROUND_CLOSEST((u32)baudrate * 13, 25);
	if (div_x > 0xffff)
		div_x = 0xffff;

	writeb(UART_LCR_DLAB, base + UART_LCR);
	__raw_writel((div_x << 16) | 65000, base + UART_XYD);
	writeb(1, base + UART_RBR);
	writeb(0, base + UART_IER);
	writeb(UART_LCR_8N1, base + UART_LCR);
	writeb(0x0f, base + UART_FCR);
	writeb(0, base + UART_MCR);
	writeb(0, base + UART_MISCC);
	writeb(0, base + UART_IER);
}

static int econet_serial_putc(struct udevice *dev, const char ch)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);

	if (!(readb(plat->base + UART_LSR) & UART_LSR_THRE))
		return -EAGAIN;

	writeb(ch, plat->base + UART_THR);
	return 0;
}

static int econet_serial_getc(struct udevice *dev)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);

	if (!(readb(plat->base + UART_LSR) & UART_LSR_DR))
		return -EAGAIN;

	return readb(plat->base + UART_RBR);
}

static int econet_serial_pending(struct udevice *dev, bool input)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);
	u8 lsr = readb(plat->base + UART_LSR);

	if (input)
		return !!(lsr & UART_LSR_DR);

	return !(lsr & UART_LSR_THRE);
}

static int econet_serial_setbrg(struct udevice *dev, int baudrate)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);

	econet_serial_hw_init(plat, baudrate);
	return 0;
}

static int econet_serial_of_to_plat(struct udevice *dev)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);

	plat->base = dev_remap_addr(dev);
	if (!plat->base)
		return -EINVAL;

	return 0;
}

static int econet_serial_probe(struct udevice *dev)
{
	struct econet_serial_plat *plat = dev_get_plat(dev);

	econet_serial_hw_init(plat, CONFIG_BAUDRATE);
	return 0;
}

static const struct dm_serial_ops econet_serial_ops = {
	.putc = econet_serial_putc,
	.pending = econet_serial_pending,
	.getc = econet_serial_getc,
	.setbrg = econet_serial_setbrg,
};

static const struct udevice_id econet_serial_ids[] = {
	{ .compatible = "econet,en751221-uart" },
	{ .compatible = "econet,en7528-uart" },
	{ }
};

U_BOOT_DRIVER(serial_econet) = {
	.name = "serial_econet",
	.id = UCLASS_SERIAL,
	.of_match = econet_serial_ids,
	.of_to_plat = econet_serial_of_to_plat,
	.plat_auto = sizeof(struct econet_serial_plat),
	.probe = econet_serial_probe,
	.ops = &econet_serial_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
