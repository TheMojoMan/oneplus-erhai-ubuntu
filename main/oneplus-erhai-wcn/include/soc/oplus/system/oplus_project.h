/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stub for the OnePlus downstream <soc/oplus/system/oplus_project.h>.
 *
 * Real header lives in the OnePlus kernel tree (drivers/soc/oplus/). cnss2's
 * oplus_wifi.c only consumes get_Modem_Version()/get_Operator_Version() to
 * pick a region-specific NV MAC quirk; returning 0 selects the default.
 * Documented deliberate deviation for the literal cnss2 port.
 */
#ifndef _OPLUS_SYSTEM_OPLUS_PROJECT_H
#define _OPLUS_SYSTEM_OPLUS_PROJECT_H

/* OnePlus engineering-build discriminators. cnss2's CNSS_ASSERT() BUG()s only
 * on FACTORY/AGING builds; get_eng_version() returns RELEASE here so asserts
 * downgrade to WARN_ON (correct for a normal Linux boot). */
enum {
	RELEASE = 0,
	PREVERSION,
	CTA,
	FACTORY,
	AGING,
	HIGH_TEMP_AGING,
	AUTO_AGING,
	SATVERSION,
	CLI,
	OEM_RELEASE,
};

static inline int get_eng_version(void)
{
	return RELEASE;
}

static inline int get_Modem_Version(void)
{
	return 0;
}

static inline int get_Operator_Version(void)
{
	return 0;
}

#endif /* _OPLUS_SYSTEM_OPLUS_PROJECT_H */
