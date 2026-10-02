// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 AIROHA Inc
 * Airoha ARM UART driver
 *
 * Author: Yuzhii0718 <admin@yuzhii0718.eu.org>
 */

#include <dm.h>
#include <dm/device-internal.h>
#include <div64.h>
#include <errno.h>
#include <log.h>
#include <serial.h>
#include <stdio.h>
#include <asm/io.h>
#include <asm/types.h>
#include <linux/kernel.h>
#include <config.h>

/*************************
 * UART Module Registers *
 *************************/
/* Register offsets relative to the UART base address. The layout is the
 * standard 16550 map accessed with 32-bit (word) transactions, with a
 * vendor-specific fractional divider (XYD) used for baudrate generation.
 * This matches the upstream DTS bindings (reg-io-width = <4>,
 * reg-shift = <2>) for the Airoha ARM UARTs (e.g. an7583 uart2/uart4/uart5).
 */
#define CR_UART_RBR        0x00
#define CR_UART_THR        0x00
#define CR_UART_IER        0x04
#define CR_UART_IIR        0x08
#define CR_UART_FCR        0x08
#define CR_UART_LCR        0x0c
#define CR_UART_MCR        0x10
#define CR_UART_LSR        0x14
#define CR_UART_MSR        0x18
#define CR_UART_SCR        0x1c
#define CR_UART_BRDL       0x00
#define CR_UART_BRDH       0x04
#define CR_UART_WORDA      0x20
#define CR_UART_MISCC      0x24
#define CR_UART_HWORDA     0x28
#define CR_UART_XYD        0x2c

/**********************
 * Baudrate generator *
 **********************/
#define UART_BRD_ACCESS    0x80    /* LCR DLAB */
/* Fractional divider Y. The vendor header uses 65000 for normal builds and
 * 25000 / 12500 for high-speed builds (UART_XYD_Y / UART_XYD_Y_HIGH_SPEED);
 * for a given generator clock only the X/Y ratio matters, the values here
 * reproduce the vendor ones exactly.
 */
#define UART_XYD_Y            65000
#define UART_XYD_Y_HS_LOW     25000  /* high-speed port, <= 115200 baud */
#define UART_XYD_Y_HIGH_SPEED 12500  /* high-speed port, > 115200 baud */
/* XYD generator clock: the 20 MHz crystal divided by UART_CRYSTAL_CLK_DIV
 * (10) for normal UARTs, and by UART_CRYSTAL_CLK_DIV_HIGH_SPEED (2) for the
 * high-speed ones. This is what the vendor uclk_20M table, arht_get_uclk()
 * and tc3162_get_uclk_20M() divider maths is based on, so the DT
 * 'clock-frequency' property (the SoC clock plan feeding the standard 16550
 * divisor path, which this driver pins to 1*16) is deliberately not used.
 */
#define UART_XYD_CLK           2000000   /* 20 MHz / 10 */
#define UART_XYD_CLK_HIGH_SPEED 10000000 /* 20 MHz / 2 */
/* Oversampling of the fractional divider. */
#define UART_XYD_OVERSAMPLE    16
#define UART_XYD_OVERSAMPLE_HS 8
/* Above this baudrate a high-speed port uses UART_XYD_Y_HIGH_SPEED. */
#define UART_XYD_HS_BAUDRATE   115200
/* Integer part of the baudrate divisor (1*16). The ARM parts always use the
 * vendor UART_BRDL_20M/UART_BRDH_20M pair, i.e. BRD = 1.
 */
#define UART_BRD_VAL       0x01
/* Highest number of UARTs a single node can describe (TC3162_NR_PORTS). */
#define UART_MAX_PORTS     4

/* LCR: DLAB cleared, 8 data bits, 1 stop bit, no parity */
#define UART_LCR_8N1       0x03
/* FCR: enable FIFO, clear RX/TX FIFOs, 16550 mode */
#define UART_FCR_VAL       0x0f
/* RX FIFO trigger level: 1 byte */
#define UART_WATERMARK     (0x0 << 6)
/* MCR / MISCC defaults (polled, no ISR -> IER left disabled) */
#define UART_MCR_VAL       0x0
#define UART_MISCC_VAL     0x0
#define UART_IER_VAL       0x00

/* LSR bits */
#define UART_LSR_DR        (1 << 0)    /* data ready */
#define UART_LSR_THRE      (1 << 5)    /* THR empty */

struct airoha_serial_plat {
    void __iomem *base;
    u32 clk;        /* XYD baudrate generator clock (Hz) */
    bool high_speed;    /* high-speed UART: 10 MHz / 8x oversampling */
};

static int airoha_serial_getc(struct udevice *dev)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);

    if (!(readl(plat->base + CR_UART_LSR) & UART_LSR_DR))
        return -EAGAIN;

    return readl(plat->base + CR_UART_RBR) & 0xff;
}

static int airoha_serial_putc(struct udevice *dev, const char ch)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);

    if (!(readl(plat->base + CR_UART_LSR) & UART_LSR_THRE))
        return -EAGAIN;

    writel((u8)ch, plat->base + CR_UART_THR);

    return 0;
}

static int airoha_serial_pending(struct udevice *dev, bool input)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);
    u32 lsr = readl(plat->base + CR_UART_LSR);

    if (input)
        return !!(lsr & UART_LSR_DR);

    return !(lsr & UART_LSR_THRE);
}

static int airoha_serial_setbrg(struct udevice *dev, int baudrate)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);
    u32 clk = plat->clk, div_y = UART_XYD_Y, div_x;
    u32 oversample = UART_XYD_OVERSAMPLE;

    if (baudrate <= 0)
        baudrate = CONFIG_BAUDRATE;

    if (!clk)
        return -EINVAL;

    /* The baudrate generator derives the baudrate from a fractional
     * divider:  baud = clk * X / (oversample * Y * BRD), with BRD fixed at
     * 1, hence X = baud * oversample * Y * BRD / clk.
     * Normal ports run at 2 MHz with 16x oversampling, high-speed ports at
     * 10 MHz with 8x oversampling and a smaller Y above 115200 baud - same
     * arithmetic as tc3162_get_uclk_20M() and
     * tc3162_get_uclk_20M_high_speed().
     */
    if (plat->high_speed) {
        clk = UART_XYD_CLK_HIGH_SPEED;
        oversample = UART_XYD_OVERSAMPLE_HS;
        div_y = baudrate > UART_XYD_HS_BAUDRATE ? UART_XYD_Y_HIGH_SPEED :
                              UART_XYD_Y_HS_LOW;
    }

    div_x = (u32)DIV_ROUND_CLOSEST_ULL((u64)baudrate * div_y *
                       (oversample * UART_BRD_VAL), clk);
    if (div_x > 0xffff)
        div_x = 0xffff;

    writel(UART_BRD_ACCESS, plat->base + CR_UART_LCR);
    writel((div_x << 16) | div_y, plat->base + CR_UART_XYD);
    writel(UART_BRD_VAL, plat->base + CR_UART_BRDL);
    writel(0, plat->base + CR_UART_BRDH);
    writel(UART_LCR_8N1, plat->base + CR_UART_LCR);

    return 0;
}

static ssize_t airoha_serial_puts(struct udevice *dev, const char *s, size_t len)
{
    size_t n = len;

    while (n > 0) {
        if (*s == '\n')
            while (airoha_serial_putc(dev, '\r') == -EAGAIN)
                ;
        while (airoha_serial_putc(dev, *s) == -EAGAIN)
            ;
        s++;
        n--;
    }

    return len;
}

static int airoha_serial_probe(struct udevice *dev)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);

    /* Enable FIFO, reset RX/TX FIFOs, 16550 mode, 1-byte watermark */
    writel(UART_FCR_VAL | UART_WATERMARK, plat->base + CR_UART_FCR);
    /* Modem control = 0 */
    writel(UART_MCR_VAL, plat->base + CR_UART_MCR);
    /* Disable IRDA / power saving / flow control */
    writel(UART_MISCC_VAL, plat->base + CR_UART_MISCC);
    /* Polled mode: leave interrupts disabled (no ISR registered) */
    writel(UART_IER_VAL, plat->base + CR_UART_IER);

    return airoha_serial_setbrg(dev, CONFIG_BAUDRATE);
}

static int airoha_serial_of_to_plat(struct udevice *dev)
{
    struct airoha_serial_plat *plat = dev_get_plat(dev);

    /* The first reg window of the node is the UART this device drives;
     * further windows are turned into additional devices by
     * airoha_serial_bind().
     */
    plat->base = dev_remap_addr(dev);
    if (!plat->base)
        return -EINVAL;

    plat->clk = UART_XYD_CLK;
    /* High-speed UARTs (hsuart3 on EN7581/AN7583) need a different
     * generator clock and oversampling, see airoha_serial_setbrg().
     */
    plat->high_speed = dev_read_bool(dev, "high-speed");

    return 0;
}

static const struct dm_serial_ops airoha_serial_ops = {
    .putc = airoha_serial_putc,
    .puts = airoha_serial_puts,
    .getc = airoha_serial_getc,
    .pending = airoha_serial_pending,
    .setbrg = airoha_serial_setbrg,
};

/* Driver used for the UARTs described by the extra reg windows of a node.
 * Such devices are bound at runtime, so they have no device tree node of
 * their own and get their platform data filled in by airoha_serial_bind().
 */
U_BOOT_DRIVER(serial_airoha_port) = {
    .name = "serial_airoha_port",
    .id = UCLASS_SERIAL,
    .plat_auto = sizeof(struct airoha_serial_plat),
    .probe = airoha_serial_probe,
    .ops = &airoha_serial_ops,
    .flags = DM_FLAG_PRE_RELOC,
};

static int airoha_serial_bind(struct udevice *dev)
{
    int i;

    /* Static (of-platdata) builds instantiate the devices at build time and
     * cannot create the extra ports dynamically.
     */
    if (CONFIG_IS_ENABLED(OF_PLATDATA))
        return 0;

    /* A node may describe several UARTs in its reg property: the vendor
     * airoha,arht-uart2 node lists uart2, hsuart3, uart4 and uart5, and the
     * vendor Linux driver registers one port per window (TC3162_NR_PORTS,
     * up to 4 on EN7581/AN7583). This device drives the first window, every
     * further window becomes a serial device of its own.
     */
    for (i = 1; i < UART_MAX_PORTS; i++) {
        struct airoha_serial_plat *plat;
        struct udevice *port;
        void __iomem *base;
        char name[32];
        int ret;

        base = dev_remap_addr_index(dev, i);
        if (!base)
            break;

        snprintf(name, sizeof(name), "%s-port%d", dev->name, i);
        ret = device_bind(dev, DM_DRIVER_REF(serial_airoha_port), name,
                  NULL, ofnode_null(), &port);
        if (ret) {
            log_debug("%s: cannot bind port %d (%d)\n", dev->name, i, ret);
            break;
        }

        /* The extra windows are normal UARTs: same clock and divider
         * settings as the first one. A high-speed port is flagged with the
         * 'high-speed' property of its own node, there is no per-window
         * property for it here.
         */
        plat = dev_get_plat(port);
        plat->base = base;
        plat->clk = UART_XYD_CLK;
    }

    return 0;
}

static const struct udevice_id airoha_serial_ids[] = {
    { .compatible = "airoha,en7523-uart" },
    { .compatible = "airoha,an7552-uart" },
    { .compatible = "airoha,an7581-uart" },
    { .compatible = "airoha,an7583-uart" },
    /* uart2 node of the ARM SoCs as described by the vendor DTS: it lists
     * the uart2/hsuart3/uart4/uart5 windows and interrupts, each of which
     * is exposed as its own serial device (see airoha_serial_bind()).
     */
    { .compatible = "airoha,arht-uart2" },
    { }
};

U_BOOT_DRIVER(serial_airoha) = {
    .name = "serial_airoha",
    .id = UCLASS_SERIAL,
    .of_match = airoha_serial_ids,
    .of_to_plat = airoha_serial_of_to_plat,
    .plat_auto = sizeof(struct airoha_serial_plat),
    .bind = airoha_serial_bind,
    .probe = airoha_serial_probe,
    .ops = &airoha_serial_ops,
    .flags = DM_FLAG_PRE_RELOC,
};
