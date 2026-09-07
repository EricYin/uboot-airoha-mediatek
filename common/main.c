// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

/* #define	DEBUG	*/

#include <autoboot.h>
#include <button.h>
#include <bootstage.h>
#include <bootstd.h>
#include <cli.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <net.h>
#include <version_string.h>
#include <efi_loader.h>
#include <net_abort.h>
#include <event.h>

static void run_preboot_environment_command(void)
{
	char *p;

	p = env_get("preboot");
	if (p != NULL) {
		int prev = 0;

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			prev = disable_ctrlc(1); /* disable Ctrl-C checking */

		run_command_list(p, -1, 0);

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			disable_ctrlc(prev);	/* restore Ctrl-C checking */
	}
}

/*
 * One-shot "abort" flag, the command line counterpart of the "failsafe"
 * flag handled below.
 *
 * "setenv abort 1; saveenv; reset" (e.g. issued from the web console)
 * stops the boot sequence: the flag is consumed here - cleared and saved -
 * so that the next reset boots normally, and main_loop() then drops into
 * the U-Boot command line instead of running the boot command.
 */
static bool boot_abort_requested(void)
{
	const char *ab = env_get("abort");
	bool abort_boot;

	if (!ab)
		return false;

	/*
	 * Evaluate the value before deleting the variable: env_set(x, NULL)
	 * frees the string that env_get() returned (_hdelete() -> free()),
	 * so "ab" is a dangling pointer once the flag has been cleared.
	 */
	abort_boot = !strcmp(ab, "1");

	env_set("abort", NULL);
	env_save();

	return abort_boot;
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	const char *s;
	bool stop_autoboot;

	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");

	if (IS_ENABLED(CONFIG_VERSION_VARIABLE))
		env_set("ver", version_string);  /* set version variable */

	cli_init();

	if (IS_ENABLED(CONFIG_USE_PREBOOT))
		run_preboot_environment_command();

	if (event_notify_null(EVT_POST_PREBOOT))
		return;

	if (IS_ENABLED(CONFIG_UPDATE_TFTP))
		update_tftp(0UL, NULL, NULL);

	if (IS_ENABLED(CONFIG_EFI_CAPSULE_ON_DISK_EARLY)) {
		/* efi_init_early() already called */
		if (efi_init_obj_list() == EFI_SUCCESS)
			efi_launch_capsules();
	}

	process_button_cmds();

	stop_autoboot = boot_abort_requested();

#ifdef CONFIG_MTK_HTTPD
	{
		const char *fs = env_get("failsafe");

		if (fs && !strcmp(fs, "1")) {
			/*
			 * Failsafe reboot requested via the web UI.
			 * Clear the flag first so a subsequent normal
			 * reboot does not re-enter failsafe mode, then
			 * start the HTTP server.
			 */
			env_set("failsafe", NULL);
			env_save();
			run_command("httpd", 0);
		} else if (fs) {
			/* Stale flag with unexpected value: just clear it */
			env_set("failsafe", NULL);
			env_save();
		}
	}
#endif

	/*
	 * One-shot "abort" flag consumed above: stop the boot sequence
	 * here, before bootdelay_process() runs.  On this platform
	 * CONFIG_AUTOBOOT_MENU_SHOW makes bootdelay_process() invoke
	 * menu_show() -> bootmenu_show(), which either shows the boot
	 * menu (auto-running its default entry after bootmenu_delay) or -
	 * with bootdelay == 0 - directly executes the first menu entry;
	 * either way the device boots regardless of an abort flag that is
	 * only checked around autoboot_command().  Dropping straight into
	 * the command line here is what actually interrupts the boot.
	 */
	if (stop_autoboot) {
		printf("Boot aborted (env abort), entering command line\n");
		cli_loop();
		panic("No CLI available");
	}

	if (IS_ENABLED(CONFIG_CMD_BTNCHK))
		run_command("btnchk", 0);

	if (IS_ENABLED(CONFIG_MTK_NET_ABORT))
		net_abort_prepare();

	s = bootdelay_process();
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);

	autoboot_command(s);

	if (IS_ENABLED(CONFIG_MTK_NET_ABORT))
		net_abort_finish();

	/* if standard boot if enabled, assume that it will be able to boot */
	if (IS_ENABLED(CONFIG_BOOTSTD_PROG)) {
		int ret;

		ret = bootstd_prog_boot();
		printf("Standard boot failed (err=%dE)\n", ret);
		panic("Failed to boot");
	}

	cli_loop();

	panic("No CLI available");
}
