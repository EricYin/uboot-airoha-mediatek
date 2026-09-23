# SPDX-License-Identifier: GPL-2.0
#
# Shared boot-image helpers (FIP / BL2 / image / storage)

obj-y += image.o storage.o
obj-$(CONFIG_ARM) += bl2.o fip.o

# Storage backends behind failsafe_storage_write() / _read(): one file per
# storage type, the same split the MediaTek tree uses between
# mmc_helper.c and mtd_helper.c.  mtd.o and ubi.o contain stubs for the
# builds without CONFIG_MTD / CONFIG_CMD_UBI, so they are always compiled.
obj-y += mtd.o ubi.o
obj-$(CONFIG_MMC) += mmc.o

# BL31 support is optional: it is usually packaged together with BL33
# (U-Boot) inside one FIP, so a board may prefer to leave it out.
obj-$(CONFIG_WEBUI_FAILSAFE_BL31) += bl31.o
