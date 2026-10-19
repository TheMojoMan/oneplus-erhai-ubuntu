/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compat reimplementation of of_get_named_gpio(), removed upstream (~v6.15,
 * everything migrated to the gpiod descriptor API).
 *
 * cnss2 uses ONLY of_get_named_gpio() (no OF_GPIO_* flags) and then consumes
 * the returned global GPIO number via the legacy integer API (gpio_request /
 * gpio_get_value / gpio_set_value, still present via <linux/gpio.h>). We
 * resolve the property to a descriptor, read its global number, and release
 * it again — returning the number WITHOUT holding the line, matching the old
 * of_get_named_gpio() semantics so the driver's later gpio_request() succeeds.
 * gpiolib's OF lookup tries both "<con_id>-gpios" and "<con_id>-gpio", so the
 * singular DT property names cnss2 uses (e.g. "wlan-pci-wake-gpio") resolve.
 *
 * NOTE (literal cnss2 port): verify SOL / WAKE / WLAN_EN GPIOs actually toggle
 * on boot — they are load-bearing for the WCN cold-boot-cal preamble.
 */
#ifndef _COMPAT_LINUX_OF_GPIO_H
#define _COMPAT_LINUX_OF_GPIO_H

#include <linux/of.h>
#include <linux/gpio.h>		/* legacy integer GPIO API (gpio_get_value, ...) */
#include <linux/gpio/consumer.h>
#include <linux/err.h>
#include <linux/string.h>

static inline int of_get_named_gpio(const struct device_node *np,
				    const char *propname, int index)
{
	struct gpio_desc *desc;
	char con_id[64];
	size_t len;
	int gpio;

	strscpy(con_id, propname, sizeof(con_id));
	len = strlen(con_id);
	if (len > 6 && !strcmp(con_id + len - 6, "-gpios"))
		con_id[len - 6] = '\0';
	else if (len > 5 && !strcmp(con_id + len - 5, "-gpio"))
		con_id[len - 5] = '\0';

	desc = fwnode_gpiod_get_index(of_fwnode_handle((struct device_node *)np),
				      con_id, index, GPIOD_ASIS, propname);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	gpio = desc_to_gpio(desc);
	gpiod_put(desc);
	return gpio;
}

#endif /* _COMPAT_LINUX_OF_GPIO_H */
