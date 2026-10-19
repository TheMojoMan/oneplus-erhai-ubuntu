/*
 *   <author>     <data>      <desc>
 *   LiFenfen   2024/09/09  , add for select BDF by device-tree , bug id 7902090
 */

#include <linux/delay.h>
#include <linux/devcoredump.h>
#include <linux/elf.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <linux/reboot.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/thermal.h>
#include <linux/version.h>

#include <soc/oplus/system/boot_mode.h>
#include <soc/oplus/system/oplus_project.h>

#include "main.h"
#include "debug.h"

#define OPLUS_WIFI_BDF_NODE "oplus_bdf"
#define OPLUS_WIFI_BDF_NAME "bdf_name"
#define OPLUS_WIFI_BDF_TYPE "project_type"

#define OPLUS_WIFI_REGION_NAME "region_name"
#define OPLUS_WIFI_REGION_TYPE "region_type"

#define OPLUS_WIFI_NAME_RFID "rfid"
#define OPLUS_WIFI_NAME_OPERATOR "operator"
#define OPLUS_WIFI_NAME_LENGTH 64

struct wifi_data {
	struct platform_device *plat_dev;
	struct device_node *of_node;
	const char *bdf_name_default;
	const char *bdf_name;
	const char *region_name;
	char project_value[OPLUS_WIFI_NAME_LENGTH];
};

static struct wifi_data *plat_env;

static void oplus_wifi_set_wifi_data(struct wifi_data *wifi_data)
{
	plat_env = wifi_data;
}

struct wifi_data *oplus_wifi_get_wifi_data(void)
{
	return plat_env;
}

static void oplus_wifi_clear_wifi_data(void)
{
	if (plat_env != NULL) {
		devm_kfree(&plat_env->plat_dev->dev, plat_env);
		plat_env = NULL;
	}
}

static void get_project_value(const char *key) {
	int ret;
	size_t length = 0;
	char *buffer;
	struct wifi_data *wifi_data;

	if (key == NULL || strlen(key) == 0)	{
		pr_err("get_project_value key is null\n");
		return;
	}

	wifi_data = oplus_wifi_get_wifi_data();
	if (!wifi_data) {
		return;
	}

	buffer = wifi_data->project_value;
	length = sizeof(wifi_data->project_value) / sizeof(wifi_data->project_value[0]);
	// custom config for different project
	if (strcmp(key, OPLUS_WIFI_NAME_RFID) == 0) {
		ret = get_Modem_Version();
		pr_err("get_project_value key: %s ret: %d\n", key, ret);
		if (ret >= 0) {
			memset(buffer, 0, length);
			ret = snprintf(buffer, length, "%d", ret);
			if (ret >= length) {
				pr_err("get_project_value key: %s ret: %d, value is too long, reset buffer for error!\n", key, ret);
				memset(buffer, 0, length);
			} else {
				pr_err("get_project_value key: %s buffer: %s successfully\n", key, buffer);
			}
		}
		return;
	}

	if (strcmp(key, OPLUS_WIFI_NAME_OPERATOR) == 0) {
		ret = get_Operator_Version();
		pr_err("get_project_value key: %s ret: %d\n", key, ret);
		if (ret >= 0) {
			memset(buffer, 0, length);
			ret = snprintf(buffer, length, "%d", ret);
			if (ret >= length) {
				pr_err("get_project_value key: %s ret: %d, value is too long, reset buffer for error!\n", key, ret);
				memset(buffer, 0, length);
			} else {
				pr_err("get_project_value key: %s buffer: %s successfully\n", key, buffer);
			}
		}
		return;
	}

	return;
}

static const char* get_oplus_wifi_value_of_node(struct device_node *of_node, const char *name) {
	const char *value;
	int ret;

	if (!name || !of_node) {
		pr_err("get_oplus_wifi_value_of_node NULL node or name\n");
		return NULL;
	}

	ret = of_property_read_string(of_node, name, &value);
	if (ret) {
		pr_err("get_oplus_wifi_value_of_node not find property %s ,ret: %d\n", name, ret);
		return NULL;
	} else {
		if (value) {
			pr_err("get_oplus_wifi_value_of_node name %s, value: %s\n", name, value);
			return value;
		}
		return NULL;
	}
	return NULL;
}

static const char* get_dts_info(struct wifi_data *wifi_data, const char* dts_type, const char* dts_name) {
	const char *name;
	const char *project_type;
	struct device_node *child_node;
	char child_node_name[OPLUS_WIFI_NAME_LENGTH];
	struct device_node *node;

	if (!wifi_data) {
		return NULL;
	}

	if (dts_type == NULL || strlen(dts_type) == 0) {
		pr_err("get_dts_info dts_type is null\n");
		return NULL;
	}

	if (dts_name == NULL || strlen(dts_name) == 0) {
		pr_err("get_dts_info dts_name is null\n");
		return NULL;
	}

	node = wifi_data->of_node;
	while (node) {
		pr_err("get_dts_info node %s property %s\n", node->name, dts_type);
		project_type = get_oplus_wifi_value_of_node(node, dts_type);
		if(project_type) {
			get_project_value(project_type);
			pr_err("get_dts_info project_type: %s project_value: %s\n", project_type, wifi_data->project_value);
			if (wifi_data->project_value[0]) {
				/* get next node bdf name of oplus DT Entries */
				memset(child_node_name, 0, sizeof(child_node_name));
				scnprintf(child_node_name, sizeof(child_node_name), "%s_%s", project_type, wifi_data->project_value);
				child_node = of_get_child_by_name(node, child_node_name);
				if (child_node) {
					pr_err("get_dts_info find node %s\n", child_node->name);
					name = get_oplus_wifi_value_of_node(child_node, dts_name);
					if(name) {
						pr_err("get_dts_info find node %s property %s, value: %s\n", child_node->name, dts_name, name);
					} else {
						pr_err("get_dts_info not find node %s property %s, try next node\n", child_node->name, dts_name);
						CNSS_ASSERT(0);
					}
					node = child_node;
					of_node_put(child_node);
				} else {
					pr_err("get_dts_info %s not find chid node %s\n", node->name, child_node_name);
					break;
				}
			} else {
				pr_err("get_dts_info %s read project_type %s without valid result, use default bdf name\n", node->name, project_type);
				break;
			}
		} else {
			pr_err("get_dts_info node %s not find property %s\n", node->name, dts_type);
			break;
		}
	}

	return name;
}

static void oplus_wifi_bdf_init(struct wifi_data *wifi_data) {
	const char *bdf_name;
	struct device_node *child_node;
	struct device *dev = &wifi_data->plat_dev->dev;

	/* get default bdf name of oplus DT Entries */
	child_node = of_get_child_by_name(dev->of_node, OPLUS_WIFI_BDF_NODE);
	if (child_node) {
		pr_info("oplus_wifi_bdf_init find node %s\n", child_node->name);
		wifi_data->of_node = child_node;
		bdf_name = get_oplus_wifi_value_of_node(child_node, OPLUS_WIFI_BDF_NAME);
		if(bdf_name) {
			wifi_data->bdf_name_default = bdf_name;
		}
		of_node_put(child_node);
	} else {
		pr_info("plus_wifi_bdf_init not find node %s\n", OPLUS_WIFI_BDF_NODE);
	}
	return;
}

const char* get_oplus_wifi_region(void) {
	struct wifi_data *wifi_data = oplus_wifi_get_wifi_data();
	if (!wifi_data) {
		return NULL;
	}

	wifi_data->region_name = get_dts_info(wifi_data, OPLUS_WIFI_REGION_TYPE, OPLUS_WIFI_REGION_NAME);
	if (wifi_data->region_name) {
		return wifi_data->region_name;
	}
	return NULL;
}

const char* get_oplus_wifi_bdf(void) {
	struct wifi_data *wifi_data = oplus_wifi_get_wifi_data();
	if (!wifi_data) {
		return NULL;
	}

	wifi_data->bdf_name = get_dts_info(wifi_data, OPLUS_WIFI_BDF_TYPE, OPLUS_WIFI_BDF_NAME);
	if (wifi_data->bdf_name) {
		return wifi_data->bdf_name;
	} else if (wifi_data->bdf_name_default){
		return wifi_data->bdf_name_default;
	}
	return NULL;
}

int oplus_wifi_init(struct platform_device *plat_dev)
{
	int ret = 0;
	struct wifi_data *wifi_data;

	if (oplus_wifi_get_wifi_data()) {
		pr_err("Driver is already initialized!\n");
		ret = -EEXIST;
		goto out;
	}

	wifi_data = devm_kzalloc(&plat_dev->dev, sizeof(*wifi_data),
				 GFP_KERNEL);
	if (!wifi_data) {
		ret = -ENOMEM;
		goto out;
	}

	wifi_data->plat_dev = plat_dev;
	oplus_wifi_bdf_init(wifi_data);
	oplus_wifi_set_wifi_data(wifi_data);
	pr_info("oplus_wifi_init successfully.\n");

	return 0;

out:
	return ret;
}

void oplus_wifi_deinit(void)
{
	oplus_wifi_clear_wifi_data();
}