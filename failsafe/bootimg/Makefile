# SPDX-License-Identifier: GPL-2.0
#
# Shared boot-image helpers (FIP / BL2 / image / storage)

obj-y += bl2.o fip.o image.o storage.o

# BL31 support is optional: it is usually packaged together with BL33
# (U-Boot) inside one FIP, so a board may prefer to leave it out.
obj-$(CONFIG_WEBUI_FAILSAFE_BL31) += bl31.o
