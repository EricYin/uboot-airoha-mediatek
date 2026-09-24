# U-Boot Airoha-Mediatek

> [!CAUTION]
> **Warning: Flashing custom bootloaders can brick your device. Proceed with caution and at your own risk.**

## About

This repository contains the U‑Boot bootloader for Airoha SoCs, along with the necessary tools and scripts to build and flash it onto supported Airoha devices.  

> [!NOTE]
> Please note that this version is maintained exclusively for the OpenWrt UBI layout, so be sure to select the correct firmware layout when flashing.  

It features an advanced failsafe web recovery interface, including U‑Boot environment variable management, firmware updates, a web console, UBI management, and more.  

To enter recovery mode, use the WPS/Mesh button or the BreedEnter/UbootEnter tool. Once in this mode, the device will automatically obtain an IP address via DHCP, redirect `http://failsafe.lan` to the U‑Boot IP address, and provide Telnet access for debugging.

## Quick Start

- Prepare

```bash
sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabi build-essential flex bison libssl-dev device-tree-compiler qemu-user-static nodejs npm
```

## Acknowledgement

- [u-boot](https://github.com/u-boot/u-boot)
- [mtk-openwrt](https://github.com/mtk-openwrt)
- [OpenWrt](https://github.com/openwrt/openwrt)
