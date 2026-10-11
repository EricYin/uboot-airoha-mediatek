// SPDX-License-Identifier: GPL-2.0+
/*
 * A general-purpose cyclic execution infrastructure, to allow "small"
 * (run-time wise) functions to be executed at a specified frequency.
 * Things like LED blinking or watchdog triggering are examples for such
 * tasks.
 *
 * Copyright (C) 2022 Stefan Roese <sr@denx.de>
 */

#include <cyclic.h>
#include <log.h>
#include <malloc.h>
#include <time.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <asm/global_data.h>
#include <u-boot/schedule.h>
#include <uthread.h>

DECLARE_GLOBAL_DATA_PTR;

void hw_watchdog_reset(void);

/*
 * Pass counter of the cyclic_run() walk below.  Each entry remembers the
 * pass it was last serviced in, so that a pass can always tell which
 * entries are still to be done without ever caching a pointer across a
 * callback (the callback is allowed to unregister - and free - cyclics).
 */
static uint32_t cyclic_run_gen;

static bool cyclic_is_registered(const struct cyclic_info *cyclic)
{
	const struct cyclic_info *c;

	hlist_for_each_entry(c, &gd->cyclic_list, list) {
		if (c == cyclic)
			return true;
	}

	return false;
}

void cyclic_register(struct cyclic_info *cyclic, cyclic_func_t func,
		     u64 delay_us, const char *name)
{
	cyclic_unregister(cyclic);

	memset(cyclic, 0, sizeof(*cyclic));

	/* Store values in struct */
	cyclic->func = func;
	cyclic->name = name;
	cyclic->delay_us = delay_us;
	cyclic->start_time_us = get_timer_us(0);

	/*
	 * A cyclic registered from within a cyclic callback (the LED drivers
	 * do that when a blink starts) is marked as serviced in the pass that
	 * is currently running, so it starts in the next one.  This keeps the
	 * walk below terminating even if a callback re-registers itself on
	 * every invocation.
	 */
	if (gd->flags & GD_FLG_CYCLIC_RUNNING)
		cyclic->run_gen = cyclic_run_gen;

	hlist_add_head(&cyclic->list, &gd->cyclic_list);
}

void cyclic_unregister(struct cyclic_info *cyclic)
{
	if (!cyclic_is_registered(cyclic))
		return;

	hlist_del(&cyclic->list);
}

static void cyclic_run(void)
{
	struct cyclic_info *cyclic, *c;
	u64 now, after, cpu_time;
	uint32_t gen;

	/*
	 * Nothing to do if the list is empty. Also, schedule() can be
	 * called before timer infrastructure is ready, in which case
	 * calling get_timer_us() before the (empty) loop could cause
	 * a divide-by-0 or otherwise crash the system. No clients
	 * should be registered before the timer infrastructure is up,
	 * so the check for the list being empty should be
	 * ok. Otherwise, we would need a new GD_FLG_TIMERS_READY
	 * flag.
	 */
	if (hlist_empty(&gd->cyclic_list))
		return;

	/* Prevent recursion */
	if (gd->flags & GD_FLG_CYCLIC_RUNNING)
		return;

	gd->flags |= GD_FLG_CYCLIC_RUNNING;
	now = get_timer_us(0);

	gen = ++cyclic_run_gen;
	if (!gen) {
		/*
		 * The pass counter wrapped around: start over and let every
		 * entry be serviced again (0 keeps its meaning of "not
		 * serviced in any pass").
		 */
		cyclic_run_gen = gen = 1;
		hlist_for_each_entry(c, &gd->cyclic_list, list)
			c->run_gen = 0;
	}

	/*
	 * Walk the list by looking up the next unserviced entry from the head
	 * on every iteration.  A cyclic callback may register or unregister
	 * cyclics (the LED drivers do that whenever a software blink starts
	 * or stops), which invalidates any pointer cached across the call -
	 * an unregistered entry may even be freed while this loop is running.
	 * Therefore no entry is dereferenced after its callback has been
	 * called, and an entry is marked as serviced before that call, so a
	 * freed entry is never looked at again.
	 */
	for (;;) {
		cyclic = NULL;

		hlist_for_each_entry(c, &gd->cyclic_list, list) {
			if (c->run_gen != gen) {
				cyclic = c;
				break;
			}
		}

		if (!cyclic)
			break;

		cyclic->run_gen = gen;

		/*
		 * Check if this cyclic function needs to get called, e.g.
		 * do not call the cyclic func too often
		 */
		if (!time_after_eq64(now, cyclic->next_call))
			continue;

		/* Call cyclic function and account it's cpu-time */
		cyclic->next_call = now + cyclic->delay_us;
		cyclic->func(cyclic);

		after = get_timer_us(0);
		cpu_time = after - now;
		now = after;

		/*
		 * Only account for what is still registered: this test
		 * compares pointers only, so it stays safe even though the
		 * callback may have unregistered (and freed) its own entry.
		 */
		if (!cyclic_is_registered(cyclic))
			continue;

		cyclic->run_cnt++;
		cyclic->cpu_time_us += cpu_time;

		/* Check if cpu-time exceeds max allowed time */
		if ((cpu_time > CONFIG_CYCLIC_MAX_CPU_TIME_US) &&
		    (!cyclic->already_warned)) {
			pr_err("cyclic function %s took too long: %lldus vs %dus max\n",
			       cyclic->name, cpu_time,
			       CONFIG_CYCLIC_MAX_CPU_TIME_US);

			/*
			 * Don't disable this function, just warn once
			 * about this exceeding CPU time usage
			 */
			cyclic->already_warned = true;
		}
	}

	gd->flags &= ~GD_FLG_CYCLIC_RUNNING;
}

void schedule(void)
{
	/* The HW watchdog is not integrated into the cyclic IF (yet) */
	if (IS_ENABLED(CONFIG_HW_WATCHDOG))
		hw_watchdog_reset();

	/*
	 * schedule() might get called very early before the cyclic IF is
	 * ready. Make sure to only call cyclic_run() when it's initalized.
	 */
	if (gd)
		cyclic_run();

	uthread_schedule();
}

int cyclic_unregister_all(void)
{
	struct cyclic_info *cyclic;
	struct hlist_node *tmp;

	hlist_for_each_entry_safe(cyclic, tmp, &gd->cyclic_list, list)
		cyclic_unregister(cyclic);

	return 0;
}
