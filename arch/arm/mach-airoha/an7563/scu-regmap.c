// SPDX-License-Identifier: GPL-2.0+
/*
 * Author: Ray Liu <ray.xy.liu@gmail.com>
 *
 * AN7563/AN7552 SCU and chip-SCU regmap helpers.
 *
 * AN7563 dtsi follows the EN7581 multi-node SCU layout: a standalone
 * chip-SCU syscon at 0x1fa20000 and a separate SCU clock-controller
 * at 0x1fb00000. Both nodes carry an AN7563-specific compatible
 * followed by the EN7581 fallback compatible, so the lookup matches
 * "airoha,en7581-{chip-,}scu" and shares the EN7581 driver path.
 */

#include <syscon.h>
#include <linux/err.h>
#include <soc/airoha/scu-regmap.h>

struct regmap *airoha_get_scu_regmap(void)
{
	ofnode node;

	node = ofnode_by_compatible(ofnode_null(), "airoha,en7581-scu");
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
