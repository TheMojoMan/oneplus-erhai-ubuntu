/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compat stub for the downstream <linux/pinctrl/qcom-pinctrl.h>.
 *
 * cnss2 uses only msm_gpio_mpm_wake_set() — configures a GPIO as an MPM
 * (MSM Power Manager) wake source so the AP can be woken from deep sleep by
 * the WLAN WAKE line. Mainline has no MPM wake-set API; no-op here (the WAKE
 * IRQ itself is still requested via the normal GPIO/IRQ path). Documented
 * deliberate deviation for the literal cnss2 port.
 */
#ifndef _COMPAT_LINUX_PINCTRL_QCOM_PINCTRL_H
#define _COMPAT_LINUX_PINCTRL_QCOM_PINCTRL_H

static inline int msm_gpio_mpm_wake_set(unsigned int gpio, bool enable)
{
	return 0;
}

#endif /* _COMPAT_LINUX_PINCTRL_QCOM_PINCTRL_H */
