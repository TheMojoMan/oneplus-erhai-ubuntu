/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stub for the OnePlus downstream <soc/oplus/system/boot_mode.h>.
 *
 * The real header lives in the OnePlus *kernel* tree (drivers/soc/oplus/),
 * a different repo than the WLAN platform modules we ported. cnss2 only
 * consumes get_boot_mode() and the MSM_BOOT_MODE__WLAN discriminator (used
 * in main.c to decide NV-MAC vs. WLAN-factory-cal boot). On a normal Linux
 * boot the mode is NORMAL, so the NV-MAC path is taken — matching a stock
 * cold boot. Documented deliberate deviation for the literal cnss2 port.
 */
#ifndef _OPLUS_SYSTEM_BOOT_MODE_H
#define _OPLUS_SYSTEM_BOOT_MODE_H

enum oplus_boot_mode {
	MSM_BOOT_MODE__NORMAL = 0,
	MSM_BOOT_MODE__FASTBOOT,
	MSM_BOOT_MODE__RECOVERY,
	MSM_BOOT_MODE__FACTORY,
	MSM_BOOT_MODE__RF,
	MSM_BOOT_MODE__WLAN,
	MSM_BOOT_MODE__MOS,
	MSM_BOOT_MODE__CHARGE,
	MSM_BOOT_MODE__SILENCE,
	MSM_BOOT_MODE__SAU,
	MSM_BOOT_MODE__AGING,
	MSM_BOOT_MODE__SAFE,
};

static inline int get_boot_mode(void)
{
	return MSM_BOOT_MODE__NORMAL;
}

#endif /* _OPLUS_SYSTEM_BOOT_MODE_H */
