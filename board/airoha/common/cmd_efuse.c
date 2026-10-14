// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha / EcoNet eFuse U-Boot commands.
 *
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * These are thin forwarders to the secure monitor (ATF BL31) SiP service.
 * The Airoha/EcoNet ATF exposes eFuse only through raw SMC opcode handlers,
 * so the commands stay a direct view of that interface: the SMC itself is
 * the efuse library (drivers/misc/airoha_efuse.c), which is also what the
 * drivers use to read the calibration fields.
 *
 *   efuse     -> AIROHA_EFUSE_SMC_FID          (0x82000001)
 *               ecnt_efuse_handler(x1 = opcode, x2, x3)
 *   phy_efuse -> AIROHA_EFUSE_SMC_FID_PHY      (0x82000303)
 *               phy_efuse_handler(x1)
 *
 * For 'efuse', argv[1] is a 4-character ASCII opcode that the secure monitor
 * decodes in ecnt_efuse_handler() (e.g. "READ", "PKID", "SKEY", ...).  It is
 * packed into x1 as the four bytes in little-endian order, exactly as the
 * switch cases in the ATF expect.
 *
 * Both calls are SMC32 (function ID bit 30 clear), so the secure monitor
 * truncates x1..x3 to 32 bits; arguments that do not fit are rejected here
 * instead of being silently truncated.
 */

#include <command.h>
#include <vsprintf.h>
#include <linux/sizes.h>
#include <soc/airoha/efuse.h>

/* x1..x3 of an SMC32 call are 32 bits wide */
#define SMC32_ARG_MAX			0xffffffffULL

/* page mapped by the secure monitor's phy_efuse_handler() */
#define PHY_EFUSE_MAP_ALIGN		SZ_4K

/*
 * Pack a 4-character opcode ("READ") into the 32-bit value the ATF decodes in
 * x1 (little-endian), e.g. "READ" -> 0x44414552.  This is the same value the
 * legacy code produced with *(unsigned int *)argv[1], written without the
 * pointer-to-integer cast.
 */
static uint32_t efuse_opcode(const char *s)
{
	uint32_t op = 0;
	int i;

	for (i = 0; i < 4 && s[i]; i++)
		op |= (uint32_t)(uint8_t)s[i] << (i * 8);
	return op;
}

/*
 * Parse a hexadecimal argument, rejecting values that do not fit the 32-bit
 * x1..x3 of an SMC32 call: the secure monitor masks the upper 32 bits, so a
 * wider value would silently address the wrong place.
 */
static int smc32_arg(const char *name, const char *arg, unsigned long *val)
{
	unsigned long long tmp = simple_strtoull(arg, NULL, 16);

	if (tmp > SMC32_ARG_MAX) {
		printf("%s: '%s' does not fit in a 32-bit SMC argument\n",
		       name, arg);
		return -1;
	}

	*val = (unsigned long)tmp;
	return 0;
}

/* Human-readable form of the status codes the secure monitor returns */
static const char *smc_status_str(int ret)
{
	switch (ret) {
	case 0:
		return "ok";
	case -1:
		return "invalid parameter";
	case -2:
		return "not supported by this secure-monitor build";
	case -3:
		return "invalid range";
	default:
		return NULL;
	}
}

static int efuse_smc_call(const char *name, unsigned long fid,
			  unsigned long r1, unsigned long r2, unsigned long r3)
{
	const char *status;
	int ret;

	ret = airoha_efuse_smc_call(fid, r1, r2, r3);

	status = smc_status_str(ret);
	if (status)
		printf("%s: SMC 0x%08lx %s\n", name, fid, status);
	else
		printf("%s: SMC 0x%08lx failed, ret=%d\n", name, fid, ret);

	return ret;
}

static int efuse_command(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	unsigned long r2, r3;

	if (argc < 4)
		return CMD_RET_USAGE;

	if (smc32_arg(argv[0], argv[2], &r2) ||
	    smc32_arg(argv[0], argv[3], &r3))
		return CMD_RET_USAGE;

	return efuse_smc_call(argv[0], AIROHA_EFUSE_SMC_FID,
			      efuse_opcode(argv[1]), r2, r3);
}

U_BOOT_CMD(
	efuse,	4,	0,	efuse_command,
	"eFuse control (Airoha/EcoNet secure-monitor SMC)",
	"efuse <OPCODE> <arg2> <arg3>\n"
	"  OPCODE is a 4-char code decoded by the ATF ecnt_efuse_handler():\n"
	"    READ  <buf> <len>     read eFuse into NS buffer at <buf>\n"
	"    PKID  <b1> <b2>       write package id\n"
	"    SKEY  <buf> <len>     write secure key from NS buffer <buf>\n"
	"    TEST  <a> <b>         eFuse test\n"
	"    B1ID/B2ID <a> <b>     write board id (idx from opcode)\n"
	"    BL2V  <a> <b>         BL2 version\n"
	"    DUKY/MBOT/BYPS        enable dual-key / multi-boot / hw-bypass\n"
	"    CKEY  <a>             check secure key\n"
	"  Availability of each opcode depends on the secure-monitor build.\n"
);

static int phy_efuse_command(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	unsigned long buf;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (smc32_arg(argv[0], argv[1], &buf))
		return CMD_RET_USAGE;

	/*
	 * The secure monitor maps the single page that contains <buf> and
	 * writes the PHY eFuse data starting at <buf> itself, so an unaligned
	 * buffer lets the tail of the load fall outside the mapping.
	 */
	if (buf & (PHY_EFUSE_MAP_ALIGN - 1)) {
		printf("%s: <buf> must be %lu-byte aligned\n", argv[0],
		       (unsigned long)PHY_EFUSE_MAP_ALIGN);
		return CMD_RET_USAGE;
	}

	return efuse_smc_call(argv[0], AIROHA_EFUSE_SMC_FID_PHY, buf, 0, 0);
}

U_BOOT_CMD(
	phy_efuse,	2,	0,	phy_efuse_command,
	"load PHY eFuse into an NS buffer (Airoha/EcoNet secure-monitor SMC)",
	"phy_efuse <buf>\n"
	"  <buf>: NS DRAM address, must be 4 KiB aligned; the secure monitor\n"
	"         maps that single 4 KiB page before filling it\n"
);
