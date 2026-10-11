// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Stefan Roese <sr@denx.de>
 */

#include <cyclic.h>
#include <dm.h>
#include <test/common.h>
#include <test/test.h>
#include <test/ut.h>
#include <watchdog.h>
#include <linux/delay.h>

/* Test that cyclic function is called */
static struct cyclic_test {
	struct cyclic_info cyclic;
	bool called;
} cyclic_test;

static void test_cb(struct cyclic_info *c)
{
	struct cyclic_test *t = container_of(c, struct cyclic_test, cyclic);
	t->called = true;
}

static int dm_test_cyclic_running(struct unit_test_state *uts)
{
	cyclic_test.called = false;
	cyclic_register(&cyclic_test.cyclic, test_cb, 10 * 1000, "cyclic_test");

	/* Execute all registered cyclic functions */
	schedule();
	ut_asserteq(true, cyclic_test.called);

	cyclic_unregister(&cyclic_test.cyclic);

	return 0;
}
COMMON_TEST(dm_test_cyclic_running, 0);

static struct cyclic_test cyclic_test_victim;

static void test_victim_cb(struct cyclic_info *c)
{
	struct cyclic_test *t = container_of(c, struct cyclic_test, cyclic);

	t->called = true;
}

/*
 * What the LED drivers do when a software blink stops: a cyclic callback
 * unregisters (and in the real drivers frees) another cyclic.  The
 * framework must not walk into the entry it just lost.
 */
static void test_remover_cb(struct cyclic_info *c)
{
	cyclic_unregister(&cyclic_test_victim.cyclic);
}

static int dm_test_cyclic_unregister_in_cb(struct unit_test_state *uts)
{
	/* registered first, so a pass walks it after the remover */
	cyclic_test_victim.called = false;
	cyclic_register(&cyclic_test_victim.cyclic, test_victim_cb, 10 * 1000,
			"cyclic_victim");
	cyclic_register(&cyclic_test.cyclic, test_remover_cb, 10 * 1000,
			"cyclic_remover");

	schedule();

	/* the entry the callback removed must not run in that same pass */
	ut_asserteq(false, cyclic_test_victim.called);

	cyclic_unregister(&cyclic_test.cyclic);
	cyclic_unregister(&cyclic_test_victim.cyclic);

	return 0;
}
COMMON_TEST(dm_test_cyclic_unregister_in_cb, 0);
