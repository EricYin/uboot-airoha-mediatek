// SPDX-License-Identifier: GPL-2.0

#ifndef __ECONET_CHIP_ID_H_
#define __ECONET_CHIP_ID_H_

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <soc/airoha/scu-regmap.h>

#define ECONET_PKG_ID_NAME(_id) [(_id)] = #_id

/*
 * Implemented by the platform code (arch/mips/mach-econet/cpu.c): reads the
 * NP-SCU, CHIP-SCU and eFuse registers of the running SoC with the helpers
 * below and returns its package name, "unknown" when they do not decode.
 */
const char *econet_get_soc_name(void);

#define ECONET_NP_SCU_BASE		0x1fb00000
#define ECONET_NP_SCU_SIZE		0x960
#define ECONET_CHIP_SCU_BASE		0x1fa20000
#define ECONET_CHIP_SCU_SIZE		0x360
#define ECONET_EFUSE_BASE		0x1fbf8000

#define ECONET_NP_SCU_PDIDR		0x05c
#define ECONET_NP_SCU_HIR		0x064
#define ECONET_NP_SCU_SCREG_WR1	0x284
#define ECONET_NP_SCU_MT751020_CFG	0x0f8

#define ECONET_CHIP_SCU_751627_PKG	0x174
#define ECONET_CHIP_SCU_7526C_PKG	0x1ec

#define ECONET_EFUSE_VERIFY_DATA0	0x214
#define ECONET_EFUSE_VERIFY_DATA1	0x218

#define ECONET_NP_SCU_HIR_MASK		GENMASK(31, 16)
#define ECONET_NP_SCU_PDIDR_MASK	GENMASK(15, 0)
#define ECONET_NP_SCU_PACKAGE_ID_MASK	GENMASK(3, 0)
#define ECONET_NP_SCU_PACKAGE_ID_EXT	BIT(7)

#define ECONET_CHIP_SCU_751627_QFP	BIT(15)
#define ECONET_CHIP_SCU_7526C_FP	BIT(10)

#define ECONET_EFUSE_PKG_7526C_MASK		0x3c
#define ECONET_EFUSE_PKG_MASK_751627		0xc0000
#define ECONET_EFUSE_REMARK_BIT_751627		BIT(0)
#define ECONET_EFUSE_PKG_REMARK_SHIFT_751627	2
#define ECONET_EFUSE_PKG_MASK			GENMASK(5, 0)
#define ECONET_EFUSE_REMARK_BIT			BIT(6)
#define ECONET_EFUSE_PKG_REMARK_SHIFT		7
#define ECONET_EFUSE_DDR3_BIT			BIT(23)
#define ECONET_EFUSE_DDR3_REMARK_BIT		BIT(24)

enum econet_pkg {
	/* EN7528 */
	EN7528_PKG = 0xb,

	/* EN7580 */
	EN7580_PKG = 0xa,

	/* EN7516, EN7527 */
	EN751627_PKG = 0x9,

	/* EN7526c, EN7522 */
	EN7526C_PKG = 0x8,

	/* EN7512, EN7521 */
	EN751221_PKG = 0x7,

	/* MT7505 */
	MT7505_PKG = 0x6,

	/* MT7510, MT7520 */
	MT751020_PKG = 0x5,
};

/*
 * Raw MIPS eFuse package encodings from the vendor SDK. These values are
 * family-relative and therefore may overlap.
 */
enum econet_mips_efuse_pkg_id {
	ECONET_EFUSE_EN7527H = 0x0,
	ECONET_EFUSE_EN7527G = 0x0,
	ECONET_EFUSE_EN7561G = 0xc0000,
	ECONET_EFUSE_EN7516G = 0x80000,

	ECONET_EFUSE_EN7526F = 0x00,
	ECONET_EFUSE_EN7521F = 0x10,
	ECONET_EFUSE_EN7521S = 0x20,
	ECONET_EFUSE_EN7512 = 0x04,
	ECONET_EFUSE_EN7526D = 0x01,
	ECONET_EFUSE_EN7526FT = 0x11,
	ECONET_EFUSE_EN7513 = 0x05,
	ECONET_EFUSE_EN7526G = 0x02,
	ECONET_EFUSE_EN7521G = 0x12,
	ECONET_EFUSE_EN7513G = 0x06,
	ECONET_EFUSE_EN7586 = 0x0a,
};

enum econet_pkg_ids {
	/*EN7528*/
	EN7528HU,
	EN7528DU,
	EN7561DU,
	EN7526FH,
	EN7521G,

	/* EN7580 */
	EN7580GT,
	EN7580ST,
	EN7580GAT,
	EN7565,
	EN7580,

	/* EN7516 */
	EN7516G,

	/* EN7527 */
	EN7527G,
	EN7561G,
	EN751627,

	/* EN7512 */
	EN7512,
	EN7513,
	EN7513G,

	/* EN7521, EN7521FC */
	EN7521FCUD,
	EN7521F,
	EN7521S,
	EN7526D,
	EN7526F,
	EN7526G,
	EN7526FT,
	EN7526FP,
	EN7526FT_C,
	EN751221,

	/* MT7520 */
	MT7520S,
	MT7520,
	MT7520G,
	MT7525,
	MT7525G,

	/* MIPS variants missing from the vendor chipId_t table */
	EN7561HU,
	EN7528GT_EN7580,
	EN7527H,
	EN7586,
	MT7510,
	MT7511,

	END_PACKAGE_ID = 0xFFFFFFFF,
};

/* Compatibility with vendor/legacy detector identifiers. */
#define EN7526FH_EN7528DU	EN7526FH
#define EN7521G_EN7528DU	EN7521G
#define EN7526FHEN7528DU	EN7526FH
#define EN7521GEN7528DU	EN7521G

static const char *const econet_pkg_id_names[] = {
	/* EN7528 */
	ECONET_PKG_ID_NAME(EN7528HU),
	ECONET_PKG_ID_NAME(EN7528DU),
	ECONET_PKG_ID_NAME(EN7561DU),
	ECONET_PKG_ID_NAME(EN7526FH),
	ECONET_PKG_ID_NAME(EN7521G),

	/* EN7580 */
	ECONET_PKG_ID_NAME(EN7580GT),
	ECONET_PKG_ID_NAME(EN7580ST),
	ECONET_PKG_ID_NAME(EN7580GAT),
	ECONET_PKG_ID_NAME(EN7565),
	ECONET_PKG_ID_NAME(EN7580),

	/* EN7516 */
	ECONET_PKG_ID_NAME(EN7516G),

	/* EN7527 */
	ECONET_PKG_ID_NAME(EN7527G),
	ECONET_PKG_ID_NAME(EN7561G),
	ECONET_PKG_ID_NAME(EN751627),

	/* EN7512 */
	ECONET_PKG_ID_NAME(EN7512),
	ECONET_PKG_ID_NAME(EN7513),
	ECONET_PKG_ID_NAME(EN7513G),

	/* EN7521 / EN7526 */
	ECONET_PKG_ID_NAME(EN7521FCUD),
	ECONET_PKG_ID_NAME(EN7521F),
	ECONET_PKG_ID_NAME(EN7521S),
	ECONET_PKG_ID_NAME(EN7526D),
	ECONET_PKG_ID_NAME(EN7526F),
	ECONET_PKG_ID_NAME(EN7526G),
	ECONET_PKG_ID_NAME(EN7526FT),
	ECONET_PKG_ID_NAME(EN7526FP),
	ECONET_PKG_ID_NAME(EN7526FT_C),
	ECONET_PKG_ID_NAME(EN751221),

	/* MT7520 */
	ECONET_PKG_ID_NAME(MT7520S),
	ECONET_PKG_ID_NAME(MT7520),
	ECONET_PKG_ID_NAME(MT7520G),
	ECONET_PKG_ID_NAME(MT7525),
	ECONET_PKG_ID_NAME(MT7525G),

	/* MIPS variants */
	ECONET_PKG_ID_NAME(EN7561HU),
	ECONET_PKG_ID_NAME(EN7528GT_EN7580),
	ECONET_PKG_ID_NAME(EN7527H),
	ECONET_PKG_ID_NAME(EN7586),
	ECONET_PKG_ID_NAME(MT7510),
	ECONET_PKG_ID_NAME(MT7511),
};

static inline const char *econet_pkg_id_name(enum econet_pkg_ids id)
{
	if (id >= END_PACKAGE_ID ||
	    (unsigned int)id >= ARRAY_SIZE(econet_pkg_id_names) ||
	    !econet_pkg_id_names[id])
		return "unknown";
	return econet_pkg_id_names[id];
}

static inline const char *
econet_pkg_id_range_name(u32 pkgid, enum econet_pkg_ids first,
			 enum econet_pkg_ids last)
{
	u32 id;

	if (pkgid > (u32)(last - first))
		return NULL;

	id = first + pkgid;
	if (id >= ARRAY_SIZE(econet_pkg_id_names) ||
	    !econet_pkg_id_names[id])
		return NULL;

	return econet_pkg_id_names[id];
}

static inline u32 econet_efuse_pkgid(u32 value)
{
	if (value & ECONET_EFUSE_REMARK_BIT)
		value >>= ECONET_EFUSE_PKG_REMARK_SHIFT;

	return value & ECONET_EFUSE_PKG_MASK;
}

static inline u32 econet_efuse_pkgid_751627(u32 value)
{
	if (value & ECONET_EFUSE_REMARK_BIT_751627)
		value >>= ECONET_EFUSE_PKG_REMARK_SHIFT_751627;

	return value & ECONET_EFUSE_PKG_MASK_751627;
}

static inline bool econet_efuse_is_ddr3(u32 value)
{
	if (value & ECONET_EFUSE_REMARK_BIT)
		return !!(value & ECONET_EFUSE_DDR3_REMARK_BIT);

	return !!(value & ECONET_EFUSE_DDR3_BIT);
}

static inline bool econet_efuse_is_enp_mod(u32 value)
{
	if (value & BIT(3))
		return !!(value & BIT(5));

	return !!(value & BIT(1));
}

static inline bool econet_efuse_is_ens_mod(u32 value)
{
	if (value & BIT(3))
		return !!(value & BIT(6));

	return !!(value & BIT(2));
}

static inline enum econet_pkg econet_pkg_from_id(u32 id)
{
	switch (id) {
	case EN7528_PKG:
	case 0x7528:
	case 0x7561:
		return EN7528_PKG;
	case EN7580_PKG:
	case 0x7580:
	case 0x7565:
		return EN7580_PKG;
	case EN751627_PKG:
	case 0x7516:
	case 0x7527:
		return EN751627_PKG;
	case EN7526C_PKG:
	case 0x7522:
		return EN7526C_PKG;
	case EN751221_PKG:
	case 0x7512:
	case 0x7513:
	case 0x7521:
	case 0x7526:
		return EN751221_PKG;
	case MT751020_PKG:
	case 0x7510:
	case 0x7520:
	case 0x7525:
		return MT751020_PKG;
	default:
		return 0;
	}
}

static inline const char *econet_pkg_family_name(enum econet_pkg pkg)
{
	switch (econet_pkg_from_id(pkg)) {
	case EN7528_PKG:
		return "EN7528";
	case EN7580_PKG:
		return "EN7580";
	case EN751627_PKG:
		return "EN7516/EN7527";
	case EN7526C_PKG:
		return "EN7526C/EN7522";
	case EN751221_PKG:
		return "EN7512/EN7521";
	case MT751020_PKG:
		return "MT7510/MT7520";
	default:
		return NULL;
	}
}

static inline const char *
econet_soc_variant_name(enum econet_pkg pkg, u32 pkgid)
{
	switch (econet_pkg_from_id(pkg)) {
	case EN7528_PKG:
		switch (pkgid) {
		case 0x0:
			return econet_pkg_id_name(EN7528HU);
		case 0x1:
			return econet_pkg_id_name(EN7528DU);
		case 0x2:
			return econet_pkg_id_name(EN7561DU);
		case 0x3:
			return econet_pkg_id_name(EN7526FH);
		case 0x4:
			return econet_pkg_id_name(EN7561HU);
		case 0x7:
			return econet_pkg_id_name(EN7521G);
		default:
			return NULL;
		}
	case EN7580_PKG:
		switch (pkgid) {
		case 0x0:
			return econet_pkg_id_name(EN7580GT);
		case 0x1:
			return econet_pkg_id_name(EN7580ST);
		case 0x2:
			return econet_pkg_id_name(EN7580GAT);
		case 0x3:
			return econet_pkg_id_name(EN7565);
		case 0x4:
			return econet_pkg_id_name(EN7528GT_EN7580);
		default:
			return NULL;
		}
	/*
	 * EN7516/EN7527, EN7526C/EN7522 and EN7512/EN7521 do not
	 * have a linear SCREG_WR1 package-id encoding. Decode them from
	 * the MIPS eFuse registers with econet_soc_variant_name_mips().
	 */
	case EN751627_PKG:
	case EN7526C_PKG:
	case EN751221_PKG:
	case MT751020_PKG:
		return NULL;
	default:
		return NULL;
	}
}

static inline const char *
econet_soc_variant_name_efuse(enum econet_pkg pkg, u32 efuse0, u32 efuse1,
			      u32 chip_scu_174, u32 chip_scu_1ec)
{
	u32 efuse_pkg;

	switch (econet_pkg_from_id(pkg)) {
	case EN751627_PKG:
		efuse_pkg = econet_efuse_pkgid_751627(efuse0);

		if (efuse_pkg == ECONET_EFUSE_EN7516G)
			return econet_pkg_id_name(EN7516G);

		if (efuse_pkg == ECONET_EFUSE_EN7561G &&
		    !(chip_scu_174 & ECONET_CHIP_SCU_751627_QFP))
			return econet_pkg_id_name(EN7561G);

		if (efuse_pkg == ECONET_EFUSE_EN7527H) {
			if (chip_scu_174 & ECONET_CHIP_SCU_751627_QFP)
				return econet_pkg_id_name(EN7527H);

			return econet_pkg_id_name(EN7527G);
		}

		return NULL;

	case EN7526C_PKG:
		efuse_pkg = econet_efuse_pkgid(efuse0) &
			     ECONET_EFUSE_PKG_7526C_MASK;

		if (efuse_pkg == ECONET_EFUSE_EN7521F) {
			if (econet_efuse_is_ddr3(efuse0))
				return econet_pkg_id_name(EN7521FCUD);

			return econet_pkg_id_name(EN7521F);
		}

		if (efuse_pkg == ECONET_EFUSE_EN7521S)
			return econet_pkg_id_name(EN7521S);

		if (efuse_pkg == ECONET_EFUSE_EN7526F) {
			bool ft_c;

			ft_c = efuse0 & ECONET_EFUSE_REMARK_BIT ?
			       !!(efuse1 & BIT(10)) :
			       !!(efuse1 & BIT(9));

			if (ft_c)
				return econet_pkg_id_name(EN7526FT_C);

			if (chip_scu_1ec & ECONET_CHIP_SCU_7526C_FP)
				return econet_pkg_id_name(EN7526FP);

			return econet_pkg_id_name(EN7526F);
		}

		return NULL;

	case EN751221_PKG:
		switch (econet_efuse_pkgid(efuse0)) {
		case ECONET_EFUSE_EN7526F:
			return econet_pkg_id_name(EN7526F);
		case ECONET_EFUSE_EN7526D:
			return econet_pkg_id_name(EN7526D);
		case ECONET_EFUSE_EN7526G:
			return econet_pkg_id_name(EN7526G);
		case ECONET_EFUSE_EN7512:
			return econet_pkg_id_name(EN7512);
		case ECONET_EFUSE_EN7513:
			return econet_pkg_id_name(EN7513);
		case ECONET_EFUSE_EN7513G:
			return econet_pkg_id_name(EN7513G);
		case ECONET_EFUSE_EN7586:
			return econet_pkg_id_name(EN7586);
		case ECONET_EFUSE_EN7521F:
			return econet_pkg_id_name(EN7521F);
		case ECONET_EFUSE_EN7526FT:
			return econet_pkg_id_name(EN7526FT);
		case ECONET_EFUSE_EN7521G:
			return econet_pkg_id_name(EN7521G);
		case ECONET_EFUSE_EN7521S:
			return econet_pkg_id_name(EN7521S);
		default:
			return NULL;
		}
	default:
		return NULL;
	}
}

static inline const char *
econet_soc_variant_name_mt751020(u32 np_scu_cfg, u32 efuse0)
{
	bool enp = econet_efuse_is_enp_mod(efuse0);
	bool ens = econet_efuse_is_ens_mod(efuse0);

	switch (np_scu_cfg & 0x3) {
	case 0x0:
		return econet_pkg_id_name(enp ? MT7510 : MT7511);
	case 0x2:
		if (ens)
			return econet_pkg_id_name(MT7520S);
		return econet_pkg_id_name(enp ? MT7520 : MT7525);
	case 0x3:
		return econet_pkg_id_name(enp ? MT7520G : MT7525G);
	default:
		return NULL;
	}
}

static inline const char *
econet_soc_variant_name_mips(enum econet_pkg pkg, u32 pkgid,
			     u32 np_scu_cfg, u32 chip_scu_174,
			     u32 chip_scu_1ec, u32 efuse0, u32 efuse1)
{
	switch (econet_pkg_from_id(pkg)) {
	case EN751627_PKG:
	case EN7526C_PKG:
	case EN751221_PKG:
		return econet_soc_variant_name_efuse(pkg, efuse0, efuse1,
						      chip_scu_174,
						      chip_scu_1ec);
	case MT751020_PKG:
		return econet_soc_variant_name_mt751020(np_scu_cfg, efuse0);
	default:
		return econet_soc_variant_name(pkg, pkgid);
	}
}

static inline const char *econet_soc_name(enum econet_pkg pkg, u32 pkgid)
{
	const char *name;

	name = econet_soc_variant_name(pkg, pkgid);
	if (name)
		return name;

	name = econet_pkg_family_name(pkg);
	if (name)
		return name;

	return "unknown";
}

static inline const char *
econet_soc_name_from_regs(u32 hir, u32 pkgid, u32 pdidr)
{
	enum econet_pkg hir_pkg = econet_pkg_from_id(hir);
	enum econet_pkg pdidr_pkg = econet_pkg_from_id(pdidr);
	const char *name;

	name = econet_soc_variant_name(hir_pkg, pkgid);
	if (name)
		return name;

	name = econet_soc_variant_name(pdidr_pkg, pkgid);
	if (name)
		return name;

	name = econet_pkg_family_name(hir_pkg);
	if (name)
		return name;

	name = econet_pkg_family_name(pdidr_pkg);
	if (name)
		return name;

	return "unknown";
}

static inline const char *
econet_soc_name_from_mips_regs(u32 hir, u32 pkgid, u32 pdidr,
			       u32 np_scu_cfg, u32 chip_scu_174,
			       u32 chip_scu_1ec, u32 efuse0, u32 efuse1)
{
	enum econet_pkg hir_pkg = econet_pkg_from_id(hir);
	enum econet_pkg pdidr_pkg = econet_pkg_from_id(pdidr);
	const char *name;

	name = econet_soc_variant_name_mips(hir_pkg, pkgid, np_scu_cfg,
					     chip_scu_174, chip_scu_1ec,
					     efuse0, efuse1);
	if (name)
		return name;

	name = econet_soc_variant_name_mips(pdidr_pkg, pkgid, np_scu_cfg,
					     chip_scu_174, chip_scu_1ec,
					     efuse0, efuse1);
	if (name)
		return name;

	name = econet_pkg_family_name(hir_pkg);
	if (name)
		return name;

	name = econet_pkg_family_name(pdidr_pkg);
	if (name)
		return name;

	return "unknown";
}

static inline u32 econet_pkgid_from_screg(u32 value)
{
	u32 pkgid = FIELD_GET(ECONET_NP_SCU_PACKAGE_ID_MASK, value);

	if (value & ECONET_NP_SCU_PACKAGE_ID_EXT)
		pkgid |= BIT(4);

	return pkgid;
}

/*
 * Direct-MMIO helper for MIPS boot code/early platform code.
 *
 * In U-Boot on MIPS the caller can pass uncached KSEG1 mappings, e.g.
 * (void __iomem *)CKSEG1ADDR(ECONET_NP_SCU_BASE).
 */
static inline const char *
econet_soc_name_from_mips_mem(void __iomem *np_scu, void __iomem *chip_scu,
			      void __iomem *efuse)
{
	u32 hir, pdidr, pkgid, np_scu_cfg = 0;
	u32 chip_scu_174 = 0, chip_scu_1ec = 0;
	u32 efuse0 = 0, efuse1 = 0;
	enum econet_pkg pkg;

	hir = FIELD_GET(ECONET_NP_SCU_HIR_MASK,
			readl(np_scu + ECONET_NP_SCU_HIR));
	pdidr = FIELD_GET(ECONET_NP_SCU_PDIDR_MASK,
			  readl(np_scu + ECONET_NP_SCU_PDIDR));
	pkgid = econet_pkgid_from_screg(readl(np_scu +
					      ECONET_NP_SCU_SCREG_WR1));

	pkg = econet_pkg_from_id(hir);
	if (!pkg)
		pkg = econet_pkg_from_id(pdidr);

	switch (pkg) {
	case EN751627_PKG:
		if (!chip_scu || !efuse)
			return econet_pkg_family_name(pkg);
		chip_scu_174 = readl(chip_scu + ECONET_CHIP_SCU_751627_PKG);
		efuse0 = readl(efuse + ECONET_EFUSE_VERIFY_DATA0);
		break;
	case EN7526C_PKG:
		if (!chip_scu || !efuse)
			return econet_pkg_family_name(pkg);
		chip_scu_1ec = readl(chip_scu + ECONET_CHIP_SCU_7526C_PKG);
		efuse0 = readl(efuse + ECONET_EFUSE_VERIFY_DATA0);
		efuse1 = readl(efuse + ECONET_EFUSE_VERIFY_DATA1);
		break;
	case EN751221_PKG:
		if (!efuse)
			return econet_pkg_family_name(pkg);
		efuse0 = readl(efuse + ECONET_EFUSE_VERIFY_DATA0);
		break;
	case MT751020_PKG:
		if (!efuse)
			return econet_pkg_family_name(pkg);
		np_scu_cfg = readl(np_scu + ECONET_NP_SCU_MT751020_CFG);
		efuse0 = readl(efuse + ECONET_EFUSE_VERIFY_DATA0);
		break;
	default:
		break;
	}

	return econet_soc_name_from_mips_regs(hir, pkgid, pdidr, np_scu_cfg,
					      chip_scu_174, chip_scu_1ec,
					      efuse0, efuse1);
}

/*
 * Package IDs are family-relative values stored by the bootloader in the
 * NP-SCU watchdog-reset scratch register 1. A value of zero is valid.
 */
static inline u32 get_pkgid(void)
{
	struct regmap *np_scu = airoha_get_scu_regmap();
	u32 value;
	int err;

	err = regmap_read(np_scu, ECONET_NP_SCU_SCREG_WR1, &value);
	if (err)
		return END_PACKAGE_ID;

	return econet_pkgid_from_screg(value);
}

static inline u32 get_pkgid_mem(void __iomem *np_scu)
{
	return econet_pkgid_from_screg(readl(np_scu +
					     ECONET_NP_SCU_SCREG_WR1));
}

/* HIR identifies the SoC family, for example EN7528_PKG (0x0b). */
static inline enum econet_pkg get_pkg(void)
{
	struct regmap *np_scu = airoha_get_scu_regmap();
	u32 value;
	int err;

	err = regmap_read(np_scu, ECONET_NP_SCU_HIR, &value);
	if (err)
		return 0;

	return FIELD_GET(ECONET_NP_SCU_HIR_MASK, value);
}

static inline u32 get_pdidr(void)
{
	struct regmap *np_scu = airoha_get_scu_regmap();
	u32 value;
	int err;

	err = regmap_read(np_scu, ECONET_NP_SCU_PDIDR, &value);
	if (err)
		return 0;

	return FIELD_GET(ECONET_NP_SCU_PDIDR_MASK, value);
}

static inline enum econet_pkg get_pkg_mem(void __iomem *np_scu)
{
	return FIELD_GET(ECONET_NP_SCU_HIR_MASK,
			 readl(np_scu + ECONET_NP_SCU_HIR));
}

static inline u32 get_pdidr_mem(void __iomem *np_scu)
{
	return FIELD_GET(ECONET_NP_SCU_PDIDR_MASK,
			 readl(np_scu + ECONET_NP_SCU_PDIDR));
}

#endif
