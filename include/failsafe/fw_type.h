/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _FAILSAFE_FW_TYPE_H_
#define _FAILSAFE_FW_TYPE_H_

typedef enum {
	FW_TYPE_BL2,
	FW_TYPE_CHAINLOADER,
	FW_TYPE_FIP,
	FW_TYPE_UBOOT,
	FW_TYPE_FW,
	FW_TYPE_INITRD,
} failsafe_fw_t;

/*
 * Human readable name of a firmware type, used by the console
 * diagnostics of the image validation path.
 */
static inline const char *failsafe_fw_type_name(failsafe_fw_t fw)
{
	switch (fw) {
	case FW_TYPE_BL2:
		return "bl2";
	case FW_TYPE_CHAINLOADER:
		return "chainloader";
	case FW_TYPE_FIP:
		return "fip";
	case FW_TYPE_UBOOT:
		return "u-boot";
	case FW_TYPE_FW:
		return "firmware";
	case FW_TYPE_INITRD:
		return "initramfs";
	default:
		return "unknown";
	}
}

#endif
