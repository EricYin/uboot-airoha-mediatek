// SPDX-License-Identifier: GPL-2.0+
/*
 * SCU regmap lookup for AN7589.
 *
 * The AN7589 SCU layout follows AN7583: the clock/reset controller is the
 * "airoha,an7589-scu" node nested in the syscon/simple-mfd node that covers
 * 0x1fb00000, and the reset registers are addressed relative to that parent
 * node.  The chip SCU keeps the EN7581 layout, so the device tree declares
 * "airoha,en7581-chip-scu" as a fallback compatible and this lookup stays
 * unchanged (same approach as AN7563).
 */

#include <syscon.h>
#include <linux/err.h>
#include <soc/airoha/scu-regmap.h>

struct regmap *airoha_get_scu_regmap(void)
{
	ofnode node;

	node = ofnode_by_compatible(ofnode_null(), "airoha,an7589-scu");
	if (!ofnode_valid(node))
		return ERR_PTR(-EINVAL);

	node = ofnode_get_parent(node);
	if (!ofnode_valid(node))
		return ERR_PTR(-EINVAL);

	return syscon_node_to_regmap(node);
}

struct regmap *airoha_get_chip_scu_regmap(void)
{
	ofnode node;

	node = ofnode_by_compatible(ofnode_null(), "airoha,en7581-chip-scu");
	if (!ofnode_valid(node))
		return ERR_PTR(-EINVAL);

	return syscon_node_to_regmap(node);
}
