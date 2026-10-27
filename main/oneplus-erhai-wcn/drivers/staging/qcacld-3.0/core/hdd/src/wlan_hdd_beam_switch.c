/**
 * DOC: wlan_hdd_oplus_beam_switch.c
 *
 * OPLUS in WLAN Host Device Driver beam switch implementation
 *
 */

#include <cds_api.h>
#include "qdf_types.h"
#include "wlan_hdd_main.h"
#include "wlan_hdd_stats.h"

int16_t last_rssi = 0;
uint8_t flag_oplus_monitor_chain_rssi = 0;

int send_chain_rssi_uevent_to_oplus(const char *src) {
	char *envp[3];
	char event_string[300];
	char match_string[] = "OPLUSCUSTOM=/wlan/beam_switch";
	int ret_val = 0;
	qdf_device_t qdf_dev;

	/*send uevent*/
	strlcpy(event_string, src, sizeof(event_string));
	qdf_dev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	envp[0] = (char *)&event_string;
	envp[1] = (char *)&match_string;
	envp[2] = NULL;

	if (qdf_dev && qdf_dev->dev) {
		ret_val = kobject_uevent_env(&(qdf_dev->dev->kobj), KOBJ_CHANGE, envp);
		if (!ret_val) {
			pr_info("wlanhdd:kobject_uevent_env %s\n", event_string);
		} else {
			pr_info("wlanhdd:kobject_uevent_env fail,error=%d!\n", ret_val);
		}
	}
    return ret_val;
}

#ifdef OPLUS_FEATURE_WIFI_BEAM_SWITCH
void set_oplus_chain_rssi_monitor(uint8_t enable_monitor) {
	flag_oplus_monitor_chain_rssi = enable_monitor;
	hdd_debug("set_oplus_chain_rssi_monitor %u ", flag_oplus_monitor_chain_rssi);
}

void send_chain_rssi_to_oplus(int v_dev, int8_t rssi0, int8_t rssi1) {
	if (flag_oplus_monitor_chain_rssi == 0) {
		return;
	}
	char chain_rssi[30] = {'\0'};
	int16_t new_rssi = (rssi1 << 8) | (rssi0 & 0xFF);
	if (v_dev != 0) {
		return;
	}

	hdd_debug("vdev 0 RSSI to oplus for ch_0: %d , ch_1: %d",
				rssi0, rssi1);
	snprintf(chain_rssi, sizeof(chain_rssi), "chainRssi=%d,%d", rssi0, rssi1);
	if (new_rssi != last_rssi) {
		send_chain_rssi_uevent_to_oplus(chain_rssi);
	}
	last_rssi = new_rssi;
}
#endif /* OPLUS_FEATURE_WIFI_BEAM_SWITCH */